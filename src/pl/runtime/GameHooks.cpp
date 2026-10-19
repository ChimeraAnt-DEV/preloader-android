#include "pl/runtime/GameHooks.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/cosmetics/NativeRenderHook.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameLocalPlayer.h"
#include "pl/runtime/GamePlayerRender.h"
#include "pl/runtime/HookInstallGate.hpp"

namespace pl::runtime {
namespace {

std::atomic_bool g_isPauseMenuOpen{false};
std::atomic_bool g_isHudScreenOpen{false};
std::atomic_bool g_isShowingMenu{false};
std::atomic_bool g_forceGlobalModMenu{false};
std::once_flag g_gameHooksOnce;
std::once_flag g_armOnce;

// The deferred-install gate. Armed at preloader init; ticked from the game's frame pump. Guarded by
// an atomic flag so the common per-frame path is a single load and returns.
HookInstallGate g_installGate;
std::atomic_bool g_installArmed{false};
std::atomic_bool g_installResolved{false};
std::atomic_int g_installGiveUps{0};

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

void (*orig_PauseMenuDtor)(void *) = nullptr;
void hook_PauseMenuDtor(void *_this) {
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  if (orig_PauseMenuDtor) {
    orig_PauseMenuDtor(_this);
  }
}

void (*orig_PauseMenuOpen)(void *) = nullptr;
void hook_PauseMenuOpen(void *_this) {
  g_isPauseMenuOpen.store(true, std::memory_order_relaxed);
  g_isShowingMenu.store(true, std::memory_order_relaxed);
  if (orig_PauseMenuOpen) {
    orig_PauseMenuOpen(_this);
  }
}

void (*orig_HudScreenDtor)(void *) = nullptr;
void hook_HudScreenDtor(void *_this) {
  g_isHudScreenOpen.store(false, std::memory_order_relaxed);
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  if (orig_HudScreenDtor) {
    orig_HudScreenDtor(_this);
  }
}

void (*orig_HudScreenOpen)(void *) = nullptr;
void hook_HudScreenOpen(void *_this) {
  g_isHudScreenOpen.store(true, std::memory_order_relaxed);
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  if (orig_HudScreenOpen) {
    orig_HudScreenOpen(_this);
  }
}

bool (*orig_isShowingMenu)(void *) = nullptr;
bool hook_isShowingMenu(void *_this) {
  bool res = false;
  if (orig_isShowingMenu) {
    res = orig_isShowingMenu(_this);
  }
  g_isShowingMenu.store(res, std::memory_order_relaxed);
  return res;
}

uintptr_t ResolveResult(
    const std::unordered_map<std::string, uintptr_t> &results,
    const std::string &signature) {
  auto it = results.find(signature);
  return it == results.end() ? 0 : it->second;
}

bool InstallHook(uintptr_t target, pl::memory::FuncPtr detour,
                 pl::memory::FuncPtr *original,
                 const char *name) {
  if (!target) {
    preloaderLogger.warn("Preloader hook target is missing: {}", name);
    return false;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target), detour,
                       original, pl::memory::HookPriority::Normal, name) != 0) {
    preloaderLogger.warn("Failed to install Preloader hook: {}", name);
    return false;
  }
  return true;
}

// One hook in a batch. Kept together with its rollback inputs so an install that
// fails part-way can be fully undone instead of leaving the process half-patched.
struct PendingHook {
  uintptr_t target;
  pl::memory::FuncPtr detour;
  pl::memory::FuncPtr *original;
  const char *name;
};

bool InstallHookBatch(const std::vector<PendingHook> &batch) {
  std::vector<const PendingHook *> installed;
  installed.reserve(batch.size());

  for (const auto &h : batch) {
    if (!InstallHook(h.target, h.detour, h.original, h.name)) {
      preloaderLogger.error(
          "Preloader hook install aborted at '{}'; rolling back {} hook(s) already "
          "installed",
          h.name, installed.size());
      for (const auto *done : installed) {
        pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(done->target),
                           done->detour);
      }
      return false;
    }
    installed.push_back(&h);
  }
  return true;
}

} // namespace

void ConfigureGameHooks(std::string rulesPath, std::string minecraftVersion) {
  ConfigureGameHookRules(std::move(rulesPath), std::move(minecraftVersion));
  g_forceGlobalModMenu.store(false, std::memory_order_relaxed);
}

