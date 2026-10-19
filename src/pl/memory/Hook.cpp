#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#include "pl/Gloss.h"
#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/HookTarget.hpp"
#include "pl/memory/PrologueVerify.hpp"

namespace pl::memory {

namespace {

// The post-install prologue read-back rule lives in PrologueVerify.hpp so it is host-testable.
// This wrapper does the (unsafe) read and hands the first instruction to the pure classifier.
bool VerifyPatchedPrologue(FuncPtr target, std::string_view name) {
  (void)name;
  if (target == nullptr) return false;
  // Volatile read: the mapping is executable but may not be cache-coherent with the writer yet; a
  // plain load can be reordered or elided by an optimizing build.
  const volatile std::uint32_t *word =
      reinterpret_cast<const volatile std::uint32_t *>(target);
  return ClassifyPrologue(*word) == PrologueVerdict::Patched;
}

} // namespace

struct HookElement {
  FuncPtr detour{};
  FuncPtr *originalFunc{};
  int priority{};
  int id{};

  bool operator<(const HookElement &o) const noexcept {
    if (priority != o.priority) {
      return priority < o.priority;
    }
    return id < o.id;
  }
};

struct HookData {
  FuncPtr target{};
  FuncPtr origin{};
  FuncPtr start{};
  GHook glossHandle{};
  int counter{};
  std::set<HookElement> chain;

  ~HookData() {
    if (glossHandle) {
      GlossHookDelete(glossHandle);
    }
  }

  int nextId() noexcept { return ++counter; }

  void rebuildChain() {
    FuncPtr *prev = nullptr;
    for (auto &e : chain) {
      if (!prev) {
        start = e.detour;
        prev = e.originalFunc;
        *prev = origin;
      } else {
        *prev = e.detour;
        prev = e.originalFunc;
      }
    }

    if (prev) {
      *prev = origin;
    } else {
      start = origin;
    }

    if (glossHandle) {
      GlossHookReplaceNewFunc(glossHandle, start);
    }
  }
};

std::unordered_map<FuncPtr, std::shared_ptr<HookData>> &hooks() {
  static std::unordered_map<FuncPtr, std::shared_ptr<HookData>> m;
  return m;
}

std::mutex mtx;

int hook(FuncPtr target, FuncPtr detour, FuncPtr *original,
         HookPriority priority, std::string_view name) {
  if (!target || !detour || !original) {
    preloaderLogger.error("hook({}): refused, null target/detour/original", name);
    return -1;
  }

  // Validate the target before anything is written. A detour on an unmapped or
  // mis-aligned address corrupts the instruction stream and faults the thread that
  // later executes it, so refuse it here rather than half-installing.
  if (!isHookAddressSane(reinterpret_cast<uintptr_t>(target))) {
    preloaderLogger.error("hook({}): refused, target {} is null or mis-aligned",
                          name, target);
    return -1;
  }
  const auto region = queryMemoryRegion(reinterpret_cast<uintptr_t>(target));
  if (!region.readable() || !region.executable()) {
    preloaderLogger.error(
        "hook({}): refused, target {} is not readable+executable ({})", name,
        target, region.describe());
    return -1;
  }
  preloaderLogger.info("hook({}): target {} -> {}", name, target,
                       region.describe());

  // GlossInit(false) allocates the trampoline pool and hook bookkeeping. It must NOT be
  // GlossInit(true): that additionally runs LinkerInit(), which re-hooks the Android
  // linker's own functions (do_dlopen/do_dlsym) via an inline hook -- an operation that
  // faults on this build (see the "Start hook linker..." -> SIGSEGV sequence). The
  // inline-hook path used below is independent of the linker init, so false is correct.
  static bool inited = false;
  if (!inited) {
    // Surface GlossHook's own "Hook success !" / "Hook failed !" and trampoline-allocation
    // lines, so logcat pinpoints the failing install and whether the trampoline came up.
    GlossEnableLog(true);
    GlossInit(false);
    inited = true;
  }

  std::lock_guard<std::mutex> lock(mtx);
  auto &map = hooks();
  auto it = map.find(target);

  if (it != map.end()) {
    auto h = it->second;
    h->chain.insert(
        {detour, original, static_cast<int>(priority), h->nextId()});
    h->rebuildChain();
    preloaderLogger.info("hook({}): chained onto existing target {} (original {})",
                         name, target, h->origin);
    return 0;
  }

  auto h = std::make_shared<HookData>();
  h->target = target;
  h->origin = target;

  h->glossHandle = GlossHook(reinterpret_cast<void *>(target),
                             reinterpret_cast<void *>(detour),
                             reinterpret_cast<void **>(&h->origin));
  if (!h->glossHandle) {
    preloaderLogger.error("hook({}): GlossHook install failed at {}", name, target);
    return -1;
  }

  // Item 7: confirm the jump is actually in place. Read back the first instruction at the target
  // and check it is a real branch, not the unpatched original or a NOP. This catches a hook backend
  // that reported success but wrote nothing (or wrote to the wrong address). A failed check is not
  // merely logged: the install is backed out, because a half-patched entry is worse than no hook
  // (the game would call a trampoline whose prologue was never rewritten) and the caller must see a
  // failure so it fails closed.
  if (!VerifyPatchedPrologue(target, name)) {
    preloaderLogger.error(
        "hook({}): patched-prologue verification FAILED at {}; backing the install out", name,
        target);
    GlossHookDelete(h->glossHandle);
    h->glossHandle = nullptr;
    return -1;
  }

  preloaderLogger.info(
      "hook({}): installed, target {} original {} detour {} (region {})", name,
      target, h->origin, detour, queryMemoryRegion(reinterpret_cast<uintptr_t>(target)).describe());

  h->chain.insert({detour, original, static_cast<int>(priority), h->nextId()});
  h->rebuildChain();
  map[target] = h;
  return 0;
}

int hook(FuncPtr target, FuncPtr detour, FuncPtr *original) {
  return hook(target, detour, original, HookPriority::Normal, {});
}

int hook(FuncPtr target, FuncPtr detour, FuncPtr *original, HookPriority priority) {
  return hook(target, detour, original, priority, {});
}

bool unhook(FuncPtr target, FuncPtr detour) {
  std::lock_guard<std::mutex> lock(mtx);
  auto &map = hooks();
  auto it = map.find(target);
  if (it == map.end()) {
    return false;
  }

  auto &h = it->second;
  bool removed = false;
  for (auto eit = h->chain.begin(); eit != h->chain.end(); ++eit) {
    if (eit->detour == detour) {
      h->chain.erase(eit);
      removed = true;
      break;
    }
  }

  if (!removed) {
    return false;
  }

  if (h->chain.empty()) {
    map.erase(it);
  } else {
    h->rebuildChain();
  }
  return true;
}

} // namespace pl::memory
