#pragma once

/**
 * @file GameResourcePackReload.h
 * @brief Runtime-detected, fail-safe in-place resource-pack refresh.
 *
 * Bedrock keeps its active resource-pack stack in {@code ResourcePackManager},
 * built and swapped by {@code ResourcePackRepository} during world load and
 * teardown. The launcher wants to apply an equipped cape/hat/pet without making
 * the player leave their world, so it asks here for a live re-scan.
 *
 * This surface is deliberately honest about what the shipped build supports.
 * Reverse-engineering of {@code libminecraftpe.so} (1.26.50.4 and 1.26.60.28)
 * found the refresh entry points -- {@code ContentManager::reloadSources},
 * {@code ResourcePackManager::setStack}/{@code _doStackOperation},
 * {@code RepositoryLoading::refreshPacks}/{@code reloadUserPacks} -- but no
 * safe, verifiable way to invoke them on a live session from outside the game:
 *
 *  - {@code ContentManager::reloadSources} is guarded by
 *    "mReloadSourcesAsync->isInitTaskCompleted()" and throws when the async
 *    init task has not completed, which is exactly the live-session state.
 *  - {@code ResourcePackManager}'s stack operations are const accessors with no
 *    visible trigger, and {@code setStack} is reached only from world
 *    setup/teardown.
 *  - Game classes export no symbols ({@code dlsym} cannot reach them), so a
 *    hook needs a per-build byte signature and there is no ABI contract to
 *    call a C++ method safely.
 *
 * Until a per-build signature for a genuinely in-place refresh is derived and
 * verified on device, this returns false and the caller falls back to relaunch.
 * The detection is runtime, not a hardcoded version list: the first confirmed
 * reload marks the running build supported in memory, and a failure is never
 * retried in the same session.
 */
namespace pl::runtime {

/**
 * @brief True when the running build has been confirmed to support a live
 *        in-place resource-pack reload this session.
 *
 * Starts false and only becomes true after a reload that the engine itself
 * confirmed. Callers use it to avoid re-attempting a reload that this build
 * cannot perform.
 */
bool IsResourcePackReloadSupported();

/**
 * @brief Asks the running game to re-read its active resource-pack stack.
 *
 * Fail-safe by contract: it never throws, never partially applies, and never
 * retries within a session. When no verified refresh hook exists for the
 * running build it returns false immediately so the caller can relaunch the
 * instance instead.
 *
 * @return true only when the engine confirmed the pack stack was re-scanned;
 *         false when the build is unsupported, the hook is unresolved, or the
 *         engine did not confirm the refresh.
 */
bool ReloadResourcePacks();

} // namespace pl::runtime
