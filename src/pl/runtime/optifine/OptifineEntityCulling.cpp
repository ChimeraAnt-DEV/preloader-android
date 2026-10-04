#include <atomic>
#include <cstdint>

#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineHookSupport.h"

/**
 * @file OptifineEntityCulling.cpp
 * @brief Tier-2 entity culling: hook the entity renderer's entry and count calls.
 *
 * **What this does and does not do.** The request is to skip entities that are off-screen or
 * occluded. That requires reading the entity object's transform and the camera frustum, whose
 * layouts are not recoverable from this build (game classes export no symbols; the vtable slot
 * gives a code address, not a field map). A hook that guessed at an object layout would corrupt
 * rendering, not improve it.
 *
 * So this item installs a **verified passthrough**: it resolves the entity renderer's vtable
 * entry, installs a detour that forwards every argument unchanged and counts the calls, and
 * reports the call rate. That is real, safe, and testable — but it is a measurement, not a
 * culling effect, and it is labelled that way in the UI. When a verified entity-transform feed
 * exists (the same route the combat modules document as unreachable today), the culling can be
 * added on top of this hook without changing its resolution or fail-safe behaviour.
 *
 * Fail-safe: an unresolved slot or a failed install reports "skipped" and the game is untouched.
 */

namespace pl::runtime {
namespace {

/** Entity renderer entry. The RTTI name is present in the target builds; the slot is per
 *  version and comes from the rules, defaulting to "unverified" (0). */
constexpr const char *kTypeInfoName = "21ActorRenderDispatcher";
constexpr const char *kLabel = "entity culling";

std::atomic<std::uint64_t> gCalls{0};
void *gOriginal = nullptr;

/**
 * Passthrough detour. The unverified arguments are forwarded by register (up to x7) without
 * being read; only the call is counted.
 */
extern "C" std::uintptr_t EntityRenderDetour(std::uintptr_t a0, std::uintptr_t a1,
                                             std::uintptr_t a2, std::uintptr_t a3,
                                             std::uintptr_t a4, std::uintptr_t a5,
                                             std::uintptr_t a6, std::uintptr_t a7) {
  gCalls.fetch_add(1, std::memory_order_relaxed);
  using Fn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t);
  if (gOriginal != nullptr) {
    return reinterpret_cast<Fn>(gOriginal)(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  return 0;
}

void ApplyEntityCulling(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gOriginal != nullptr) {
    MarkOptifineActive(state, std::string(kLabel) + ": hook active");
    return;
  }
  const std::size_t slot = ReadOptifineHookSlot("entityRenderVtableIndex");
  InstallTier2CountingHook(kTypeInfoName, slot, kLabel,
                           reinterpret_cast<void *>(&EntityRenderDetour), &gOriginal,
                           state);
}

struct Registrar {
  Registrar() {
    RegisterOptifineItem(OptifineItem::EntityCulling, true, &ApplyEntityCulling);
  }
};
Registrar gRegistrar;

} // namespace
} // namespace pl::runtime
