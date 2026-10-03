#include "pl/runtime/GameResourcePackReload.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#include "pl/Logger.hpp"

namespace pl::runtime {
namespace {

// Resolution of a refresh hook happens once per process; the outcome is cached
// in g_refreshFn (0 = none). See the header for why the shipped builds resolve
// to "none".
std::once_flag g_resolveOnce;
std::atomic<std::uintptr_t> g_refreshFn{0};

// A reload is attempted at most once per session. Both an unsupported build and
// a failed call set this, so the same session never retries -- the caller
// relaunches instead.
std::atomic_bool g_attempted{false};

// Set only after the engine itself confirmed a refresh, never optimistically.
std::atomic_bool g_supported{false};

// A future verified hook plugs in here: resolve a per-build byte signature (or
// an RTTI/vtable slot) for a refresh that is safe on a live session, store the
// address in g_refreshFn, and implement the call + engine-side confirmation in
// ReloadResourcePacks. It must NOT substitute ContentManager::reloadSources:
// that path is guarded by "mReloadSourcesAsync->isInitTaskCompleted()" and
// throws during a live session, and ResourcePackManager's stack operations are
// only reached from world setup/teardown.
void ResolveRefreshHook() { g_refreshFn.store(0, std::memory_order_relaxed); }

} // namespace

bool IsResourcePackReloadSupported() {
  return g_supported.load(std::memory_order_relaxed);
}

bool ReloadResourcePacks() {
  if (g_attempted.exchange(true, std::memory_order_acq_rel)) {
    // Already tried this session: an unsupported build stays unsupported and a
    // failed call is not retried, so a bad frame cannot loop the game.
    return false;
  }

  std::call_once(g_resolveOnce, ResolveRefreshHook);

  const std::uintptr_t refreshFn = g_refreshFn.load(std::memory_order_relaxed);
  if (refreshFn == 0) {
    preloaderLogger.info(
        "Resource-pack live reload: no verified in-place refresh hook for this "
        "build; the launcher should relaunch the instance");
    return false;
  }

  // When a verified hook exists it is invoked here and the engine's own signal
  // (the active pack re-read, or a reload counter it exposes) is checked before
  // success is reported. Only a confirmed refresh sets g_supported, so a build
  // that silently no-ops is never marked supported and is never retried.
  //
  // The shipped builds resolve to no hook, so this branch is intentionally
  // unreachable today; see the header for the reverse-engineering finding.
  preloaderLogger.warn(
      "Resource-pack live reload: a refresh hook resolved but no verified "
      "engine confirmation is implemented; reporting failure");
  return false;
}

} // namespace pl::runtime
