#include <atomic>
#include <cstdint>
#include <string>

#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineHookSupport.h"

/**
 * @file OptifineOreUiStripping.cpp
 * @brief Tier-2 OreUI stripping: hook the in-game HUD screen and count its updates.
 *
 * **What this does and does not do.** The request is to skip the OreUI overlay's work when no
 * menu is open. Detecting "no menu is open" and skipping the right draw requires the screen
 * object's state, which this build does not expose. What is safely reachable is the in-game HUD
 * screen entry ({@code InGamePlayScreen}, present in the target builds): a passthrough detour
 * that forwards every argument unchanged and counts calls.
 *
 * The launcher pairs this count with the existing {@code hudScreenOpen} hook to show the HUD is
 * live. It is a measurement, not a stripping effect, and its status detail says so; the item
 * defaults OFF so nobody is told a draw was skipped when it was not.
 *
 * Fail-safe: an unresolved slot or a failed install reports "skipped"; the game is untouched.
 */

namespace pl::runtime {
namespace {

constexpr const char *kTypeInfoName = "16InGamePlayScreen";
constexpr const char *kLabel = "OreUI stripping";

std::atomic<std::uint64_t> gUpdates{0};
void *gOriginal = nullptr;

extern "C" std::uintptr_t HudDetour(std::uintptr_t a0, std::uintptr_t a1,
                                    std::uintptr_t a2, std::uintptr_t a3,
                                    std::uintptr_t a4, std::uintptr_t a5,
                                    std::uintptr_t a6, std::uintptr_t a7) {
  gUpdates.fetch_add(1, std::memory_order_relaxed);
  using Fn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t);
  if (gOriginal != nullptr) {
    return reinterpret_cast<Fn>(gOriginal)(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  return 0;
}

void ApplyOreUiStripping(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gOriginal != nullptr) {
    MarkOptifineActive(state, std::string(kLabel) + ": HUD hook active");
    return;
  }
  const std::size_t slot = ReadOptifineHookSlot("hudScreenUpdateVtableIndex");
  InstallTier2CountingHook(kTypeInfoName, slot, kLabel,
                           reinterpret_cast<void *>(&HudDetour), &gOriginal, state);
}

struct Registrar {
  Registrar() {
    RegisterOptifineItem(OptifineItem::OreUiStripping, true, &ApplyOreUiStripping);
  }
};
Registrar gRegistrar;

} // namespace

/** @brief The HUD update count observed since the hook was installed. */
std::uint64_t OptifineHudUpdateCount() { return gUpdates.load(std::memory_order_relaxed); }

} // namespace pl::runtime
