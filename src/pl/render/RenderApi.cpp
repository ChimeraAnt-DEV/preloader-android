/**
 * @file RenderApi.cpp
 * @brief Public per-frame render callback + camera matrix API (pl/Render.hpp).
 *
 * The render seam itself is the existing player-render detour: it already runs on the render thread
 * with the GL context current. This file keeps the callback registry and the camera resolution that
 * the seam dispatches to, and exposes the fallback camera math.
 */

#include "pl/Render.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/render/CameraSource.h"
#include "pl/render/GlesProbe.hpp"
#include "pl/runtime/GameLocalPlayer.h"

namespace pl::render {
namespace {

std::mutex g_callbackMutex;
std::vector<RenderCallback> g_callbacks;

/// Reads the current GL viewport, or {0,0} when the GLES surface is unavailable. Cached table.
void QueryViewport(int &width, int &height) {
  width = 0;
  height = 0;
  const GlesFunctions gles = ResolveGles();
  if (!gles.GetIntegerv) return;
  using GetIntegervFn = void (*)(std::uint32_t, std::int32_t *);
  auto getIntegerv = reinterpret_cast<GetIntegervFn>(gles.GetIntegerv);
  std::int32_t viewport[4] = {0, 0, 0, 0};
  getIntegerv(0x0BA2 /* GL_VIEWPORT */, viewport);
  width = viewport[2];
  height = viewport[3];
}

std::atomic<std::uint64_t> g_frameTick{0};
std::atomic<bool> g_live{false};

/// Set once so the (expensive) camera scan runs off the render thread at first registration.
std::atomic<bool> g_cameraResolveRequested{false};

} // namespace

bool RegisterRenderCallback(RenderCallback callback) {
  if (!callback) return false;
  {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_callbacks.push_back(std::move(callback));
  }
  // Resolve the real camera once, lazily. Do it here rather than in the render path: the scan walks
  // the game's whole writable data and must never run on the render thread.
  if (!g_cameraResolveRequested.exchange(true, std::memory_order_acq_rel)) {
    CameraSource::ResolveOnce();
  }
  return true;
}

std::size_t UnregisterRenderCallbacks(bool all) {
  std::lock_guard<std::mutex> lock(g_callbackMutex);
  if (all) {
    const std::size_t removed = g_callbacks.size();
    g_callbacks.clear();
    return removed;
  }
  if (g_callbacks.empty()) return 0;
  g_callbacks.pop_back();
  return 1;
}

bool IsRenderCallbackLive() { return g_live.load(std::memory_order_acquire); }

std::uint64_t RenderFrameTick() { return g_frameTick.load(std::memory_order_acquire); }

// --- fallback camera math --------------------------------------------------------------

bool MakePerspective(float fovDegrees, float aspectRatio, float nearPlane, float farPlane,
                     Mat4 &out) {
  if (!(fovDegrees > 1.0F && fovDegrees < 179.0F)) return false;
  if (!(aspectRatio > 0.0F)) return false;
  if (!(nearPlane > 0.0F) || !(farPlane > nearPlane)) return false;

  const float radians = fovDegrees * static_cast<float>(std::numbers::pi) / 180.0F;
  const float f = 1.0F / std::tan(radians * 0.5F);
  out = Mat4{};
  out.m[0] = f / aspectRatio;
  out.m[5] = f;
  out.m[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
  out.m[11] = -1.0F;
  out.m[14] = (2.0F * farPlane * nearPlane) / (nearPlane - farPlane);
  out.m[15] = 0.0F;
  return true;
}

bool MakeView(const Vec3 &eye, const Vec3 &forward, const Vec3 &up, Mat4 &out) {
  auto normalize = [](const Vec3 &v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length <= 1e-6F) return Vec3{0.0F, 0.0F, 0.0F};
    return Vec3{v.x / length, v.y / length, v.z / length};
  };
  auto cross = [](const Vec3 &a, const Vec3 &b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  auto dot = [](const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };

  const Vec3 f = normalize(forward);
  if (f.x == 0.0F && f.y == 0.0F && f.z == 0.0F) return false;
  // Right = forward x up, then re-orthogonalise up against forward.
  const Vec3 s = normalize(cross(f, up));
  if (s.x == 0.0F && s.y == 0.0F && s.z == 0.0F) return false;
  const Vec3 u = cross(s, f);

  // Column-major right-handed look-at: rows are the basis, translation is -basis*eye.
  out = Mat4{};
  out.m[0] = s.x;  out.m[4] = s.y;  out.m[8]  = s.z;
  out.m[1] = u.x;  out.m[5] = u.y;  out.m[9]  = u.z;
  out.m[2] = -f.x; out.m[6] = -f.y; out.m[10] = -f.z;
  out.m[12] = -dot(s, eye);
  out.m[13] = -dot(u, eye);
  out.m[14] = dot(f, eye);
  out.m[15] = 1.0F;
  return true;
}

bool ReadLocalPlayerPosition(float out[3]) {
  return pl::runtime::ReadLocalPlayerPosition(out);
}

bool ReadLocalPlayerRotation(float out[2]) {
  return pl::runtime::ReadLocalPlayerRotation(out);
}

bool ComputeViewProjection(float fovDegrees, float aspectRatio, float nearPlane, float farPlane,
                           Mat4 &out) {
  float position[3] = {0.0F, 0.0F, 0.0F};
  float rotation[2] = {0.0F, 0.0F};
  if (!pl::runtime::ReadLocalPlayerPosition(position)) return false;
  if (!pl::runtime::ReadLocalPlayerRotation(rotation)) return false;

  // Bedrock reports yaw/pitch in degrees. Yaw is measured clockwise from -Z (the usual Minecraft
  // convention), so forward = (-sin(yaw), -sin(pitch)... ) with pitch positive looking down.
  const float yaw = rotation[0] * static_cast<float>(std::numbers::pi) / 180.0F;
  const float pitch = rotation[1] * static_cast<float>(std::numbers::pi) / 180.0F;
  const float cosPitch = std::cos(pitch);
  const Vec3 forward{
      -std::sin(yaw) * cosPitch,
      -std::sin(pitch),
      -std::cos(yaw) * cosPitch,
  };
  // The eye sits at the player's feet plus eye height (~1.62 blocks), the same offset Bedrock uses.
  const Vec3 eye{position[0], position[1] + 1.62F, position[2]};

  Mat4 view;
  if (!MakeView(eye, forward, Vec3{0.0F, 1.0F, 0.0F}, view)) return false;
  Mat4 projection;
  if (!MakePerspective(fovDegrees, aspectRatio, nearPlane, farPlane, projection)) return false;

  out = Mat4::multiply(projection, view);
  return out.isFinite();
}

// --- dispatch (called from the render seam) --------------------------------------------

namespace detail {

/**
 * @brief Builds the context and invokes every registered callback for this frame.
 *
 * Called from the render detour after the game's own draw, on the render thread. The callback list
 * is copied under the lock and then invoked outside it, so a callback that (un)registers another
 * callback cannot deadlock and a slow callback cannot block registration.
 */
void DispatchRenderCallbacks() {
  std::vector<RenderCallback> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_callbacks.empty()) return;
    snapshot = g_callbacks;
  }

  RenderContext context;
  context.frameTick = g_frameTick.fetch_add(1, std::memory_order_acq_rel) + 1;
  QueryViewport(context.viewportWidth, context.viewportHeight);

  const CameraSource::Frame camera = CameraSource::ReadFrame();
  if (camera.hasViewProjection) {
    context.viewProjection = camera.viewProjection;
    context.hasViewProjection = true;
  }
  if (camera.hasView) {
    context.view = camera.view;
    context.hasView = true;
  }
  if (camera.hasProjection) {
    context.projection = camera.projection;
    context.hasProjection = true;
  }
  // The camera eye is the inverse-translation of the view matrix: -R^T * t.
  if (camera.hasView) {
    const Mat4 &v = camera.view;
    context.cameraPosition = Vec3{
        -(v.m[0] * v.m[12] + v.m[1] * v.m[13] + v.m[2] * v.m[14]),
        -(v.m[4] * v.m[12] + v.m[5] * v.m[13] + v.m[6] * v.m[14]),
        -(v.m[8] * v.m[12] + v.m[9] * v.m[13] + v.m[10] * v.m[14]),
    };
    context.hasCameraPosition = true;
    context.cameraForward = Vec3{-v.m[2], -v.m[6], -v.m[10]};
    context.hasCameraForward = true;
  }

  g_live.store(true, std::memory_order_release);

  for (const RenderCallback &callback : snapshot) {
    // A callback must never take the process down: a throw is swallowed and the frame continues.
    try {
      callback(context);
    } catch (...) {
      preloaderLogger.warn("Render callback threw; frame continues");
    }
  }
}

} // namespace detail

} // namespace pl::render
