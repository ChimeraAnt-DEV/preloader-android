#pragma once

/**
 * @file CosmeticDrawGate.hpp
 * @brief Pure decision logic for the per-render-frame cosmetic draw.
 *
 * <p>Two defects lived in the render callback's inline conditions and are fixed here so the rules
 * can be unit-tested without a device or a GL context (the callback itself only marshals data):
 *
 * <ul>
 *   <li><b>The debug cube could never draw.</b> The callback returned before calling the renderer
 *       whenever nothing was equipped or the geometry blob was empty, so the self-test cube the
 *       renderer can draw with an empty frame was unreachable — the intended pass/fail instrument
 *       on a device was dead. When the cube is armed the callback must skip the "nothing equipped"
 *       and geometry gates (a camera/anchor is still required: without somewhere to draw there is
 *       nothing to prove).</li>
 *   <li><b>At most one draw per published Java frame.</b> The callback compared the published frame
 *       version against the last drawn one and returned on a match, but Java publishes at most one
 *       frame per game tick while the renderer runs every frame. Any render frame without a fresh
 *       published version therefore drew nothing, so cosmetics flickered or vanished whenever the
 *       render rate exceeded the push rate. The gate now draws the latest snapshot on <em>every</em>
 *       render call; the version is retained only to let a caller report what it drew, not to skip.</li>
 * </ul>
 */

#include <cstddef>
#include <cstdint>

namespace pl::cosmetics {

/** Why a render call did or did not draw, for the diagnostics line. */
enum class DrawDecision {
    /** Draw the frame the registry holds. */
    Draw,
    /** Nothing is equipped and the debug cube is not armed. */
    SkipNothingEquipped,
    /** No published frame is available yet. */
    SkipNoFrame,
    /** The live local-player feed is unavailable (not in a world). */
    SkipNoLocalPlayer,
    /** No geometry blob was published and the debug cube is not armed. */
    SkipNoGeometry,
};

/**
 * @brief The pure draw decision.
 *
 * @param frameValid      a published cosmetic frame exists
 * @param frameFlags      the frame's flags (low nibble = equipped pieces, bit 3 = VFX)
 * @param debugCubeArmed  the diagnostic cube toggle is on
 * @param localPlayerLive the local-player position feed succeeded this frame
 * @param geometrySize    bytes of published geometry blob
 */
inline DrawDecision DecideCosmeticDraw(bool frameValid, std::uint32_t frameFlags,
                                       bool debugCubeArmed, bool localPlayerLive,
                                       std::size_t geometrySize) {
    if (!frameValid) {
        // No frame at all: the renderer has no transforms to apply. (The cube still needs a frame
        // header for the piece flags, and the launcher always publishes one while a session runs.)
        return DrawDecision::SkipNoFrame;
    }
    const bool nothingEquipped = (frameFlags & 0xFu) == 0;
    if (nothingEquipped && !debugCubeArmed) {
        return DrawDecision::SkipNothingEquipped;
    }
    if (!localPlayerLive) {
        return DrawDecision::SkipNoLocalPlayer;
    }
    // The geometry blob is only needed for a cosmetic piece. With the debug cube armed an empty
    // blob must not block the draw, or the instrument is unreachable again.
    if (geometrySize == 0 && !debugCubeArmed) {
        return DrawDecision::SkipNoGeometry;
    }
    return DrawDecision::Draw;
}

/**
 * @brief Whether a render call should reuse the previously drawn frame version.
 *
 * <p>Retained for the diagnostics counter only. It must never be used to skip a draw: the renderer
 * runs more often than Java publishes, so skipping on an unchanged version is exactly the flicker
 * bug. This always returns {@code true} so a caller that keeps the old call shape cannot regress.
 *
 * @param publishedVersion the version just read from the registry
 * @param lastDrawnVersion the version drawn on the previous render call
 */
inline bool ShouldDrawFrameVersion(std::uint64_t publishedVersion, std::uint64_t lastDrawnVersion) {
    (void) publishedVersion;
    (void) lastDrawnVersion;
    return true;
}

} // namespace pl::cosmetics
