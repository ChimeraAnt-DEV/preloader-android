#pragma once

#include <cstdint>

/**
 * @file OptifineDeviceRules.h
 * @brief Pure rules that decide what the host optimizations may do on a device.
 *
 * Kept free of Android and syscalls so the decisions are unit-testable on the host: a wrong
 * "pin to these cores" rule is otherwise only visible as a device that boots but stutters.
 */
namespace pl::runtime {

/** Highest core count this suite will ever consider pinning to. */
constexpr int kOptifineMaxCores = 64;

/** Minimum number of performance cores before CPU pinning is worth doing. */
constexpr int kOptifineMinPerformanceCores = 4;

/** A core counts as "performance" when its max frequency is at least this fraction of the
 *  highest core's. 0.9 excludes little cores clocked well below the big cluster. */
constexpr double kOptifinePerformanceCoreRatio = 0.9;

/**
 * @brief Counts the performance cores in a frequency table.
 *
 * @param maxFreqKhz per-core maximum frequency in kHz; 0 means unknown for that core.
 * @param count number of entries.
 * @return the number of cores at or above {@link kOptifinePerformanceCoreRatio} of the fastest
 *         core, or 0 when no core has a known frequency (the caller must then skip pinning).
 */
int CountPerformanceCores(const std::uint64_t *maxFreqKhz, int count);

/**
 * @brief Whether the device has enough performance cores to make pinning worthwhile.
 */
bool ShouldPinToPerformanceCores(int performanceCoreCount);

/**
 * @brief Builds a CPU affinity mask from the indices of the cores to pin to.
 *
 * @param coreIndices core indices to include (values outside 0..63 are ignored).
 * @param count number of indices.
 * @return a bitmask with one bit per selected core, or 0 when nothing valid was selected.
 */
std::uint64_t BuildCoreMask(const int *coreIndices, int count);

/**
 * @brief The refresh rate the suite should request.
 *
 * @param supportedHz the display's supported refresh rates, ascending; may be empty.
 * @param count number of entries.
 * @param currentHz the rate the game is currently using, or 0 if unknown.
 * @return the highest supported rate above {@code currentHz}, or 0 when there is nothing to
 *         unlock (a 60Hz-only panel, an unknown list, or the game already at the top rate).
 */
int SelectTargetRefreshRate(const int *supportedHz, int count, int currentHz);

} // namespace pl::runtime
