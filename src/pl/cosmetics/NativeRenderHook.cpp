/**
 * @file NativeRenderHook.cpp
 * @brief Player-render interception + cosmetic matrix anchoring (Task 2).
 */

#include "pl/cosmetics/NativeRenderHook.hpp"

#include <atomic>
#include <ctime>

#include "pl/Logger.hpp"
#include "pl/hooks/DobbyHookManager.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameHookRules.h"

namespace pl::cosmetics {
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
}

} // namespace

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

  preloaderLogger.info("Native cosmetics: LivePlayerRenderer::render hooked (slot {})", slot);
  return true;
}

} // namespace pl::cosmetics
