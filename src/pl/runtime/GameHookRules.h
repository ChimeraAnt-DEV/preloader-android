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
  /**
   * The engine's own image loader, `mce::ImageUtils::loadImageFromMemory(mce::Image&, ImageFormat,
   * const unsigned char*, size_t, bool)`. Resolved by byte signature (the class exports no symbol)
   * so the cosmetics path can hand it custom cape PNG bytes and get back a valid `mce::Image`, which
   * is what SwapCapeImage needs. Empty when the build has no verified pattern.
   */
  std::string imageLoaderSig;
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

/**
 * @brief Reports whether the running Minecraft version falls within a verified range.
 *
 * A hook whose target was only confirmed against specific builds must not be installed on an
 * unverified one: an RTTI name resolves to a real function on every build, so a wrong vtable
 * index yields a valid-but-unrelated address, and hooking it detours whatever that slot happens
 * to be. The cosmetics player-render hook uses this to stay off on builds whose slot index has
 * not been verified. Comparison is numeric and component-wise, so a leading {@code 1.} and a
 * channel suffix do not affect it. An empty bound means "unbounded on that side"; an empty
 * running version never matches a bounded range.
 */
bool MatchesConfiguredVersion(const std::string &minVersion,
                              const std::string &maxVersion);

/**
 * @brief Reports whether signature rules have actually been delivered to this process.
 *
 * The launcher configures the rules before the hooks are installed. If that call was lost —
 * the preloader library was not loaded yet, a stale build, or a different process — the rules
 * path is empty and there is no verified target to hook. Installing a detour against an
 * unresolved address is what faults on the render thread, so hook installation refuses to run
 * when this returns false and the session launches vanilla instead.
 */
bool GameHookRulesConfigured();

} // namespace pl::runtime
