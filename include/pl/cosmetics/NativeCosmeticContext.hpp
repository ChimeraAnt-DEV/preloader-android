#pragma once

/**
 * @file NativeCosmeticContext.hpp
 * @brief Resolves the render-context world anchor at runtime.
 *
 * The game's player-render function is stripped and its signatures are version-dependent, so
 * nothing here bakes in a code address. Instead:
 *  - A best-effort byte-pattern scan looks for a plausible camera/projection matrix in
 *    `libminecraftpe.so` (see {@link ResolveRenderWorldFromPatterns}).
 *  - The reliable fallback is the live local-player feed: the cosmetics are anchored at the
 *    player's world position as a translation, so they appear where the player stands even when the
 *    full camera matrix cannot be recovered.
 *
 * A caller chooses the strongest anchor it has and feeds it to
 * {@code pl::cosmetics::SetCosmeticWorld}. All of it is fail-closed: an unresolved scan returns
 * false and the caller keeps whatever anchor it already had.
 */

#include <cstdint>

#include "pl/cosmetics/NativeModelMatrix.hpp"

namespace pl::cosmetics {

/**
 * @brief Attempts to resolve a camera/projection matrix by scanning the game module for a
 *        characteristic 4x4 matrix pattern.
 *
 * This is deliberately a *probe*: the resolver scans the readable ranges of the game module for a
 * contiguous 16-float region that looks like a perspective projection (monotonic X/Y scale,
 * plausible near/far values) and returns the first match offset as a runtime address. On a build
 * where no such region exists (or where the pattern is ambiguous), it returns 0 and the caller
 * falls back to the player-position anchor. It never dereferences the result here — returning the
 * address is the whole contract, and the caller decides whether to read it.
 *
 * @param moduleName the game module to scan, e.g. "libminecraftpe.so"
 * @param outAddress receives the resolved matrix address, or 0 when not found
 * @return true when a candidate matrix was located
 */
bool ResolveRenderWorldFromPatterns(const char *moduleName, std::uintptr_t &outAddress);

/**
 * @brief Builds a world anchor from the live local-player position.
 *
 * Returns a translation matrix for the player's current world coordinates (the same feed the
 * in-world nametag icons use). Without a live position the matrix is identity.
 */
Mat4 WorldFromLocalPlayer();

} // namespace pl::cosmetics