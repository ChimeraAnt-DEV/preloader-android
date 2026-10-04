#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @file OptifineMode.h
 * @brief "Bedrock Optifine Mode" host-side optimizations.
 *
 * A suite of host-level performance changes the preloader applies to the process the game
 * runs in. It is split into two tiers by risk:
 *
 *  - **Tier 1** touches only the process/host (allocator, thread scheduling, CPU affinity,
 *    display refresh). It installs no game-code hook, so it cannot change game behaviour and
 *    is safe to default ON.
 *  - **Tier 2** hooks game functions found by pattern scan. A wrong or absent pattern is
 *    detected at runtime and the item is skipped, never partially applied.
 *
 * Every item is independently toggleable so a user can isolate one that misbehaves on their
 * device. The configuration arrives from the launcher as a flat key=value blob (see
 * {@link ConfigureOptifineMode}) rather than one JNI call per option, so the two sides cannot
 * drift out of sync field-by-field.
 *
 * **Honesty contract.** This module never claims a frame rate. It reports only what it
 * actually did ("allocator: mimalloc", "render thread: SCHED_FIFO 1") or why it did nothing
 * ("render thread: not found yet", "affinity: 2 perf cores < 4"). The launcher renders those
 * strings verbatim.
 */
namespace pl::runtime {

/** Stable ids for each optimization. Persisted and shown to the user; never renumber. */
enum class OptifineItem : int {
  Allocator = 0,        //!< Tier 1: mimalloc interposition.
  RenderPriority = 1,   //!< Tier 1: render-thread SCHED_FIFO boost.
  CpuAffinity = 2,      //!< Tier 1: pin the main thread to performance cores.
  RefreshRate = 3,      //!< Tier 1: ask for the panel's native refresh rate.
  EntityCulling = 4,    //!< Tier 2: skip off-screen/occluded entity rendering.
  ParticleCulling = 5,  //!< Tier 2: drop distant/ambient particles.
  DynamicRenderDistance = 6, //!< Tier 2: reduce render distance under FPS pressure.
  CallbackTrimming = 7, //!< Tier 2: drop callbacks that never fire meaningfully.
  OreUiStripping = 8,   //!< Tier 2: skip the OreUI overlay when no menu is open.
  Count = 9,
};

/** One item's configuration and reported outcome. */
struct OptifineItemState {
  bool enabled{false};
  bool tier2{false};
  /**
   * 0 = not attempted, 1 = active/installed, 2 = skipped (with {@link detail}), 3 = failed.
   * The distinction between "skipped" and "failed" matters: a skipped item is expected on some
   * devices (e.g. affinity with too few cores), a failure is unexpected and worth surfacing.
   */
  int status{0};
  /** Short, user-facing explanation of the current status. */
  std::string detail;
  /** Whether the game must be restarted for a change to take effect. */
  bool needsRestart{false};
};

/**
 * Signature an item implementation registers with. Called with {@code enabled == true} only.
 * May throw; the caller reports the outcome as a failure.
 */
using ItemHandler = void (*)(bool enabled, OptifineItemState &state);

/** The full suite state, indexed by {@link OptifineItem}. */
struct OptifineState {
  bool masterEnabled{false};
  OptifineItemState items[static_cast<int>(OptifineItem::Count)];
};

/**
 * @brief Applies the configuration from the launcher.
 *
 * @param blob Newline-separated {@code key=value} pairs. Recognised keys:
 *             {@code master}, {@code enabled.<itemId>}, {@code crash.<itemId>} (the
 *             consecutive-crash counter for that item). Unknown keys are ignored, so a newer
 *             launcher can send a key an older preloader does not know.
 *
 * Safe to call repeatedly; a later call replaces the previous configuration. Items that were
 * turned off are torn down (e.g. the allocator is not un-installed, but its state is reported
 * as inactive) and items turned on are installed.
 */
void ConfigureOptifineMode(std::string_view blob);

/** @brief The current state, for the launcher to render. */
OptifineState ReadOptifineState();

/** @brief True when the master switch is on and at least one item is active. */
bool IsOptifineModeActive();

/** @brief Applies host-side items that do not need the game to have started yet. */
void ApplyOptifineModeEarly();

/** @brief The id string used in the config blob and by the launcher. */
std::string_view OptifineItemId(OptifineItem item);

/** @brief A stable human-readable name for an item. */
std::string_view OptifineItemName(OptifineItem item);

/**
 * @brief Registers an item's apply handler.
 *
 * Internal API for the item implementation files, called once at static-init time. The handler
 * is invoked with {@code enabled == true} only (a disabled item is never called); it must set
 * {@code state.status}/{@code state.detail} to describe the outcome and may throw, in which case
 * the outcome is reported as a failure.
 *
 * @param item   which optimization the handler implements
 * @param tier2  true for a game-hook item (defaults OFF in the launcher's UI)
 * @param handler the apply function
 */
void RegisterOptifineItem(OptifineItem item, bool tier2, ItemHandler handler);

/**
 * @brief Marks an item's outcome as active, skipped or failed.
 *
 * These live here, next to {@link OptifineItemState}, rather than in the Tier-2 hook-support
 * header: both tiers report through the same three states, and a Tier-1 file must not have to
 * include the Tier-2 hook header (and its game-module constants) just to set a status.
 */
void MarkOptifineActive(OptifineItemState &state, std::string detail);

/** @copydoc MarkOptifineActive */
void MarkOptifineSkipped(OptifineItemState &state, std::string reason);

/** @copydoc MarkOptifineActive */
void MarkOptifineFailed(OptifineItemState &state, std::string reason);

/**
 * @brief Records that this item's hook was installed, for crash-loop tracking.
 *
 * The launcher persists which items were active; if the next launch crashes before the game
 * reaches a world, the launcher increments the crash counter and disables an item on the third
 * consecutive crash. This call only reports the set that was applied.
 */
std::string AppliedOptifineItemsBlob();

/** @brief Records the refresh rate the launcher requested (Tier-1 refresh item). */
void SetOptifineRefreshRateTarget(int hz);

/** @brief Configures the dynamic-render-distance governor bounds. */
void ConfigureOptifineRenderDistance(int minDistance, int maxDistance, int fpsThreshold);

/** @brief The tick count observed by the callback-trimming hook (0 when not installed). */
std::uint64_t OptifineTickCount();

/** @brief The HUD update count observed by the OreUI-stripping hook (0 when not installed). */
std::uint64_t OptifineHudUpdateCount();


} // namespace pl::runtime
