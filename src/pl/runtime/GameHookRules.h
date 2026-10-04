#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace pl::runtime {

struct GameHookSignatures {
  std::string pauseMenuDtor;
  std::string pauseMenuOpen;
  std::string hudScreenDtor;
  std::string hudScreenOpen;
  std::string isShowingMenu;
  std::optional<std::size_t> isShowingMenuVtableIndex;
  std::optional<std::size_t> playerRenderVtableIndex;
};

void ConfigureGameHookRules(std::string rulesPath, std::string minecraftVersion);
std::optional<GameHookSignatures> LoadConfiguredGameHookSignatures();

/**
 * @brief Reads a numeric vtable slot for an Optifine hook target from the version rules.
 *
 * The rules JSON carries one entry per verified target (e.g. {@code particleEngineVtableIndex});
 * a running version whose rule omits the key returns 0, which the caller treats as "no verified
 * hook for this build" rather than guessing a slot. This is the runtime-detected, never
 * version-allowlisted rule the Tier-2 items follow.
 *
 * @param key the rule key, e.g. {@code "particleEngineVtableIndex"}.
 * @return the slot, or 0 when unavailable.
 */
std::size_t ReadConfiguredOptifineSlot(const char *key);

} // namespace pl::runtime
