/**
 * @file NativeRenderHook.cpp
 * @brief Player-render interception + cosmetic matrix anchoring (Task 2).
 */

#include "pl/cosmetics/NativeRenderHook.hpp"

#include <atomic>
#include <ctime>

#include "pl/Logger.hpp"
#include "pl/Render.hpp"
#include "pl/cosmetics/NativeCosmeticContext.hpp"
#include "pl/hooks/DobbyHookManager.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/render/CameraSource.h"
#include "pl/render/RenderDispatch.h"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/GameLocalPlayer.h"
#include "pl/runtime/GamePlayerRender.h"

namespace pl::cosmetics {

// Forward declaration defined after the anonymous namespace; the post-render callback.
void OnPlayerRenderFrame();

namespace {

constexpr const char *kGameModule = "libminecraftpe.so";
constexpr std::size_t kDefaultRenderSlot = 17; // verified on 1.26.60.x
// A floor, not a narrow window (see GamePlayerRender.cpp for the rationale). Empty max = unbounded.
constexpr const char *kVerifiedMinVersion = "1.21.132";
constexpr const char *kVerifiedMaxVersion = "";

std::atomic_bool g_hookInstalled{false};
std::atomic_bool g_callbackLive{false};

// Whether the anchor for the last frame came from the engine's view-projection (true) or the
// first-person fallback (false). A device pass/fail test must be able to tell these apart.
std::atomic_bool g_lastAnchorFromEngine{false};
std::atomic_bool g_anchorKnown{false};

std::atomic<BoneMatrixSource *> g_boneSource{nullptr};

} // namespace

// The cosmetic world anchor. Guarded by an atomic flag; a non-finite matrix clears it.
std::atomic<bool> g_hasCosmeticWorld{false};
Mat4 g_cosmeticWorld;

void SetCosmeticWorld(const Mat4 &world) {
  g_cosmeticWorld = world;
  if (world.isFinite()) {
    g_hasCosmeticWorld.store(true, std::memory_order_release);
  } else {
    g_hasCosmeticWorld.store(false, std::memory_order_release);
  }
}

Mat4 GetCosmeticWorld() {
  // 1. An explicit override wins (a mod that resolved its own camera).
  if (g_hasCosmeticWorld.load(std::memory_order_acquire)) {
    g_lastAnchorFromEngine.store(true, std::memory_order_relaxed);
    g_anchorKnown.store(true, std::memory_order_relaxed);
    return g_cosmeticWorld;
  }

  // The player's world position is where the cosmetics are anchored. Without it there is nowhere
  // sensible to draw, so bail to a plain translation (still better than the origin).
  float position[3] = {0.0F, 0.0F, 0.0F};
  const bool hasPosition = pl::runtime::ReadLocalPlayerPosition(position);
  const Mat4 toPlayer = hasPosition
      ? Mat4::translation(Vec3{position[0], position[1], position[2]})
      : Mat4::identity();

  // 2. The real camera view-projection, resolved once and revalidated per frame. This is the
  //    correct anchor: it places the cosmetics under the game's own projection and view.
  const pl::render::CameraSource::Frame camera = pl::render::CameraSource::ReadFrame();
  if (camera.hasViewProjection) {
    g_lastAnchorFromEngine.store(true, std::memory_order_relaxed);
    g_anchorKnown.store(true, std::memory_order_relaxed);
    return Mat4::multiply(camera.viewProjection, toPlayer);
  }

  // 3. Fall back to a real first-person view-projection built from the live local player pose. This
  //    at least orients and projects the cosmetics correctly, unlike the old position-only
  //    translation which put everything off-screen.
  float rotation[2] = {0.0F, 0.0F};
  if (hasPosition && pl::runtime::ReadLocalPlayerRotation(rotation)) {
    // The viewport aspect is unknown here; 16:9 is a reasonable default and only affects the
    // horizontal field, not placement.
    constexpr float kFov = 70.0F;
    constexpr float kAspect = 16.0F / 9.0F;
    Mat4 viewProj;
    if (pl::render::ComputeViewProjection(kFov, kAspect, 0.05F, 512.0F, viewProj)) {
      // The engine camera did not resolve; the anchor is the first-person fallback. Recorded so a
      // device pass/fail test cannot silently accept the fallback as if it were the engine camera.
      g_lastAnchorFromEngine.store(false, std::memory_order_relaxed);
      g_anchorKnown.store(true, std::memory_order_relaxed);
      return Mat4::multiply(viewProj, toPlayer);
    }
  }

  // 4. Last resort: a plain translation to the player position (no view/projection). The renderer
  //    still draws, but placement is approximate.
  g_lastAnchorFromEngine.store(false, std::memory_order_relaxed);
  g_anchorKnown.store(true, std::memory_order_relaxed);
  return toPlayer;
}

/**
 * @brief Records that the player-render seam fired this frame.
 *
 * Called after the game's player render. The cosmetic meshes themselves are no longer rasterised
 * here: a cosmetic reaches the game through the engine's own texture/image pipeline (the cape
 * texture swap), which does not need a GL draw and therefore works on a Vulkan-only build. All this
 * callback does is mark the seam live so the launcher can confirm the hook is installed and the
 * renderer has run.
 */
void OnPlayerRenderFrame() {
  // The callback only runs because the render hook is installed and the game rendered a frame, so
  // its invocation is the honest "the render seam is live" signal.
  g_callbackLive.store(true, std::memory_order_relaxed);
}

void SetBoneMatrixSource(BoneMatrixSource *source) {
  g_boneSource.store(source, std::memory_order_relaxed);
}

bool HasBoneMatrixSource() { return g_boneSource.load(std::memory_order_relaxed) != nullptr; }

RenderStats ReadRenderStats() {
  // Delegate to the single player-render feed, so the two cannot report different frame counts.
  std::uint32_t out[4] = {0, 0, 0, 0};
  RenderStats stats;
  if (pl::runtime::ReadPlayerRenderStats(out)) {
    stats.frameTick = out[0];
    stats.frameCalls = out[1];
    stats.totalCalls = out[2];
  }
  return stats;
}

bool IsNativeCosmeticRenderHookLive() { return g_callbackLive.load(std::memory_order_relaxed); }

bool LastCameraAnchorFromEngine() {
  return g_anchorKnown.load(std::memory_order_relaxed)
      && g_lastAnchorFromEngine.load(std::memory_order_relaxed);
}

bool BuildCosmeticMatrix(std::string_view bone, const Mat4 &cosmeticOffset, Mat4 &out) {
  BoneMatrixSource *source = g_boneSource.load(std::memory_order_relaxed);
  if (source == nullptr) return false;
  Mat4 boneMatrix;
  if (!source->readBoneMatrix(bone, boneMatrix)) return false;
  const Mat4 world = Mat4::identity();
  out = composeCosmeticMatrix(world, boneMatrix, cosmeticOffset);
  return out.isFinite();
}

bool InitNativeCosmeticRenderHook(std::size_t vtableIndex) {
  if (g_hookInstalled.exchange(true, std::memory_order_relaxed)) {
    return g_callbackLive.load(std::memory_order_relaxed);
  }

  // Only the builds whose slot index is verified may be hooked. On an unverified build the RTTI
  // name still resolves to a real function, so a stale index would detour an unrelated slot and
  // fault the render thread; fail safe to the resource-pack path instead.
  if (!pl::runtime::MatchesConfiguredVersion(kVerifiedMinVersion, kVerifiedMaxVersion)) {
    preloaderLogger.warn(
        "Native cosmetics: build outside the verified range {}-{}; not hooking the render seam",
        kVerifiedMinVersion, kVerifiedMaxVersion);
    return false;
  }

  // Confirm the render entry resolves on this build (by string, so the slot index does not matter).
  // A resolution failure means the whole version gate was too optimistic; keep the pack path.
  std::uintptr_t renderCheck = pl::memory::resolveVtableSlotReferencingString(
      "18LivePlayerRenderer", "variable.player_x_rotation", kGameModule, 200);
  if (!renderCheck && vtableIndex != 0) {
    renderCheck = pl::memory::resolveVtableFunction("18LivePlayerRenderer", vtableIndex,
                                                    kGameModule);
  }
  if (!renderCheck) {
    preloaderLogger.warn(
        "Native cosmetics: LivePlayerRenderer render entry unresolved; keeping the pack path");
    return false;
  }

  // Register into the single player-render hook rather than installing a second detour on the same
  // slot. Two detours on one function duplicated the frame accounting and left ownership ambiguous;
  // the feed owns the hook and runs this callback once per frame after the game's render.
  pl::runtime::RegisterPostRenderCallback(&OnPlayerRenderFrame);

  // Best-effort: resolve the real view-projection matrix once, off the render thread. The scan is
  // cached in CameraSource and revalidated every frame cheaply; when it is unavailable the anchor
  // falls back to the live local-player pose (position + rotation), which keeps the anchor on the
  // player rather than at the render origin.
  pl::render::CameraSource::ResolveOnce();
  if (pl::render::CameraSource::HasResolvedAddress()) {
    preloaderLogger.info(
        "Native cosmetics: camera matrix resolved; anchoring via real view-projection");
  } else {
    preloaderLogger.info("Native cosmetics: no camera matrix; anchoring via local-player pose");
  }

  preloaderLogger.info("Native cosmetics: registered post-render callback");
  return true;
}

} // namespace pl::cosmetics
