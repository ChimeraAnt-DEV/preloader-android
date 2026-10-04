#include "pl/runtime/GamePlayerRender.h"

#include <atomic>
#include <cstdint>
#include <ctime>

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Vtable.hpp"

namespace pl::runtime {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

// LivePlayerRenderer's render entry. Slot 17 was confirmed against the shipped
// 1.26.60.28 binary (see docs/player-render-hook.md): it is the only slot whose body
// references variable.player_x_rotation / variable.is_first_person / variable.is_using_vr,
// i.e. the routine that poses and draws the player model. The index is overridable from
// the per-version signature rules; the RTTI name resolution means no code address is baked in.
constexpr std::size_t kDefaultRenderSlot = 17;

// Passthrough hook arity. The exact C++ signature of the render entry is not known (the
// game strips its symbols), so the detour forwards the full integer argument register set
// and never inspects any argument. That is what keeps an unknown ABI safe: nothing is
// dereferenced, so a wrong guess cannot read freed memory or mis-handle an object.
using RenderFn = void (*)(void *, void *, void *, void *, void *, void *, void *, void *);

RenderFn g_origRender = nullptr;
std::atomic_bool g_hookInstalled{false};

std::atomic<std::uint32_t> g_renderTick{0};   // increments once per frame (first call)
std::atomic<std::uint32_t> g_frameCalls{0};   // calls seen in the current frame
std::atomic<std::uint32_t> g_totalCalls{0};   // lifetime call count
std::atomic<long long> g_lastRenderMs{0};

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

// The detour. It runs on the render thread, once per rendered player model. It records the
// tick and call counts and immediately forwards every argument unchanged; it performs no
// dereference, allocation, or lock, so it cannot add a stall or a fault to the render path.
void HookRender(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  const long long now = MonotonicMs();
  const long long last = g_lastRenderMs.load(std::memory_order_relaxed);
  // A gap larger than a plausible frame (~1s) starts a new frame, so the per-frame call
  // count reflects how many player models were drawn in this frame -- more than one is the
  // renderer covering non-local players too.
  if (last == 0 || now - last > 1000) {
    g_frameCalls.store(1, std::memory_order_relaxed);
    g_renderTick.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_frameCalls.fetch_add(1, std::memory_order_relaxed);
  }
  g_totalCalls.fetch_add(1, std::memory_order_relaxed);
  g_lastRenderMs.store(now, std::memory_order_relaxed);

  if (g_origRender) {
    g_origRender(a, b, c, d, e, f, g, h);
  }
}

} // namespace

void InitPlayerRenderSource(std::size_t vtableIndex) {
  if (g_hookInstalled.exchange(true, std::memory_order_relaxed)) {
    return;
  }

  const std::size_t slot = vtableIndex != 0 ? vtableIndex : kDefaultRenderSlot;
  const std::uintptr_t renderFn =
      pl::memory::resolveVtableFunction("18LivePlayerRenderer", slot, kGameModule);
  if (!renderFn) {
    preloaderLogger.warn(
        "Player-render feed: LivePlayerRenderer slot {} unresolved; native cosmetics "
        "stay on the resource-pack path",
        slot);
    return;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(renderFn),
                       reinterpret_cast<pl::memory::FuncPtr>(HookRender),
                       reinterpret_cast<pl::memory::FuncPtr *>(&g_origRender)) != 0) {
    preloaderLogger.warn(
        "Player-render feed: hook install failed; native cosmetics stay on the "
        "resource-pack path");
  } else {
    preloaderLogger.info("Player-render feed: LivePlayerRenderer::render hooked (slot {})",
                         slot);
  }
}

bool IsPlayerRenderHookLive() {
  return g_lastRenderMs.load(std::memory_order_relaxed) != 0;
}

bool ReadPlayerRenderStats(std::uint32_t out[4]) {
  const long long last = g_lastRenderMs.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  out[0] = g_renderTick.load(std::memory_order_relaxed);
  out[1] = g_frameCalls.load(std::memory_order_relaxed);
  out[2] = g_totalCalls.load(std::memory_order_relaxed);
  const long long delta = MonotonicMs() - last;
  out[3] = delta < 0 ? 0 : static_cast<std::uint32_t>(delta);
  return true;
}

long long LastPlayerRenderMs() {
  return g_lastRenderMs.load(std::memory_order_relaxed);
}

} // namespace pl::runtime
