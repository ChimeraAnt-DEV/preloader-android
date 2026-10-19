#pragma once

/**
 * @file SkinSeamObservation.hpp
 * @brief Turns the join-packet / texture-bind seam counters into an honest skin diagnosis.
 *
 * <p>The reported bug is that a custom skin is not visible to other players. Deciding whether the
 * fault is the client (the skin never leaves the device) or the server (the skin is dropped) needs
 * a reading from the join path, and this build cannot supply one directly: the game strips its
 * symbols, so the join packet's layout is unknown and the detours are register-preserving
 * passthroughs that count calls without reading the payload. Fabricating a skin id from that would
 * be the over-claim this module exists to avoid.
 *
 * <p>What the counters <em>can</em> prove is which stage is reached, and that is enough to route a
 * bug report:
 *
 * <ul>
 *   <li>The join hook never installed — nothing was observed, so the client side is unproven.</li>
 *   <li>The join hook installed but no join was ever seen — the seam does not carry the traffic this
 *       build expects, so it proves nothing either.</li>
 *   <li>Joins observed — the client reached the packet handler; whether the payload carried the skin
 *       (and whether the server kept it) still needs the field layout, which is the next step.</li>
 * </ul>
 *
 * <p>Pure C++ with no Android types, so the classification is host-tested with the real counters and
 * no device.
 */

#include <cstdint>

namespace pl::cosmetics {

/** @brief Which stage of the skin path the counters prove. */
enum class SkinSeamStage {
    /** The join hook is not installed (or the rule did not resolve it). */
    JoinHookAbsent,
    /** The join hook is installed but has not seen a join yet. */
    JoinHookIdle,
    /** Joins were observed; the payload fields themselves are not readable in this build. */
    JoinObserved,
};

/**
 * @brief Classifies the seam counters into a stage and a one-line explanation.
 *
 * @param packetHookInstalled whether the player-join detour installed
 * @param packetCalls         join-packet calls the detour forwarded
 * @param textureHookInstalled whether the texture-bind detour installed
 * @param textureCalls        texture-bind calls the detour forwarded
 * @param skinCaptured        whether the engine captured a base-skin texture this session
 * @return the stage; use {@link DescribeSkinSeam} for the report line
 */
inline SkinSeamStage ClassifySkinSeam(bool packetHookInstalled, std::uint32_t packetCalls,
                                      bool textureHookInstalled, std::uint32_t textureCalls,
                                      bool skinCaptured) {
    (void) textureHookInstalled;
    (void) textureCalls;
    (void) skinCaptured;
    if (!packetHookInstalled) return SkinSeamStage::JoinHookAbsent;
    return packetCalls == 0U ? SkinSeamStage::JoinHookIdle : SkinSeamStage::JoinObserved;
}

/**
 * @brief The report line for a stage, naming what is proven and what is not.
 *
 * <p>Kept in one place so the diagnostics report and any future surface cannot describe the same
 * counters differently, and so the wording is pinned by a host test.
 */
inline const char *DescribeSkinSeam(SkinSeamStage stage) {
    switch (stage) {
        case SkinSeamStage::JoinHookAbsent:
            return "the join-packet hook is not installed, so nothing was observed (unproven)";
        case SkinSeamStage::JoinHookIdle:
            return "the join-packet hook is installed but has seen no join yet";
        case SkinSeamStage::JoinObserved:
            return "joins observed; the payload's skin fields are not readable in this build, so "
                   "whether the client sent the skin is still unproven";
    }
    return "unknown";
}

} // namespace pl::cosmetics
