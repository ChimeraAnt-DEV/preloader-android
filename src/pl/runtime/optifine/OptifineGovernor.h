#pragma once

/**
 * @file OptifineGovernor.h
 * @brief Pure hysteresis rule for dynamic render distance.
 *
 * The governor decides when the effective render distance should step down under frame pressure
 * and when it may step back up. It is pure so the rule — which is where the regressions live —
 * is unit-testable without a game.
 *
 * The asymmetry is deliberate: a drop happens after a short sustained dip, a restore only after
 * a long sustained recovery. Symmetric thresholds make the distance oscillate at the boundary,
 * which is more distracting than a slightly-too-low distance.
 */
namespace pl::runtime {

struct RenderDistanceConfig {
  int minDistance{4};
  int maxDistance{12};
  int fpsThreshold{45};
  /** Consecutive ms below threshold before stepping down one chunk. */
  long long dropSustainMs{1500};
  /** Consecutive ms above threshold before stepping back up one chunk. */
  long long restoreSustainMs{10000};
};

struct RenderDistanceState {
  int currentDistance{0};
  long long belowSinceMs{0};
  long long aboveSinceMs{0};
  bool initialized{false};
};

/**
 * @brief Feeds one FPS sample and returns the distance to use now.
 *
 * @param config the bounds and thresholds.
 * @param state  carried between calls (initialise to {} on first use).
 * @param fps    the measured frames per second.
 * @param nowMs  monotonic milliseconds.
 * @return the effective render distance after this sample.
 */
int StepRenderDistance(const RenderDistanceConfig &config, RenderDistanceState &state,
                       int fps, long long nowMs);

} // namespace pl::runtime
