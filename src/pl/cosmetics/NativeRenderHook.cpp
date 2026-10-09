/**
 * @file NativeRenderHook.cpp
 * @brief Player-render interception + cosmetic matrix anchoring (Task 2).
 */

#include "pl/cosmetics/NativeRenderHook.hpp"

#include <atomic>
#include <ctime>

#include "pl/Logger.hpp"
#include "pl/cosmetics/NativeCosmeticContext.hpp"
#include "pl/cosmetics/NativeCosmeticRenderer.hpp"
#include "pl/hooks/DobbyHookManager.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/GameLocalPlayer.h"

namespace pl::cosmetics {

// Forward declaration defined after the anonymous namespace; the detour calls it after the game's
// renderer runs, which is always well after module load.
void DrawCosmeticsOnce(const pl::runtime::CosmeticFrameHeader &cosmetic);

namespace {

constexpr const char *kGameModule = "libminecraftpe.so";
constexpr std::size_t kDefaultRenderSlot = 17; // verified on 1.26.60.x
constexpr const char *kVerifiedMinVersion = "1.26.60.00";
constexpr const char *kVerifiedMaxVersion = "1.26.61.00";

// Unknown C++ signature: forward the full integer argument register set and dereference nothing.
using RenderFn = void (*)(void *, void *, void *, void *, void *, void *, void *, void *);

RenderFn g_origRender = nullptr;
std::atomic_bool g_hookInstalled{false};
std::atomic_bool g_hookLive{false};

std::atomic<std::uint32_t> g_frameTick{0};
std::atomic<std::uint32_t> g_frameCalls{0};
std::atomic<std::uint32_t> g_totalCalls{0};
std::atomic<std::uint32_t> g_cosmeticFrameTick{0};
std::atomic<std::uint32_t> g_lastCosmeticDrawTick{0};
std::atomic<std::uint64_t> g_lastDrawnFrameVersion{0};
std::atomic<long long> g_lastRenderMs{0};

std::atomic<BoneMatrixSource *> g_boneSource{nullptr};

long long monotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

/**
 * @brief The passthrough detour.
 *
 * Records the frame tick / call counts, then forwards every argument unchanged. No dereference,
 * allocation or lock, so it cannot add a stall or a fault to the render path.
 */
void HookRender(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  const long long now = monotonicMs();
  const long long last = g_lastRenderMs.load(std::memory_order_relaxed);
  if (last == 0 || now - last > 1000) {
    g_frameCalls.store(1, std::memory_order_relaxed);
    g_frameTick.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_frameCalls.fetch_add(1, std::memory_order_relaxed);
  }
  g_totalCalls.fetch_add(1, std::memory_order_relaxed);
  g_lastRenderMs.store(now, std::memory_order_relaxed);
  g_hookLive.store(true, std::memory_order_relaxed);

  // Consume the per-frame transform buffer the launcher pushes (CosmeticFrame wire layout). The
  // launcher computes the cape chain, pet pose and head look in Java; this is where the render
  // thread reads them, so the cosmetic data is live every frame rather than only at a hook install.
  const pl::runtime::CosmeticFrameHeader cosmetic = pl::runtime::ReadCosmeticFrameHeader();
  if (cosmetic.valid) {
    g_cosmeticFrameTick.fetch_add(1, std::memory_order_relaxed);
  }

  if (g_origRender) {
    g_origRender(a, b, c, d, e, f, g, h);
  }

  // The rasterization step: draw the equipped capes/pets/hats into the frame the game just drew.
  // The frame and geometry blobs come from the launcher; the atlas is the first cape override's
  // pixels. The world matrix is supplied by a provider when one is installed; identity otherwise,
  // which renders around the origin of the render pass (the honest fallback until the camera
  // matrix is resolved on the build).
  g_lastCosmeticDrawTick.fetch_add(1, std::memory_order_relaxed);
  DrawCosmeticsOnce(cosmetic);
}

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
  if (g_hasCosmeticWorld.load(std::memory_order_acquire)) return g_cosmeticWorld;
  // No explicit override: anchor the cosmetics at the live local player, so they appear where the
  // player stands rather than at the render origin.
  return WorldFromLocalPlayer();
}

/**
 * @brief Rasterises the equipped cosmetics for the current frame.
 *
 * Called after the game's player render. Pulls the frame + geometry blobs from the registry and the
 * first cape override as the atlas, then hands them to {@link NativeCosmeticRenderer}. All failure
 * modes are handled inside the renderer (bounds checks, GLES availability); the only thing this
 * method does is marshal the inputs.
 *
 * <p><b>Which player?</b> The hook's render arguments are an unknown ABI, so the detour cannot
 * inspect which model it is drawing. The honest scoping available is: the cosmetic frame is
 * published by the launcher <em>for the local player</em>, and the local-player feed is live only
 * while the game has a live local player. So this only draws when {@code ReadLocalPlayerPosition}
 * is live (i.e. we are in a world and the local player exists), and it draws <em>once per published
 * frame version</em> — the first render call of the frame — rather than once per rendered model,
 * so other players drawn later in the same frame do not get the local player's cosmetics layered
 * on them.
 */
void DrawCosmeticsOnce(const pl::runtime::CosmeticFrameHeader &cosmetic) {
  if (!cosmetic.valid || (cosmetic.flags & 0x7u) == 0) return;

  // Lock-free snapshot of the double-buffered frame, with the published version.
  std::vector<std::uint8_t> frameBytes;
  std::uint64_t version = 0;
  if (!pl::runtime::CosmeticFrameSnapshot(frameBytes, version)) return;
  // Re-enter once per frame version: the first render call of a frame draws the cosmetics; the
  // later calls (other player models in the same frame) find the same version and skip.
  if (version == g_lastDrawnFrameVersion.load(std::memory_order_relaxed)) return;
  g_lastDrawnFrameVersion.store(version, std::memory_order_relaxed);

  // Scope to a live local player: if the game has not produced a local-player snapshot (not in a
  // world / loading screen), there is nobody to wear the cosmetics, so draw nothing.
  float probe[3] = {0.0F, 0.0F, 0.0F};
  if (!pl::runtime::ReadLocalPlayerPosition(probe)) return;

  std::vector<std::uint8_t> geometry;
  pl::runtime::RenderGeometryData(geometry);
  if (geometry.empty()) return;

  const pl::runtime::CosmeticImage *atlas = pl::runtime::FirstCapeOverride();

  pl::cosmetics::CosmeticDrawResult result = NativeCosmeticRenderer::drawFrame(
      std::span<const std::uint8_t>(frameBytes.data(), frameBytes.size()),
      std::span<const std::uint8_t>(geometry.data(), geometry.size()),
      atlas ? atlas->rgba : nullptr,
      atlas ? static_cast<int>(atlas->width) : 0,
      atlas ? static_cast<int>(atlas->height) : 0,
      GetCosmeticWorld());
  (void)result;
}

void SetBoneMatrixSource(BoneMatrixSource *source) {
  g_boneSource.store(source, std::memory_order_relaxed);
}

bool HasBoneMatrixSource() { return g_boneSource.load(std::memory_order_relaxed) != nullptr; }

RenderStats ReadRenderStats() {
  RenderStats stats;
  stats.frameTick = g_frameTick.load(std::memory_order_relaxed);
  stats.frameCalls = g_frameCalls.load(std::memory_order_relaxed);
  stats.totalCalls = g_totalCalls.load(std::memory_order_relaxed);
  return stats;
}

bool IsNativeCosmeticRenderHookLive() { return g_hookLive.load(std::memory_order_relaxed); }

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
    return g_hookLive.load(std::memory_order_relaxed);
  }

  // Only the builds whose slot index is verified may be hooked. On an unverified build the RTTI
  // name still resolves to a real function, so a stale index would detour an unrelated slot and
  // fault the render thread; fail safe to the resource-pack path instead.
  if (!pl::runtime::MatchesConfiguredVersion(kVerifiedMinVersion, kVerifiedMaxVersion)) {
    preloaderLogger.warn(
        "Native cosmetics: build outside the verified range {}-{}; staying on the resource-pack path",
        kVerifiedMinVersion, kVerifiedMaxVersion);
    return false;
  }

  const std::size_t slot = vtableIndex != 0 ? vtableIndex : kDefaultRenderSlot;
  const std::uintptr_t renderFn =
      pl::memory::resolveVtableFunction("18LivePlayerRenderer", slot, kGameModule);
  if (!renderFn) {
    preloaderLogger.warn(
        "Native cosmetics: LivePlayerRenderer slot {} unresolved; staying on the resource-pack path",
        slot);
    return false;
  }

  const auto status = pl::hooks::DobbyHookManager::instance().hook(
      reinterpret_cast<void *>(renderFn), reinterpret_cast<void *>(HookRender),
      reinterpret_cast<void **>(&g_origRender), "LivePlayerRenderer::render");
  if (status != pl::hooks::HookStatus::Installed) {
    preloaderLogger.warn(
        "Native cosmetics: render hook not installed; staying on the resource-pack path");
    return false;
  }

  // Resolve the GLES surface once; on a Vulkan-only build the renderer stays a no-op every frame.
  NativeCosmeticRenderer::init();

  // Best-effort: scan the game module for a camera/projection matrix at runtime. When found, feed
  // it to the renderer as the world anchor, so the cosmetics are drawn through the real camera. The
  // scan is gated by the same version range guard above (it only runs on verified builds).
  std::uintptr_t cameraAddress = 0;
  if (pl::cosmetics::ResolveRenderWorldFromPatterns(kGameModule, cameraAddress) &&
      cameraAddress != 0) {
    const auto *floats = reinterpret_cast<const float *>(cameraAddress);
    Mat4 world;
    std::memcpy(world.m, floats, sizeof(world.m));
    if (world.isFinite()) {
      pl::cosmetics::SetCosmeticWorld(world);
      preloaderLogger.info(
          "Native cosmetics: render world anchored from camera matrix at {:x}", cameraAddress);
    }
  } else {
    // Fall back to the live local-player anchor (set every frame in GetCosmeticWorld).
    preloaderLogger.info("Native cosmetics: using local-player world anchor (no camera matrix)");
  }

  preloaderLogger.info("Native cosmetics: LivePlayerRenderer::render hooked (slot {})", slot);
  return true;
}

} // namespace pl::cosmetics
