#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @file OptifineConfig.h
 * @brief Pure configuration rules for Bedrock Optifine Mode.
 *
 * Deliberately free of Android, logging and syscalls so the rules that decide whether an
 * optimization runs are unit-testable on the host. A regression in these rules is otherwise
 * invisible until a device boot-loops, so they are kept in one place and pinned by tests.
 */
namespace pl::runtime {

/** Number of optimizations. Mirrors {@code OptifineItem::Count}. */
constexpr int kOptifineItemCount = 9;

/**
 * @brief Bounds on the launcher's config blob (item 20).
 *
 * The blob crosses a JNI boundary from the launcher, so a buggy or hostile build could hand the
 * preloader a multi-megabyte string, a million keys or a single kilobyte key. The parser stores
 * every key in a map, so an unbounded blob is unbounded memory. These caps are generous for the
 * real blob (9 items x 3 keys plus the master switch is 28 keys, well under 1 KiB) and a blob past
 * them is parsed up to the limit and then ignored, never rejected outright - the launcher and the
 * preloader must stay compatible if one side adds a key.
 */
constexpr std::size_t kOptifineMaxBlobBytes = 16U * 1024U;
/** The most keys the parser will store; later keys are ignored. */
constexpr std::size_t kOptifineMaxKeys = 128U;
/** A key longer than this is skipped rather than stored. */
constexpr std::size_t kOptifineMaxKeyLength = 64U;
/** A value longer than this is truncated before it is stored. */
constexpr std::size_t kOptifineMaxValueLength = 256U;

/**
 * @brief The stable id string for an item index, used in the config blob and by the launcher.
 * Returns "unknown" for an out-of-range index. Never renumber an existing id.
 */
std::string_view OptifineItemIdAt(int index);

/** What to do with one item, given the master switch, its toggle and its crash history. */
enum class OptifineDecision {
  /** Master off or the item's own toggle off: do nothing. */
  Off,
  /** Install/apply the item. */
  Apply,
  /** The item crashed the game too many times in a row: skip it and report why. */
  AutoDisabled,
};

/** Parsed configuration blob. */
struct OptifineConfig {
  bool master{false};
  bool enabled[kOptifineItemCount]{};
  /** Consecutive-crash counters reported by the launcher. */
  int crashCount[kOptifineItemCount]{};
};

/**
 * @brief Parses the launcher's flat {@code key=value} blob.
 *
 * Recognised keys: {@code master}, {@code enabled.<id>}, {@code crash.<id>}. Unknown keys and
 * malformed lines are ignored, so a newer launcher can send a key an older preloader does not
 * know without breaking the rest.
 */
OptifineConfig ParseOptifineConfig(std::string_view blob);

/**
 * @brief The decision for one item index.
 *
 * The crash-loop rule is the important one: an item that has crashed the game on three
 * consecutive launches is disabled even though its toggle says on, because a hook that kills
 * the process before any UI can appear is worse than a missing optimization.
 */
OptifineDecision DecideOptifineItem(const OptifineConfig &config, int index);

/** @brief The number of consecutive crashes after which an item is auto-disabled. */
constexpr int kOptifineCrashLoopThreshold = 3;

} // namespace pl::runtime
