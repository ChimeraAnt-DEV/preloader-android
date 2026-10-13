#include "pl/runtime/GamePlayerRender.h"

#include <atomic>
#include <cstdint>
#include <ctime>

#include "pl/Logger.hpp"
#include "pl/Render.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/render/RenderDispatch.h"
#include "pl/runtime/GameHookRules.h"

namespace pl::runtime {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

// LivePlayerRenderer's render entry. Slot 17 was confirmed against the shipped
// 1.26.60.28 binary (see docs/player-render-hook.md): it is the only slot whose body
// references variable.player_x_rotation / variable.is_first_person / variable.is_using_vr,
// i.e. the routine that poses and draws the player model. The index is overridable from
// the per-version signature rules; the RTTI name resolution means no code address is baked in.
constexpr std::size_t kDefaultRenderSlot = 17;

// The slot was verified on the 1.26.60.x builds. RTTI name resolution means the type is found on
// other builds too, but the slot index can differ; the render hook therefore only installs when the
// slot was confirmed, either from the bundled per-version rules or from a successful runtime probe.
// The gate is deliberately a *floor* rather than a narrow window: builds newer than the verified
// one keep the hook, because the RTTI name still resolves to `LivePlayerRenderer` and a stale index
// is caught by the runtime check below (the slot must resolve to a real function). Widening the
// range is what lets versions like 26.50/26.52 use the native path instead of "nothing draws".
constexpr const char *kVerifiedMinVersion = "1.21.132";
// No upper bound: an empty bound means "unbounded on that side" (see MatchesConfiguredVersion).
constexpr const char *kVerifiedMaxVersion = "";

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

// Which slot the hook resolved to, and how, for the in-app diagnostics report. Set once at install.
std::atomic<std::size_t> g_resolvedSlot{0};
std::atomic_bool g_resolvedByString{false};
std::atomic_bool g_slotKnown{false};

// The single post-render seam. A fixed small table so registration allocates nothing and the
// render thread never locks. At most 4 consumers (cosmetics + future mods).
constexpr std::size_t kMaxPostRenderCallbacks = 4;
std::atomic<PostRenderCallback> g_postRender[kMaxPostRenderCallbacks];
std::atomic<std::size_t> g_postRenderCount{0};

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

// The detour. It runs on the render thread, once per rendered player model. It records the
// tick and call counts, forwards every argument unchanged, then runs the registered post-render
// callbacks.
//
// <b>The callbacks must run on every call, not only on a "new frame".</b> The renderer is invoked
// once per rendered player model, so a frame boundary has to be inferred. The previous version
// inferred it as a gap of more than one second, which is wrong: during play the renderer runs every
// ~16 ms, so the "first call of the frame" test was true only for the very first frame after a
// second-long stall — the cosmetics callback therefore ran once at startup and then never again, so
// nothing appeared in game even though the preview worked. The cosmetic draw callback dedupes per
// published frame version itself, so running it on every call draws exactly once per launcher tick.
void HookRender(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  const long long now = MonotonicMs();
  const long long last = g_lastRenderMs.load(std::memory_order_relaxed);
  // A gap beyond a plausible frame (~1s) is treated as a new frame for the call *count* only; it
  // does not gate the callbacks. The count is what proves the hook covers non-local players.
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

  // Run the post-render seam on every call. Consumers that draw once per frame dedupe internally
  // (the cosmetics callback keys on the published frame version).
  const std::size_t count = g_postRenderCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count && i < kMaxPostRenderCallbacks; ++i) {
    PostRenderCallback callback = g_postRender[i].load(std::memory_order_acquire);
    if (callback != nullptr) callback();
  }
  // Also dispatch the public mod render callbacks (pl/Render.hpp) from this same seam, so mods and
  // the native cosmetics share one render-thread callback point.
  ::pl::render::detail::DispatchRenderCallbacks();
}

} // namespace