void InitGameHooks() {
  // Rules gate BEFORE the once, deliberately: if the launcher has not delivered the signature
  // rules yet (the library was loaded after the rules call, a stale build, or a different
  // process), install nothing AND do not consume the once. A later PumpGameHookInstall after
  // delivery then gets a clean attempt instead of permanently skipping cosmetics for the session.
  if (!GameHookRulesConfigured()) {
    preloaderLogger.warn(
        "Preloader signature rules were not delivered; deferring hook install (will retry "
        "when they arrive)");
    return;
  }

  std::call_once(g_gameHooksOnce, [] {
    g_installResolved.store(true, std::memory_order_release);
    g_forceGlobalModMenu.store(false, std::memory_order_relaxed);

    // The local-player feed is independent of the pause/HUD hooks and is useful on
    // its own (the voice nametag icons), so it is installed even when the overlay
    // hooks below cannot be resolved.
    InitLocalPlayerSource();
    auto signatures = LoadConfiguredGameHookSignatures();
    if (!signatures) {
      return;
    }

    // The player-render feed is independent of the overlay hooks: the native cosmetics path
    // uses it to know the renderer is live and to drive its physics off a real render tick. It
    // is fail-closed, so a build where the slot does not resolve simply keeps the pack path.
    InitPlayerRenderSource(signatures->playerRenderVtableIndex.value_or(0));

    // The native cosmetics render interception (Task 2). It hooks the same LivePlayerRenderer slot
    // the render feed above uses, which is why the hook backend chains: the two detours both run,
    // in install order. Ownership is fail-closed, so a build outside the verified range keeps the
    // resource-pack path.
    pl::cosmetics::InitNativeCosmeticRenderHook(
        signatures->playerRenderVtableIndex.value_or(0));

    // The native cosmetics seams (skin/cape retrieval and the texture binder). Like the render
    // feed they are fail-closed: a build whose slot does not resolve keeps the resource-pack path.
    InitCosmeticsHooks(0, 0, 0);

    std::vector<std::string> requestedSignatures{
        signatures->pauseMenuDtor, signatures->pauseMenuOpen,
        signatures->hudScreenDtor, signatures->hudScreenOpen};
    if (!signatures->isShowingMenu.empty()) {
      requestedSignatures.push_back(signatures->isShowingMenu);
    }
    auto results = pl::memory::resolveSignatures(
        requestedSignatures, "libminecraftpe.so");

    uintptr_t pauseDtor = ResolveResult(results, signatures->pauseMenuDtor);
    uintptr_t pauseOpen = ResolveResult(results, signatures->pauseMenuOpen);
    uintptr_t hudDtor = ResolveResult(results, signatures->hudScreenDtor);
    uintptr_t hudOpen = ResolveResult(results, signatures->hudScreenOpen);
    uintptr_t isShowingMenuAddr = 0;
    if (signatures->isShowingMenuVtableIndex) {
      isShowingMenuAddr = pl::memory::resolveVtableFunction(
          "14ClientInstance", *signatures->isShowingMenuVtableIndex,
          "libminecraftpe.so");
    } else if (!signatures->isShowingMenu.empty()) {
      isShowingMenuAddr = ResolveResult(results, signatures->isShowingMenu);
    }

    if (!pauseDtor || !pauseOpen || !hudDtor || !hudOpen ||
        !isShowingMenuAddr) {
      preloaderLogger.warn(
          "Preloader runtime data is incomplete; game-only overlays remain hidden");
      return;
    }

    // Install the overlay hooks as one atomic batch: abort on the first failure, roll
    // back the hooks already installed, and never leave the process half-patched.
    const std::vector<PendingHook> overlayHooks{
        {pauseDtor, (pl::memory::FuncPtr)hook_PauseMenuDtor,
         (pl::memory::FuncPtr *)&orig_PauseMenuDtor, "PauseMenuDtor"},
        {pauseOpen, (pl::memory::FuncPtr)hook_PauseMenuOpen,
         (pl::memory::FuncPtr *)&orig_PauseMenuOpen, "PauseMenuOpen"},
        {hudDtor, (pl::memory::FuncPtr)hook_HudScreenDtor,
         (pl::memory::FuncPtr *)&orig_HudScreenDtor, "HudScreenDtor"},
        {hudOpen, (pl::memory::FuncPtr)hook_HudScreenOpen,
         (pl::memory::FuncPtr *)&orig_HudScreenOpen, "HudScreenOpen"},
        {isShowingMenuAddr, (pl::memory::FuncPtr)hook_isShowingMenu,
         (pl::memory::FuncPtr *)&orig_isShowingMenu, "isShowingMenu"},
    };

    if (!InstallHookBatch(overlayHooks)) {
      preloaderLogger.warn(
          "Preloader runtime hooks are incomplete; game-only overlays remain hidden");
      return;
    }

    preloaderLogger.info("Preloader runtime hooks installed (5/5)");
  });
}

