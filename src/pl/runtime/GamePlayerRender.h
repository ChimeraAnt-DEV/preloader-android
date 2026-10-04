#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file GamePlayerRender.h
 * @brief Per-frame player-render feed, captured from the game's own player renderer.
 *
 * The cosmetics system wants to drive its motion (segmented cape, pet attachment) from the
 * transform the game actually renders the player with, not from a value the launcher
 * reconstructs. The in-world player model is drawn by {@code LivePlayerRenderer}, whose
 * {@code render} entry is a primary-vtable slot reachable by RTTI name -- the same
 * name-resolution mechanism the local-player feed already uses, so no per-build code
 * address is needed.
 *
 * This hook is deliberately narrow and fail-closed. It does **not** dereference any of the
 * renderer's arguments (their ABI is not verified), so it can never read freed memory or
 * mis-handle an object. It records only:
 *
 *  - that the player renderer ran this frame (a render tick the caller can advance physics on);
 *  - how many player-render calls occurred (the renderer runs once per rendered player, so a
 *    count above one is direct evidence the hook covers non-local players too);
 *  - the local player's own transform, read through the existing local-player snapshot.
 *
 * With no hook resolved, no live render call, or a stale snapshot it reports "no data" and the
 * caller keeps the resource-pack path. It never draws and never partially renders.
 */
namespace pl::runtime {

/**
 * @brief Installs the player-render hook.
 *
 * Resolves {@code LivePlayerRenderer}'s render slot by RTTI name and hooks it. Safe to call
 * once, from {@code InitGameHooks}; a failure leaves the feed unavailable (fail-closed).
 *
 * @param vtableIndex Primary-vtable slot of the render entry, or 0 to use the built-in default.
 */
void InitPlayerRenderSource(std::size_t vtableIndex);

/** True once the player renderer has actually run this session. */
bool IsPlayerRenderHookLive();

/**
 * @brief Reads the render tick counter and per-frame call count.
 *
 * @param out Receives {renderTick, callsThisFrame, totalCalls, msSinceLastRender}.
 * @return true when the renderer has run at least once this session.
 */
bool ReadPlayerRenderStats(std::uint32_t out[4]);

/**
 * @brief Returns the monotonic timestamp of the most recent player-render call.
 * @return Milliseconds, or 0 when the renderer has not run.
 */
long long LastPlayerRenderMs();

} // namespace pl::runtime
