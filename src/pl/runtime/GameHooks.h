#pragma once

#include <string>

namespace pl::runtime {

void ConfigureGameHooks(std::string rulesPath, std::string minecraftVersion);

/**
 * @brief Arms the deferred hook install.
 *
 * <p>Called from the preloader's mod-enable entry, which the game activity reaches in
 * {@code onCreate} — before the engine's graphics backend and the {@code libminecraftpe.so}
 * relocation have finished. The install itself runs from {@link PumpGameHookInstall} on the game's
 * own frame tick, so the RTTI/vtable resolution sees a fully mapped module and the code patch runs
 * on the same thread that calls the render entry. Calling this more than once is a no-op.
 */
void ArmGameHookInstall();

/**
 * @brief Runs the deferred hook install if the runtime is now up. Safe to call every frame.
 *
 * @return true once the install has run (installed or given up).
 */
bool PumpGameHookInstall();

/** True once the deferred install has produced a decision. */
bool GameHookInstallResolved();

/** True when the deferred install has not yet run (diagnostics: "waiting for the render context"). */
bool GameHookInstallPending();

/**
 * @brief The deferred install's state: 0 not armed, 1 armed and waiting, 2 resolved.
 *
 * A single int for the diagnostics report, so "the install never ran" is distinguishable from
 * "it ran and gave up".
 */
int GameHookInstallState();

/** Frame ticks the install gate observed before deciding (diagnostics). */
int GameHookInstallFrameTicks();

/**
 * @brief Installs immediately, for tests and callers that already know the runtime is up.
 *
 * <p>Idempotent: the install runs at most once per process.
 */
void InitGameHooks();

bool IsPauseMenuOpen();
bool IsHudScreenOpen();
bool IsShowingMenu();
bool ShouldForceGlobalModMenu();

} // namespace pl::runtime
