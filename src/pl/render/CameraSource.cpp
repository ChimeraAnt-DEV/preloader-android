/**
 * @file CameraSource.cpp
 * @brief One-time camera matrix resolution + cheap per-frame read.
 */

#include "pl/render/CameraSource.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <mutex>
#include <string>
#include <vector>

#include "pl/Logger.hpp"

namespace pl::render {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

struct Region {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
};

// The resolved camera block: a view matrix immediately followed by a projection matrix, in the
// game's writable data. 0 when nothing was found.
std::atomic<std::uintptr_t> g_viewAddress{0};
std::atomic<std::uintptr_t> g_projectionAddress{0};
std::once_flag g_scanOnce;

void ReadableRegionsFor(const std::string &moduleName, std::vector<Region> &out) {
  std::FILE *maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return;
  char line[4096];
  while (std::fgets(line, sizeof(line), maps)) {
    if (std::strstr(line, moduleName.c_str()) == nullptr) continue;
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    char perms[5] = {};
    if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, perms) != 3) continue;
    if (end <= start) continue;
    // The camera lives in writable data (.data / .bss), not code. Scanning r-xp as well would
    // only add false positives from constant pools.
    if (perms[0] != 'r' || perms[1] != 'w') continue;
    out.push_back(Region{start, end});
  }
  std::fclose(maps);
}

bool Finite(float v) { return std::isfinite(v); }

} // namespace

bool CameraSource::ValidateProjection(const float *m) {
  // Column-major perspective: m[0] and m[5] are the focal scales (>1 for a normal FOV), m[10] is
  // the negative near/far remap and m[11] is -1.
  const float m0 = m[0], m5 = m[5], m10 = m[10], m11 = m[11];
  if (!Finite(m0) || !Finite(m5) || !Finite(m10) || !Finite(m11)) return false;
  if (!(m0 > 0.5F && m0 < 6.0F)) return false;
  if (!(m5 > 0.5F && m5 < 6.0F)) return false;
  if (!(m10 < -0.5F && m10 > -2.0F)) return false;
  if (std::fabs(m11 + 1.0F) > 0.25F) return false;
  // Off-diagonal entries of a symmetric perspective are zero.
  if (std::fabs(m[1]) > 0.05F || std::fabs(m[4]) > 0.05F) return false;
  return true;
}

bool CameraSource::ValidateView(const float *m) {
  // A view matrix is a rigid transform: the bottom row is (0,0,0,1) and the upper-left 3x3 is
  // orthonormal with determinant +1. Checking length of the basis vectors is enough to reject
  // the random data that otherwise passes a loose check.
  for (int i = 0; i < 16; ++i) {
    if (!Finite(m[i])) return false;
  }
  if (std::fabs(m[3]) > 0.05F || std::fabs(m[7]) > 0.05F || std::fabs(m[11]) > 0.05F) return false;
  if (std::fabs(m[15] - 1.0F) > 0.05F) return false;

  const float cx = m[0], cy = m[1], cz = m[2];
  const float ux = m[4], uy = m[5], uz = m[6];
  const float fx = m[8], fy = m[9], fz = m[10];
  auto length = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
  const float lc = length(cx, cy, cz);
  const float lu = length(ux, uy, uz);
  const float lf = length(fx, fy, fz);
  if (lc < 0.5F || lc > 2.0F) return false;
  if (lu < 0.5F || lu > 2.0F) return false;
  if (lf < 0.5F || lf > 2.0F) return false;
  // Orthogonality of the basis.
  if (std::fabs(cx * ux + cy * uy + cz * uz) > 0.1F) return false;
  if (std::fabs(cx * fx + cy * fy + cz * fz) > 0.1F) return false;
  if (std::fabs(ux * fx + uy * fy + uz * fz) > 0.1F) return false;
  return true;
}

void CameraSource::ResolveOnce() {
  std::call_once(g_scanOnce, [] {
    std::vector<Region> regions;
    ReadableRegionsFor(kGameModule, regions);
    if (regions.empty()) {
      preloaderLogger.info("Camera source: no writable game regions to scan");
      return;
    }

    for (const Region &region : regions) {
      const std::size_t count = (region.end - region.start) / sizeof(float);
      if (count < 32) continue;
      const float *base = reinterpret_cast<const float *>(region.start);
      // Look for a projection matrix whose immediately-preceding 16 floats are a view matrix.
      for (std::size_t i = 16; i + 16 <= count; ++i) {
        const float *candidateProjection = base + i;
        if (!ValidateProjection(candidateProjection)) continue;
        const float *candidateView = base + i - 16;
        if (!ValidateView(candidateView)) continue;

        const std::uintptr_t viewAddr =
            region.start + (i - 16) * sizeof(float);
        const std::uintptr_t projAddr = region.start + i * sizeof(float);
        g_viewAddress.store(viewAddr, std::memory_order_release);
        g_projectionAddress.store(projAddr, std::memory_order_release);
        preloaderLogger.info("Camera source: resolved view {:x} projection {:x}", viewAddr,
                             projAddr);
        return;
      }
    }
    preloaderLogger.info(
        "Camera source: no view/projection pair found; callers fall back to ComputeViewProjection");
  });
}

bool CameraSource::HasResolvedAddress() {
  return g_viewAddress.load(std::memory_order_acquire) != 0 &&
         g_projectionAddress.load(std::memory_order_acquire) != 0;
}

CameraSource::Frame CameraSource::ReadFrame() {
  Frame frame;
  const std::uintptr_t viewAddr = g_viewAddress.load(std::memory_order_acquire);
  const std::uintptr_t projAddr = g_projectionAddress.load(std::memory_order_acquire);
  if (viewAddr == 0 || projAddr == 0) return frame;

  const float *view = reinterpret_cast<const float *>(viewAddr);
  const float *projection = reinterpret_cast<const float *>(projAddr);
  // Re-validate every frame: the cached address could have been freed or repurposed if the
  // renderer was torn down, and a stale read must not become a wrong matrix.
  if (!ValidateView(view) || !ValidateProjection(projection)) return frame;

  std::memcpy(frame.view.m, view, sizeof(float) * 16);
  std::memcpy(frame.projection.m, projection, sizeof(float) * 16);
  frame.hasView = true;
  frame.hasProjection = true;
  frame.viewProjection = pl::cosmetics::Mat4::multiply(frame.projection, frame.view);
  frame.hasViewProjection = frame.viewProjection.isFinite();
  return frame;
}

} // namespace pl::render
