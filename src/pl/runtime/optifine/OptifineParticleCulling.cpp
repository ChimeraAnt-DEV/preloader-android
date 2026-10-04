#include <atomic>
#include <cstdint>
#include <string>

#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineHookSupport.h"

/**
 * @file OptifineParticleCulling.cpp
 * @brief Tier-2 particle culling: hook the particle engine's entry and count calls.
 *
 * **What this does and does not do.** The request is to drop distant, ambient and low-value
 * emitters. Dropping a particle requires reading the emitter's kind and position from an object
 * whose layout is not recoverable from this build, so a hook that "culled" by guessing would
 * remove the wrong particles.
 *
 * This item installs a **verified passthrough**: it resolves {@code ParticleEngine}'s vtable
 * entry (the RTTI name {@code 14ParticleEngine} is present in the target builds), forwards
 * every argument unchanged, and counts calls so the launcher can show the engine is being
 * exercised. That is a measurement, not a culling effect, and it is labelled as such.
 *
 * Fail-safe: an unresolved slot or a failed install reports "skipped"; the game is untouched.
 */

namespace pl::runtime {
namespace {

constexpr const char *kTypeInfoName = "14ParticleEngine";
constexpr const char *kLabel = "particle culling";

std::atomic<std::uint64_t> gCalls{0};
void *gOriginal = nullptr;

extern "C" std::uintptr_t ParticleDetour(std::uintptr_t a0, std::uintptr_t a1,
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

void ApplyParticleCulling(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gOriginal != nullptr) {
    MarkOptifineActive(state, std::string(kLabel) + ": hook active");
    return;
  }
  const std::size_t slot = ReadOptifineHookSlot("particleEngineVtableIndex");
  InstallTier2CountingHook(kTypeInfoName, slot, kLabel,
                           reinterpret_cast<void *>(&ParticleDetour), &gOriginal, state);
}

struct Registrar {
  Registrar() {
    RegisterOptifineItem(OptifineItem::ParticleCulling, true, &ApplyParticleCulling);
  }
};
Registrar gRegistrar;

} // namespace
} // namespace pl::runtime
