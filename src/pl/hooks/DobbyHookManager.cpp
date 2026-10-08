/**
 * @file DobbyHookManager.cpp
 * @brief Hook manager implementation (Task 2).
 *
 * Resolution goes through GlossHook's xdl-based `GlossOpen`/`GlossSymbol`/`GlossGetLibBias` (the
 * same reason Dobby ships an xdl-based symbol resolver: `dlsym` cannot see a stripped module's
 * debug symbols). Installation goes through {@link pl::memory::hook}, which is the engine's proven
 * GlossHook trampoline.
 */

#include "pl/hooks/DobbyHookManager.hpp"

#include <mutex>
#include <unordered_map>

#include "pl/Gloss.h"
#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/HookTarget.hpp"

namespace pl::hooks {

void *ArgRegisters::at(int index) const {
  switch (index) {
    case 0:
      return x0;
    case 1:
      return x1;
    case 2:
      return x2;
    case 3:
      return x3;
    case 4:
      return x4;
    case 5:
      return x5;
    case 6:
      return x6;
    case 7:
      return x7;
    default:
      return nullptr;
  }
}

namespace {

std::mutex &handleMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, GHandle> &handleCache() {
  static std::unordered_map<std::string, GHandle> cache;
  return cache;
}

/** @brief Opens (and caches) a module handle. Returns null when the module is not loaded. */
GHandle openModule(std::string_view module) {
  std::lock_guard<std::mutex> lock(handleMutex());
  std::string key(module);
  auto it = handleCache().find(key);
  if (it != handleCache().end()) return it->second;
  GHandle handle = GlossOpen(key.c_str());
  if (handle != nullptr) handleCache().emplace(key, handle);
  return handle;
}

} // namespace

DobbyHookManager &DobbyHookManager::instance() {
  static DobbyHookManager manager;
  return manager;
}

uintptr_t DobbyHookManager::resolve(std::string_view module, std::string_view symbol) {
  GHandle handle = openModule(module);
  if (handle == nullptr) {
    preloaderLogger.warn("DobbyHookManager: module '{}' not loaded; cannot resolve '{}'",
                         std::string(module), std::string(symbol));
    return 0;
  }
  std::string name(symbol);
  size_t size = 0;
  return GlossSymbol(handle, name.c_str(), &size);
}

uintptr_t DobbyHookManager::resolveOffset(std::string_view module, uintptr_t offset) {
  GHandle handle = openModule(module);
  if (handle == nullptr) {
    preloaderLogger.warn("DobbyHookManager: module '{}' not loaded; cannot resolve offset 0x{:x}",
                         std::string(module), offset);
    return 0;
  }
  const uintptr_t bias = GlossGetLibBiasEx(handle);
  if (bias == 0) {
    preloaderLogger.warn("DobbyHookManager: module '{}' load bias unavailable", std::string(module));
    return 0;
  }
  return bias + offset;
}

bool DobbyHookManager::isAddressSane(uintptr_t address) {
  return pl::memory::isHookAddressSane(address);
}

std::string DobbyHookManager::describeTarget(uintptr_t address) {
  return pl::memory::queryMemoryRegion(address).describe();
}

HookStatus DobbyHookManager::hook(void *target, void *detour, void **original,
                                  std::string_view name) {
  if (target == nullptr || detour == nullptr) {
    return HookStatus::Unsupported;
  }
  const auto address = reinterpret_cast<uintptr_t>(target);
  if (!isAddressSane(address)) {
    preloaderLogger.warn("DobbyHookManager: refusing mis-aligned target for '{}'", std::string(name));
    return HookStatus::Failed;
  }

  const int result =
      pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target),
                       reinterpret_cast<pl::memory::FuncPtr>(detour),
                       reinterpret_cast<pl::memory::FuncPtr *>(original),
                       pl::memory::HookPriority::Normal, name);
  if (result != 0) {
    preloaderLogger.warn("DobbyHookManager: install failed for '{}' at 0x{:x} ({})",
                         std::string(name), address, describeTarget(address));
    return HookStatus::Failed;
  }
  preloaderLogger.info("DobbyHookManager: hooked '{}' at 0x{:x}", std::string(name), address);
  return HookStatus::Installed;
}

bool DobbyHookManager::unhook(void *target, void *detour) {
  if (target == nullptr || detour == nullptr) return false;
  return pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(target),
                            reinterpret_cast<pl::memory::FuncPtr>(detour));
}

uintptr_t DobbySymbolResolver(const char *imageName, const char *symbolName) {
  if (imageName == nullptr || symbolName == nullptr) return 0;
  return DobbyHookManager::instance().resolve(imageName, symbolName);
}

HookStatus DobbyHook(void *address, void *replace, void **result) {
  return DobbyHookManager::instance().hook(address, replace, result, "DobbyHook");
}

bool DobbyUnhook(void *address, void *replace) {
  return DobbyHookManager::instance().unhook(address, replace);
}

} // namespace pl::hooks
