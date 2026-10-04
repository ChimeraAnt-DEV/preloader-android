#include <mutex>
#include <string>

#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineGovernor.h"

/**
 * @file OptifineRenderDistance.cpp
 * @brief Tier-2 dynamic render distance: step the distance down under sustained low FPS.
 *
 * The hysteresis rule lives in {@link OptifineGovernor} and is unit-tested. This file is the
 * item wiring: it owns the state and the bounds, and exposes a sample entry point the game tick
 * (or the launcher, when it has a frame-rate reading) can feed.
 *
 * **Honest limitation.** Actually changing the game's render distance needs a native setter on
 * the options object, which this build does not expose (game classes export no symbols). So this
 * item computes the distance the governor *would* choose and reports it; it does not yet push it
 * into the game. The UI labels it accordingly, and it defaults OFF so a user is never told a
 * distance changed when it did not. The moment a verified options setter exists, it is one call
 * from here.
 *
 * Fail-safe: with no FPS feed the governor never steps and the item reports "waiting for a
 * frame-rate reading".
 */

namespace pl::runtime {
namespace {

std::mutex gMutex;
RenderDistanceState gState{};
RenderDistanceConfig gConfig{};
bool gHasReading = false;
int gLastDistance = 0;

struct Registrar {
  Registrar() {
    RegisterOptifineItem(OptifineItem::DynamicRenderDistance, true, &ApplyRenderDistance);
  }

  static void ApplyRenderDistance(bool enabled, OptifineItemState &state) {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gHasReading) {
      MarkOptifineSkipped(state, "waiting for a frame-rate reading");
      return;
    }
    MarkOptifineActive(state, "render distance governor: " +
                                  std::to_string(gLastDistance) + " chunks");
  }
};
Registrar gRegistrar;

} // namespace

/** @brief Feeds one FPS sample; returns the effective distance the governor chose. */
int SampleOptifineRenderDistance(int fps, long long nowMs) {
  std::lock_guard<std::mutex> lock(gMutex);
  gHasReading = true;
  gLastDistance = StepRenderDistance(gConfig, gState, fps, nowMs);
  return gLastDistance;
}

/** @brief Configures the governor bounds from the launcher's sub-option values. */
void ConfigureOptifineRenderDistance(int minDistance, int maxDistance, int fpsThreshold) {
  std::lock_guard<std::mutex> lock(gMutex);
  gConfig.minDistance = minDistance;
  gConfig.maxDistance = maxDistance;
  gConfig.fpsThreshold = fpsThreshold;
  gState = RenderDistanceState{};
}

} // namespace pl::runtime
