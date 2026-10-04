#include "pl/runtime/OptifineMode.h"

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "pl/Logger.hpp"
#include "pl/runtime/OptifineConfig.h"

namespace pl::runtime {
namespace {

constexpr int kItemCount = static_cast<int>(OptifineItem::Count);

std::mutex gRegistryMutex;
std::array<ItemHandler, kItemCount> &Registry() {
  static std::array<ItemHandler, kItemCount> table{};
  return table;
}
std::array<bool, kItemCount> &Tier2Flags() {
  static std::array<bool, kItemCount> table{};
  return table;
}

std::mutex gStateMutex;
OptifineState gState{};
OptifineConfig gConfig{};
/** Items the launcher reported as having been active on the last launch. */
std::string gAppliedBlob;

constexpr const char *kItemNames[] = {
    "mimalloc allocator", "Render thread priority", "CPU affinity pinning",
    "Refresh rate unlock", "Entity culling", "Particle culling",
    "Dynamic render distance", "Callback trimming", "OreUI overlay stripping"};

/**
 * Runs one item's handler and records the outcome.
 *
 * A handler that throws (allocation failure, unexpected syscall error) must not take the
 * process down: the whole point of an independently-toggleable optimization is that a broken
 * one degrades to "off", so an exception is caught and reported as a failure.
 *
 * Caller holds {@link gStateMutex}.
 */
void ApplyItem(int index, OptifineDecision decision) {
  OptifineItemState &state = gState.items[index];
  state.tier2 = Tier2Flags()[index];

  switch (decision) {
  case OptifineDecision::Off:
    state.enabled = false;
    state.status = 0;
    state.detail = "off";
    return;
  case OptifineDecision::AutoDisabled:
    state.enabled = false;
    state.status = 2;
    state.detail = "auto-disabled after 3 consecutive crashes";
    return;
  case OptifineDecision::Apply:
    break;
  }

  state.enabled = true;
  ItemHandler handler = Registry()[index];
  if (handler == nullptr) {
    state.enabled = false;
    state.status = 2;
    state.detail = "not available in this build";
    return;
  }
  try {
    handler(true, state);
  } catch (const std::exception &ex) {
    state.status = 3;
    state.detail = std::string("failed: ") + ex.what();
  } catch (...) {
    state.status = 3;
    state.detail = "failed: unknown error";
  }
}

} // namespace

void MarkOptifineActive(OptifineItemState &state, std::string detail) {
  state.status = 1;
  state.detail = std::move(detail);
}

void MarkOptifineSkipped(OptifineItemState &state, std::string reason) {
  state.status = 2;
  state.detail = std::move(reason);
}

void MarkOptifineFailed(OptifineItemState &state, std::string reason) {
  state.status = 3;
  state.detail = std::move(reason);
}

void RegisterOptifineItem(OptifineItem item, bool tier2, ItemHandler handler) {
  const int index = static_cast<int>(item);
  if (index < 0 || index >= kItemCount) return;
  std::lock_guard<std::mutex> lock(gRegistryMutex);
  Registry()[index] = handler;
  Tier2Flags()[index] = tier2;
}

std::string_view OptifineItemId(OptifineItem item) {
  return OptifineItemIdAt(static_cast<int>(item));
}

std::string_view OptifineItemName(OptifineItem item) {
  const int index = static_cast<int>(item);
  if (index < 0 || index >= kItemCount) return "Unknown";
  return kItemNames[index];
}

void ConfigureOptifineMode(std::string_view blob) {
  const OptifineConfig config = ParseOptifineConfig(blob);

  std::lock_guard<std::mutex> lock(gStateMutex);
  gConfig = config;
  gState.masterEnabled = config.master;

  int appliedCount = 0;
  std::string applied;
  for (int i = 0; i < kItemCount; ++i) {
    ApplyItem(i, DecideOptifineItem(config, i));
    if (gState.items[i].enabled && gState.items[i].status == 1) {
      applied += OptifineItemIdAt(i);
      applied += '\n';
      ++appliedCount;
    }
  }
  gAppliedBlob = applied;

  preloaderLogger.info("Optifine mode: master={} applied={} item(s)", config.master,
                       appliedCount);
}

OptifineState ReadOptifineState() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  return gState;
}

bool IsOptifineModeActive() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  if (!gState.masterEnabled) return false;
  for (const auto &item : gState.items) {
    if (item.enabled && item.status == 1) return true;
  }
  return false;
}

void ApplyOptifineModeEarly() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  if (!gState.masterEnabled) return;
  // Some host items (render-thread priority, CPU affinity) cannot find their target thread
  // until the game has started it, so a first pass reports "deferred" and this later pass
  // retries. Handlers are idempotent: an already-active item is left alone.
  for (int i = 0; i < kItemCount; ++i) {
    OptifineItemState &state = gState.items[i];
    if (!state.enabled || state.status == 1) continue;
    if (DecideOptifineItem(gConfig, i) == OptifineDecision::AutoDisabled) continue;
    ApplyItem(i, OptifineDecision::Apply);
  }
}

std::string AppliedOptifineItemsBlob() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  return gAppliedBlob;
}

} // namespace pl::runtime
