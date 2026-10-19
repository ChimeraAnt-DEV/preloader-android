#pragma once

/**
 * @file HookInstallGate.hpp
 * @brief Decides when the deferred game-hook install may run.
 *
 * <p>The hooks used to install from the preloader's `enableLoadedMods` call, which the game activity
 * makes in `onCreate` <em>before</em> the engine's graphics backend and the `libminecraftpe.so`
 * relocation have finished. Two things go wrong there:
 *
 * <ul>
 *   <li>The RTTI/vtable resolution reads the game module's `.rela.dyn` and vtables, which are not
 *       fully mapped yet, so the slot resolves to nothing and the install silently bails — the
 *       "Hook installed: no" the diagnostics report.</li>
 *   <li>Even when it resolves, the render seam it hooks is not yet being driven, so the first frames
 *       the feed needs to observe are gone before the hook exists.</li>
 * </ul>
 *
 * <p>The install is therefore deferred to the game's own frame tick, which by definition runs after
 * the runtime is up and on the same thread that calls the render entry (so the code patch never
 * races the renderer). This gate is the pure rule for when that deferred install runs, so the
 * "installed exactly once, never waited forever" invariants are host-tested without a device.
 */

namespace pl::runtime {

/** What the caller should do on this frame. */
enum class InstallDecision {
    /** Not yet: keep ticking. */
    Wait,
    /** The runtime is up — install the hooks now. */
    Install,
    /** The wait exceeded its budget; give up so the state is never left "pending" forever. */
    GiveUp,
};

/**
 * @brief The deferred-install state machine.
 *
 * <p>Pure and allocation-free: it only counts frame ticks against a monotonic clock. {@link #start}
 * arms it at preloader-init time; {@link #tick} is called once per game frame and reports the
 * decision. Once it has decided (install or give up) it stays resolved and reports {@code Wait}
 * forever, so the install can never run twice.
 */
class HookInstallGate {
public:
    /** Frames to observe before installing. One is enough: the first tick proves the runtime is up. */
    static constexpr int kMinFrameTicks = 1;
    /** Never wait longer than this for the runtime; a stuck loading screen must not pend forever. */
    static constexpr long long kGiveUpMs = 30'000;

    /** Never defer more than this many times; a runtime that never delivers rules must settle. */
    static constexpr int kMaxDefers = 8;

    /** Arms the gate. {@code nowMs} is a monotonic millisecond timestamp. */
    void start(long long nowMs) {
        startedMs_ = nowMs;
        frameTicks_ = 0;
        deferredCount_ = 0;
        resolved_ = false;
    }

    /** @return true once the gate has produced a decision and will never produce another. */
    bool isResolved() const { return resolved_; }

    /**
     * Re-arms after a deferred install.
     *
     * <p>A tick can decide {@code Install} while the install body is not yet able to run (the
     * launcher has not delivered the signature rules). Without this the gate stays resolved and
     * every later tick reports {@code Wait}, so the hooks never install even once the rules arrive
     * - the deferred install would be lost for the whole session. Re-arming schedules another
     * attempt and counts it, so a runtime that never delivers the rules still gives up eventually
     * rather than retrying forever.
     *
     * @param nowMs the monotonic time to re-arm at
     * @return the number of deferred re-arms so far
     */
    int defer(long long nowMs) {
        startedMs_ = nowMs;
        resolved_ = false;
        return ++deferredCount_;
    }

    /** How many times the install was deferred and re-armed. */
    int deferredCount() const { return deferredCount_; }

    /** The number of frame ticks observed so far (diagnostics). */
    int frameTicks() const { return frameTicks_; }

    /** Advances one frame and returns the decision. A resolved gate always returns Wait. */
    InstallDecision tick(long long nowMs) {
        if (resolved_) return InstallDecision::Wait;
        ++frameTicks_;
        if (frameTicks_ >= kMinFrameTicks) {
            resolved_ = true;
            return InstallDecision::Install;
        }
        if (nowMs - startedMs_ >= kGiveUpMs) {
            resolved_ = true;
            return InstallDecision::GiveUp;
        }
        return InstallDecision::Wait;
    }

private:
    long long startedMs_ = 0;
    int frameTicks_ = 0;
    int deferredCount_ = 0;
    bool resolved_ = false;
};

} // namespace pl::runtime
