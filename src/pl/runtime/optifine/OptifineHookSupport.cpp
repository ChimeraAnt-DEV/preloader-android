#include "pl/runtime/optifine/OptifineHookSupport.h"

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/GameHookRules.h"

namespace pl::runtime {

std::uintptr_t ResolveOptifineSlot(const char *typeInfoName, std::size_t slot) {
  if (typeInfoName == nullptr) return 0;
  return pl::memory::resolveVtableFunction(typeInfoName, slot, kOptifineGameModule);
}

bool InstallOptifineHook(std::uintptr_t slotAddress, void *detour, void **original,
                         const char *label) {
  if (slotAddress == 0 || detour == nullptr || original == nullptr) return false;
  return pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(slotAddress), detour,
                          reinterpret_cast<pl::memory::FuncPtr *>(original),
                          pl::memory::HookPriority::Normal,
                          label == nullptr ? "optifine" : label) == 0;
}

bool InstallTier2CountingHook(const char *typeInfoName, std::size_t slot,
                              const char *label, void *detour, void **original,
                              OptifineItemState &state) {
  if (typeInfoName == nullptr || label == nullptr || detour == nullptr) {
    MarkOptifineSkipped(state, "internal: missing hook target");
    return false;
  }

  std::size_t effectiveSlot = slot;
  if (effectiveSlot == 0) {
    // No verified slot for the running version. This is the honest "pattern scan failed"
    // case: report it and leave the game untouched rather than guessing a slot.
    MarkOptifineSkipped(state,
                        std::string(label) + ": no verified hook slot for this version");
    preloaderLogger.warn("Optifine {}: no verified vtable slot for this build", label);
    return false;
  }

  const std::uintptr_t address = ResolveOptifineSlot(typeInfoName, effectiveSlot);
  if (address == 0) {
    MarkOptifineSkipped(state,
                        std::string(label) + ": vtable slot unresolved in this build");
    preloaderLogger.warn("Optifine {}: vtable slot {} unresolved for {}", label,
                         effectiveSlot, typeInfoName);
    return false;
  }

  if (!InstallOptifineHook(address, detour, original, label)) {
    MarkOptifineFailed(state, std::string(label) + ": hook install failed");
    preloaderLogger.warn("Optifine {}: detour install failed at 0x{:x}", label, address);
    return false;
  }

  MarkOptifineActive(state, std::string(label) + ": hook installed");
  preloaderLogger.info("Optifine {}: hook installed at 0x{:x} (slot {})", label, address,
                       effectiveSlot);
  return true;
}

std::size_t ReadOptifineHookSlot(const char *key) {
  if (key == nullptr) return 0;
  return pl::runtime::ReadConfiguredOptifineSlot(key);
}

} // namespace pl::runtime
