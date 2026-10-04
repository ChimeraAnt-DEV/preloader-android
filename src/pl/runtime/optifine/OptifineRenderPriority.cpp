#include <sched.h>
#include <sys/types.h>

#include <string>

#include "pl/Logger.hpp"
#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineProcess.h"

/**
 * @file OptifineRenderPriority.cpp
 * @brief Tier-1 render-thread priority boost.
 *
 * The game's render thread is found by name in {@code /proc/self/task}, then asked for
 * {@code SCHED_FIFO} at the lowest real-time priority (1). Priority 1 is deliberate: a
 * real-time thread that outranks everything can starve the rest of the process, so this asks
 * only to run ahead of normal threads, never to run the machine.
 *
 * Fail-safe: the game may not have started its render thread yet (the normal first pass), the
 * kernel may refuse real-time scheduling, or the thread may already be gone. Every case is
 * logged and reported as "deferred"/"skipped"; nothing here can crash. A retry on the next
 * tick picks the thread up once it exists.
 */

namespace pl::runtime {
namespace {

/** Thread names to try, in order. Different builds name the render thread differently. */
constexpr const char *kRenderThreadNames[] = {"Render", "render", "Minecraft Render",
                                              "RenderThread"};
/** Lowest real-time priority: ahead of normal threads, behind everything else that asks. */
constexpr int kRealtimePriority = 1;

pid_t gRenderTid = -1;
bool gApplied = false;

bool TryApplyToThread(pid_t tid, OptifineItemState &state) {
  int oldPolicy = 0;
  int oldPriority = 0;
  GetThreadScheduling(tid, oldPolicy, oldPriority);

  if (!SetThreadScheduling(tid, SCHED_FIFO, kRealtimePriority)) {
    // Refused by the kernel (no CAP_SYS_NICE) or the thread is gone. Report it honestly; the
    // game runs fine on the default scheduler.
    MarkOptifineSkipped(state, "render thread found, real-time scheduling refused");
    preloaderLogger.warn(
        "Optifine render priority: SCHED_FIFO refused for tid {} (errno {})", tid, errno);
    return false;
  }

  int newPolicy = 0;
  int newPriority = 0;
  GetThreadScheduling(tid, newPolicy, newPriority);
  gApplied = true;
  state.needsRestart = false;
  MarkOptifineActive(state, "render thread SCHED_FIFO priority 1");
  preloaderLogger.info(
      "Optifine render priority: tid {} {}->{} policy {}->{}", tid, oldPriority,
      newPriority, oldPolicy, newPolicy);
  return true;
}

void ApplyRenderPriority(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gApplied && gRenderTid > 0) {
    MarkOptifineActive(state, "render thread SCHED_FIFO priority 1");
    return;
  }

  for (const char *name : kRenderThreadNames) {
    const pid_t tid = FindThreadByName(name);
    if (tid > 0) {
      gRenderTid = tid;
      TryApplyToThread(tid, state);
      return;
    }
  }

  // The render thread is created when the game starts drawing, which is after this first
  // pass. Report "deferred" so the launcher retries rather than claiming success.
  MarkOptifineSkipped(state, "waiting for the render thread to start");
}

struct Registrar {
  Registrar() {
    RegisterOptifineItem(OptifineItem::RenderPriority, false, &ApplyRenderPriority);
  }
};
Registrar gRegistrar;

} // namespace
} // namespace pl::runtime
