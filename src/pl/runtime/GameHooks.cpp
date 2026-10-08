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

namespace pl::runtime {
namespace {

std::atomic_bool g_isPauseMenuOpen{false};
std::atomic_bool g_isHudScreenOpen{false};
std::atomic_bool g_isShowingMenu{false};
std::atomic_bool g_forceGlobalModMenu{false};
std::once_flag g_gameHooksOnce;

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
  std::call_once(g_gameHooksOnce, [] {
    g_forceGlobalModMenu.store(false, std::memory_order_relaxed);
    // Hard guard: never install a detour against an unresolved address. When the launcher has
    // not delivered the signature rules -- the library was loaded after the rules call, a stale
    // build, or a different process -- there is no verified target, and installing anyway faults
    // on the render thread (SIGSEGV right after "Start hook linker"). Fail safe to a vanilla
    // launch instead. This is checked before every hook below, including the local-player feed.
    if (!GameHookRulesConfigured()) {
      preloaderLogger.warn(
          "Preloader signature rules were not delivered; refusing to install hooks and "
          "launching vanilla");
      return;
    }

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
