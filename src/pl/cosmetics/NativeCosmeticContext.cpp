/**
 * @file NativeCosmeticContext.cpp
 * @brief Resolves the render-context world anchor at runtime.
 */

#include "pl/cosmetics/NativeCosmeticContext.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <inttypes.h>
#include <string>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/runtime/GameLocalPlayer.h"

namespace pl::cosmetics {
namespace {

struct Region {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
};

bool IsPlausibleProjectionRows(float m0, float m5, float m10, float m11) {
  // Column-major perspective projection: m[0] and m[5] are the 1/tan(fov) focal scales (> 1),
  // m[10] is the negative near/far remap and m[11] is -1.
  if (!std::isfinite(m0) || !std::isfinite(m5) || !std::isfinite(m10) || !std::isfinite(m11)) {
    return false;
  }
  return m0 > 0.9F && m0 < 5.0F && m5 > 0.9F && m5 < 5.0F && m10 < -0.5F && m10 > -2.0F &&
         std::fabs(m11 + 1.0F) < 0.2F;
}

// Walks /proc/self/maps and returns the readable ranges belonging to the module. Self-contained so
// the cosmetic context does not depend on internals of the signature resolver.
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
    if (end <= start || perms[0] != 'r') continue;
    out.push_back(Region{start, end});
  }
  std::fclose(maps);
}

} // namespace

bool ResolveRenderWorldFromPatterns(const char *moduleName, std::uintptr_t &outAddress) {
  outAddress = 0;
  if (moduleName == nullptr || moduleName[0] == '\0') return false;

  std::vector<Region> regions;
  ReadableRegionsFor(moduleName, regions);

  for (const auto &region : regions) {
    const std::size_t count = (region.end - region.start) / sizeof(float);
    if (count < 16) continue;
    const float *base = reinterpret_cast<const float *>(region.start);
    for (std::size_t i = 0; i + 16 <= count; ++i) {
      const float m0 = base[i + 0];
      const float m5 = base[i + 5];
      const float m10 = base[i + 10];
      const float m11 = base[i + 11];
      if (IsPlausibleProjectionRows(m0, m5, m10, m11)) {
        outAddress = region.start + i * sizeof(float);
        preloaderLogger.info("Native cosmetics: plausible camera matrix at {:x}", outAddress);
        return true;
      }
    }
  }
  preloaderLogger.info("Native cosmetics: no camera matrix pattern found; using player anchor");
  return false;
}

Mat4 WorldFromLocalPlayer() {
  float position[3] = {0.0F, 0.0F, 0.0F};
  if (pl::runtime::ReadLocalPlayerPosition(position)) {
    return Mat4::translation(Vec3{position[0], position[1], position[2]});
  }
  return Mat4::identity();
}

} // namespace pl::cosmetics