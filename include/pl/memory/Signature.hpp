#pragma once

/**
 * @file Signature.hpp
 * @brief Signature resolver API.
 */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pl/Export.hpp"

namespace pl::memory {

/**
 * @brief Resolves one byte signature inside a loaded module.
 */
PL_EXPORT uintptr_t resolveSignature(std::string_view signature,
                                     std::string_view moduleName);

/**
 * @brief Resolves multiple byte signatures inside a loaded module.
 */
PL_EXPORT std::unordered_map<std::string, uintptr_t>
resolveSignatures(std::span<const std::string> signatures,
                  std::string_view moduleName);

/**
 * @brief Whether an address falls inside the executable range of a loaded module.
 *
 * A hook target that resolves to a data section, a constant pool or an unrelated function is what
 * makes an inline detour fault the render thread. When a hook's vtable slot cannot be verified for a
 * given build, this guard lets the caller confirm the resolved address is actually code before
 * installing a detour — so widening a version gate cannot turn into a crash on an unverified build.
 */
PL_EXPORT bool isExecutableAddress(uintptr_t address, std::string_view moduleName);

/**
 * @brief Resolves a vtable slot by the diagnostic string its function references, not by index.
 *
 * The slot index of a virtual function moves between Minecraft builds, so a hardcoded index is
 * correct for exactly one build — on another build it either resolves to an unrelated function or
 * to null. This finds the slot whose function references @p needle (e.g.
 * {@code "variable.player_x_rotation"} for {@code LivePlayerRenderer::render}) by scanning each
 * slot's function for an ADRP/ADD pair that forms the string's address, and returns the first match.
 * It needs no per-build index, which is the point: the same call works across builds.
 *
 * @param typeInfoName the RTTI type name, e.g. {@code "18LivePlayerRenderer"}
 * @param needle       the NUL-terminated string the target function references
 * @param moduleName   the module to resolve in
 * @param maxSlots     the highest slot index to consider
 * @param outSlot      optionally receives the matching slot index
 * @return the function address, or 0 when no slot references the string
 */
PL_EXPORT uintptr_t resolveVtableSlotReferencingString(std::string_view typeInfoName,
                                                       std::string_view needle,
                                                       std::string_view moduleName,
                                                       std::size_t maxSlots,
                                                       std::size_t *outSlot = nullptr);

} // namespace pl::memory