void RegisterPostRenderCallback(PostRenderCallback callback) {
  if (callback == nullptr) return;
  const std::size_t count = g_postRenderCount.load(std::memory_order_acquire);
  // Idempotent per function pointer, and bounded.
  for (std::size_t i = 0; i < count && i < kMaxPostRenderCallbacks; ++i) {
    if (g_postRender[i].load(std::memory_order_acquire) == callback) return;
  }
  if (count >= kMaxPostRenderCallbacks) return;
  g_postRender[count].store(callback, std::memory_order_release);
  g_postRenderCount.store(count + 1, std::memory_order_release);
}

void InitPlayerRenderSource(std::size_t vtableIndex) {
  if (g_hookInstalled.exchange(true, std::memory_order_relaxed)) {
    return;
  }

  // Only the builds whose slot index is verified may be hooked. An unverified build fails safe
  // to the resource-pack path rather than installing a detour on an unrelated slot.
  if (!MatchesConfiguredVersion(kVerifiedMinVersion, kVerifiedMaxVersion)) {
    preloaderLogger.warn(
        "Player-render feed: running build is outside the verified range {}-{}; native "
        "cosmetics stay on the resource-pack path",
        kVerifiedMinVersion, kVerifiedMaxVersion);
    return;
  }

  // Resolve the render entry first by the diagnostic string its body references, so a build whose
  // slot index moved can still resolve without a per-build number. When the string scan finds
  // nothing (verified: the render diagnostic is not referenced by any LivePlayerRenderer vtable
  // slot on 26.52, so the scan correctly declines there), fall back to the configured index and
  // then the default.
  std::size_t resolvedSlot = 0;
  std::uintptr_t renderFn = pl::memory::resolveVtableSlotReferencingString(
      "18LivePlayerRenderer", "variable.player_x_rotation", kGameModule, 200, &resolvedSlot);
  std::size_t slot = resolvedSlot;
  bool byString = renderFn != 0;
  if (!renderFn) {
    slot = vtableIndex != 0 ? vtableIndex : kDefaultRenderSlot;
    renderFn = pl::memory::resolveVtableFunction("18LivePlayerRenderer", slot, kGameModule);
  }
  if (!renderFn) {
    preloaderLogger.warn(
        "Player-render feed: LivePlayerRenderer slot unresolved; native cosmetics "
        "stay on the resource-pack path");
    return;
  }
  // The RTTI name resolves on every build, but the slot index can differ; on an unverified build a
  // stale index can land on a data pointer. Refuse to detour anything that is not executable code,
  // so the widened version gate cannot turn a wrong slot into a render-thread fault.
  if (!pl::memory::isExecutableAddress(renderFn, kGameModule)) {
    preloaderLogger.warn(
        "Player-render feed: LivePlayerRenderer slot {} did not resolve to executable code; "
        "skipping the hook",
        slot);
    return;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(renderFn),
                       reinterpret_cast<pl::memory::FuncPtr>(HookRender),
                       reinterpret_cast<pl::memory::FuncPtr *>(&g_origRender),
                       pl::memory::HookPriority::Normal,
                       "LivePlayerRenderer::render") != 0) {
    preloaderLogger.warn(
        "Player-render feed: hook install failed; native cosmetics stay on the "
        "resource-pack path");
  } else {
    preloaderLogger.info("Player-render feed: LivePlayerRenderer::render hooked (slot {})",
                         slot);
    g_resolvedSlot.store(slot, std::memory_order_relaxed);
    g_resolvedByString.store(byString, std::memory_order_relaxed);
    g_slotKnown.store(true, std::memory_order_relaxed);
  }
}

bool LastResolvedRenderSlot(std::size_t &outSlot, bool &outByString) {
  if (!g_slotKnown.load(std::memory_order_relaxed)) return false;
  outSlot = g_resolvedSlot.load(std::memory_order_relaxed);
  outByString = g_resolvedByString.load(std::memory_order_relaxed);
  return true;
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
