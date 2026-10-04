#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "pl/Logger.hpp"
#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineDeviceRules.h"
#include "pl/runtime/optifine/OptifineProcess.h"

/**
 * @file OptifineCpuAffinity.cpp
 * @brief Tier-1 CPU affinity pinning.
 *
 * Big.LITTLE devices schedule the game's main thread onto little cores under load, which shows
 * up as stutter even when a big core is idle. This reads the per-core maximum frequencies from
 * sysfs, identifies the performance cluster, and pins the main (game) thread to it.
 *
 * The rules live in {@link OptifineDeviceRules} and are unit-tested. This file is the platform
 * half. Default ON only where it is worth doing: fewer than four performance cores and the
 * device is left alone (a 4-little-core budget phone gains nothing and loses thermal headroom).
 *
 * Fail-safe: an unreadable frequency table, an unavailable syscall, or an unsupported kernel
 * all report "skipped" and never crash.
 */

namespace pl::runtime {
namespace {

bool gApplied = false;
unsigned long long gAppliedMask = 0;

void ApplyCpuAffinity(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gApplied) {
    MarkOptifineActive(state, "main thread pinned to performance cores");
    return;
  }

  int coreIndices[kOptifineMaxCores] = {};
  const int performanceCount = ReadPerformanceCoreIndices(coreIndices, kOptifineMaxCores);
  if (performanceCount == 0) {
    MarkOptifineSkipped(state, "no performance-core frequency data");
    return;
  }
  if (!ShouldPinToPerformanceCores(performanceCount)) {
    MarkOptifineSkipped(
        state, std::to_string(performanceCount) + " performance cores (need 4+)");
    return;
  }

  const unsigned long long mask = BuildCoreMask(coreIndices, performanceCount);
  if (mask == 0) {
    MarkOptifineSkipped(state, "no pinnable cores");
    return;
  }

  // Pin the calling thread (the main/game thread) rather than a guessed tid: by the time this
  // runs the caller is already on the main thread, so gettid() is exactly the target.
  const pid_t self = static_cast<pid_t>(::gettid());
  if (!SetThreadAffinity(self, mask)) {
    MarkOptifineSkipped(state, "sched_setaffinity refused");
    preloaderLogger.warn("Optifine affinity: sched_setaffinity failed (errno {})", errno);
    return;
  }

  gApplied = true;
  gAppliedMask = mask;
  MarkOptifineActive(state,
                     std::to_string(performanceCount) + " performance cores pinned");
  preloaderLogger.info("Optifine affinity: main thread pinned, mask 0x{:x}", mask);
}

struct Registrar {
  Registrar() { RegisterOptifineItem(OptifineItem::CpuAffinity, false, &ApplyCpuAffinity); }
};
Registrar gRegistrar;

} // namespace
} // namespace pl::runtime
