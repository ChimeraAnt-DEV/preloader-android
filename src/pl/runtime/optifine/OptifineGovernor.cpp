#include "pl/runtime/optifine/OptifineGovernor.h"

#include <algorithm>

namespace pl::runtime {

int StepRenderDistance(const RenderDistanceConfig &config, RenderDistanceState &state,
                       int fps, long long nowMs) {
  const int minDistance = std::max(1, config.minDistance);
  const int maxDistance = std::max(minDistance, config.maxDistance);

  if (!state.initialized) {
    state.currentDistance = maxDistance;
    state.initialized = true;
    state.belowSinceMs = 0;
    state.aboveSinceMs = 0;
  }
  state.currentDistance = std::clamp(state.currentDistance, minDistance, maxDistance);

  const bool below = fps < config.fpsThreshold;
  if (below) {
    // A dip starts (or continues) the down-timer and clears the up-timer, so a single bad
    // sample between two good ones cannot both arm a drop and a restore.
    state.aboveSinceMs = 0;
    if (state.belowSinceMs == 0) state.belowSinceMs = nowMs;
    if (nowMs - state.belowSinceMs >= config.dropSustainMs &&
        state.currentDistance > minDistance) {
      state.currentDistance -= 1;
      state.belowSinceMs = nowMs; // re-arm: one step per sustain window, not one per sample.
    }
  } else {
    state.belowSinceMs = 0;
    if (state.aboveSinceMs == 0) state.aboveSinceMs = nowMs;
    if (nowMs - state.aboveSinceMs >= config.restoreSustainMs &&
        state.currentDistance < maxDistance) {
      state.currentDistance += 1;
      state.aboveSinceMs = nowMs;
    }
  }

  return state.currentDistance;
}

} // namespace pl::runtime
