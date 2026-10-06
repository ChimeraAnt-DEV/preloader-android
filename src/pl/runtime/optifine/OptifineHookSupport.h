#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "pl/runtime/OptifineMode.h"

/**
 * @file OptifineHookSupport.h
 * @brief Shared helpers for the Tier-2 game-hook items.
 *
 * Every Tier-2 item resolves a function through RTTI name + vtable slot (never a baked-in
 * code address) and installs a passthrough detour. These helpers centralise the two rules the
 * items must all follow, so an item cannot forget one:
 *
 *  - **Resolve, never assume.** A slot that does not resolve means the build is different from
 *    what the item expects, and the item reports "skipped" with the reason rather than
 *    hooking a wrong address.
 *  - **Passthrough or nothing.** The detours forward their arguments unchanged and only count
 *    the call; they never dereference an argument whose ABI is unverified. That is what makes
 *    an unknown signature safe.
 */
namespace pl::runtime {

/** The game module every Tier-2 hook targets. */
inline constexpr const char *kOptifineGameModule = "libminecraftpe.so";

/**
 * @brief Resolves a primary-vtable slot by RTTI type name.
 *
 * @param typeInfoName the stored type name, e.g. {@code "14ParticleEngine"}.
 * @param slot the slot index from the vtable address point.
 * @return the function address, or 0 when unresolved.
 */
std::uintptr_t ResolveOptifineSlot(const char *typeInfoName, std::size_t slot);

/**
 * @brief Installs a passthrough detour on a resolved slot.
 *
 * @param slotAddress the address from {@link ResolveOptifineSlot}.
 * @param detour the detour function.
 * @param original receives the trampoline to the original.
 * @param label short name used in the install log.
 * @return true when the hook installed.
 */
bool InstallOptifineHook(std::uintptr_t slotAddress, void *detour, void **original,
                         const char *label);

/**
 * @brief A reusable Tier-2 hook: resolve a vtable slot by RTTI name, install a passthrough
 * detour that only counts calls, and report the outcome.
 *
 * The detour never dereferences an argument (their ABI is unverified), so an unknown signature
 * is safe. The per-frame call counter is what proves the hook is live; an effect that needs
 * per-object semantics is deliberately not attempted here, because guessing at an unverified
 * object layout is how a "culling" hook corrupts rendering instead of improving it.
 *
 * @param typeInfoName RTTI name, e.g. {@code "14ParticleEngine"}.
 * @param slot         primary-vtable slot, or 0 to read it from the per-version rules.
 * @param label        short name used in the status detail.
 * @param counter      receives a pointer to this item's atomic call counter.
 * @param state        filled with the outcome.
 * @return true when the hook installed.
 */
bool InstallTier2CountingHook(const char *typeInfoName, std::size_t slot,
                              const char *label, void *detour, void **original,
                              OptifineItemState &state);

/**
 * @brief Reads the per-version optifine vtable slot for a target from the rules JSON.
 *
 * @param key the rules key, e.g. {@code "particleEngineVtableIndex"}.
 * @return the slot, or 0 when the running version has no verified slot.
 */
std::size_t ReadOptifineHookSlot(const char *key);

} // namespace pl::runtime
