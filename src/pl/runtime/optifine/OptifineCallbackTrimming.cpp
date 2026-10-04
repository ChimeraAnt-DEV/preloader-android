#include <atomic>
#include <cstdint>
#include <string>

#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineHookSupport.h"

/**
 * @file OptifineCallbackTrimming.cpp
 * @brief Tier-2 callback trimming: hook the app-platform tick and count it.
 *
 * **What this does and does not do.** "Trimming" a callback requires knowing which registered
 * listener is low-value, and that list lives behind an object layout this build does not expose.
 * What is safely reachable is the tick entry itself ({@code AppPlatformListener}, the base the
 * per-frame callbacks share): a passthrough detour that forwards every argument unchanged and
 * counts ticks.
 *
 * The count is not decorative: the launcher uses the tick rate as its frame-rate reading for the
 * dynamic render-distance governor, because the tick is the per-frame entry point and the
 * launcher cannot otherwise observe the game's frame rate. That makes this hook the feed for
 * another item rather than a standalone effect, which is what its status detail says.
 *
 * Fail-safe: an unresolved slot or a failed install reports "skipped"; the game is untouched.
 */

namespace pl::runtime {
namespace {

constexpr const char *kTypeInfoName = "19AppPlatformListener";
constexpr const char *kLabel = "callback trimming";

std::atomic<std::uint64_t> gTicks{0};
void *gOriginal = nullptr;

extern "C" std::uintptr_t TickDetour(std::uintptr_t a0, std::uintptr_t a1,
                                     std::uintptr_t a2, std::uintptr_t a3,
                                     std::uintptr_t a4, std::uintptr_t a5,
                                     std::uintptr_t a6, std::uintptr_t a7) {
  gTicks.fetch_add(1, std::memory_order_relaxed);
  using Fn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t);
  if (gOriginal != nullptr) {
    return reinterpret_cast<Fn>(gOriginal)(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  return 0;
}

void ApplyCallbackTrimming(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gOriginal != nullptr) {
    MarkOptifineActive(state, std::string(kLabel) + ": tick hook active");
    return;
  }
  const std::size_t slot = ReadOptifineHookSlot("appPlatformTickVtableIndex");
  InstallTier2CountingHook(kTypeInfoName, slot, kLabel,
                           reinterpret_cast<void *>(&TickDetour), &gOriginal, state);
}

struct Registrar {
  Registrar() {
    RegisterOptifineItem(OptifineItem::CallbackTrimming, true, &ApplyCallbackTrimming);
  }
};
Registrar gRegistrar;

} // namespace

/** @brief The tick count observed since the hook was installed. */
std::uint64_t OptifineTickCount() { return gTicks.load(std::memory_order_relaxed); }

} // namespace pl::runtime
