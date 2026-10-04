#pragma once

#include <sys/types.h>

#include <string>

/**
 * @file OptifineProcess.h
 * @brief Thin wrappers over the process/thread syscalls the Tier-1 items need.
 *
 * Split from the items so the items read as rules and this file reads as the platform. Every
 * function is fail-safe: a failed syscall returns a sentinel and the caller reports "skipped",
 * never crashes.
 */
namespace pl::runtime {

/** The Linux thread name of a task, or empty when it cannot be read. */
std::string ThreadName(pid_t tid);

/**
 * @brief Finds a task id whose name contains {@code needle} (case-insensitive).
 *
 * @return the tid, or -1 when no matching task exists yet. A not-yet-created render thread is
 *         the normal "deferred" case, not an error.
 */
pid_t FindThreadByName(const std::string &needle);

/**
 * @brief Sets a thread's scheduling policy/priority.
 * @return true on success; false (with errno left set) on any failure.
 */
bool SetThreadScheduling(pid_t tid, int policy, int priority);

/** @brief Reads a thread's current scheduling policy and priority. */
bool GetThreadScheduling(pid_t tid, int &policy, int &priority);

/**
 * @brief Reads the per-core maximum frequency table (kHz) from sysfs.
 *
 * @param out receives up to {@code maxCores} values, indexed by core.
 * @param maxCores capacity of {@code out}.
 * @return the number of cores read, or 0 when the table is unavailable.
 */
int ReadCpuMaxFrequencies(unsigned long long *out, int maxCores);

/**
 * @brief Reads the core indices whose max frequency is at or above the performance threshold.
 *
 * @param out receives the core indices.
 * @param maxCores capacity of {@code out}.
 * @return the number of indices written.
 */
int ReadPerformanceCoreIndices(int *out, int maxCores);

/** @brief Pins a thread to a CPU mask. @return true on success. */
bool SetThreadAffinity(pid_t tid, unsigned long long mask);

/** @brief True when the kernel reports the calling process may set real-time scheduling. */
bool CanUseRealtimeScheduling();

} // namespace pl::runtime