void ArmGameHookInstall() {
  std::call_once(g_armOnce, [] {
    g_installGate.start(MonotonicMs());
    g_installArmed.store(true, std::memory_order_release);
    preloaderLogger.info(
        "Preloader hook install armed; deferring until the game runtime is up");
  });
}

namespace {
// How many give-ups are tolerated before the install is marked resolved permanently. The gate's
// kGiveUpMs is a safety net for a stuck loading screen; a handful of retries with a backoff covers
// a runtime that starts slowly (or rules that arrive late) without spinning forever.
constexpr int kMaxGiveUps = 3;
constexpr long long kGiveUpBackoffMs = 5'000;
} // namespace

bool PumpGameHookInstall() {
  if (g_installResolved.load(std::memory_order_acquire)) return true;
  if (!g_installArmed.load(std::memory_order_acquire)) return false;

  const long long nowMs = MonotonicMs();
  const InstallDecision decision = g_installGate.tick(nowMs);
  if (decision == InstallDecision::Wait) return false;

  if (decision == InstallDecision::GiveUp) {
    // The runtime did not come up within the gate's budget. Retry after a backoff instead of
    // giving up permanently: a slow start, or signature rules delivered after the library was
    // loaded, can be resolved by a later attempt. Only after kMaxGiveUps is the state sealed,
    // leaving the game vanilla rather than patching something half-mapped.
    if (g_installGiveUps.fetch_add(1, std::memory_order_relaxed) + 1 >= kMaxGiveUps) {
      g_installResolved.store(true, std::memory_order_release);
      preloaderLogger.warn(
          "Preloader hook install gave up after {} attempts; the runtime did not come up",
          g_installGiveUps.load(std::memory_order_relaxed));
      return true;
    }
    g_installGate.start(nowMs + kGiveUpBackoffMs);
    preloaderLogger.warn(
        "Preloader hook install wait exceeded the gate budget after {} frame tick(s); retrying in "
        "{}ms",
        g_installGate.frameTicks(), kGiveUpBackoffMs);
    return false;
  }

  // Install path. InitGameHooks defers (returns without consuming the once) when the signature
  // rules have not been delivered yet, so a later frame tick retries; it is not a permanent skip.
  preloaderLogger.info("Preloader hook install running after {} frame tick(s)",
                       g_installGate.frameTicks());
  InitGameHooks();
  return GameHookInstallResolved();
}

bool GameHookInstallResolved() { return g_installResolved.load(std::memory_order_acquire); }

bool GameHookInstallPending() {
  return g_installArmed.load(std::memory_order_acquire)
      && !g_installResolved.load(std::memory_order_acquire);
}

int GameHookInstallState() {
  if (!g_installArmed.load(std::memory_order_acquire)) return 0;
  return g_installResolved.load(std::memory_order_acquire) ? 2 : 1;
}

int GameHookInstallFrameTicks() { return g_installGate.frameTicks(); }

bool IsPauseMenuOpen() {
  return g_isPauseMenuOpen.load(std::memory_order_relaxed);
}

bool IsHudScreenOpen() {
  return g_isHudScreenOpen.load(std::memory_order_relaxed);
}

bool IsShowingMenu() {
  return g_isShowingMenu.load(std::memory_order_relaxed);
}

bool ShouldForceGlobalModMenu() {
  return g_forceGlobalModMenu.load(std::memory_order_relaxed);
}

} // namespace pl::runtime
