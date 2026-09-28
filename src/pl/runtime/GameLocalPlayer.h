#pragma once

#include <cstdint>

/**
 * @file GameLocalPlayer.h
 * @brief Local-player position/rotation feed for the in-world Voice nametag icons.
 *
 * The Voice nametag icon projects a peer's advertised world position against the
 * local player's own view. The peer positions arrive over the voice protocol; the
 * local view is the only thing that has to come from the game, and it is two
 * reads on the local player: its position and its yaw/pitch.
 *
 * This is deliberately the smallest possible native surface. It does not walk the
 * entity list, resolve nametags, or touch the renderer: it captures the live
 * {@code ClientInstance} once per frame (by hooking the client's own
 * {@code getLocalPlayer} vtable slot) and reads two fields off the returned
 * {@code LocalPlayer}. With no hook resolved or no live instance it reports "no
 * data" and the caller draws nothing -- never a guessed position.
 */
namespace pl::runtime {

/**
 * @brief Installs the local-player feed.
 *
 * Resolves the {@code ClientInstance} vtable slot that returns the local player
 * and hooks it to capture the live instance each frame. Safe to call once, from
 * {@code InitGameHooks}; a failure leaves the feed unavailable (fail-closed).
 */
void InitLocalPlayerSource();

/** True once a live local player has been seen this session. */
bool IsLocalPlayerAvailable();

/**
 * @brief Reads the local player's world position.
 * @param out Receives x, y, z on success; left untouched otherwise.
 * @return true when a live position was read.
 */
bool ReadLocalPlayerPosition(float out[3]);

/**
 * @brief Reads the local player's view rotation in degrees.
 * @param out Receives yaw, pitch on success; left untouched otherwise.
 * @return true when a live rotation was read.
 */
bool ReadLocalPlayerRotation(float out[2]);

} // namespace pl::runtime
