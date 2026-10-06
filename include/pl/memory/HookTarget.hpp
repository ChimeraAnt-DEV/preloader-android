#pragma once

/**
 * @file HookTarget.hpp
 * @brief Pre-install validation and diagnostics for hook targets.
 *
 * Installing a detour writes to the first instructions of the target function. A target
 * that is null, mis-aligned, or outside a readable+executable mapping faults the thread
 * that later executes it — on the game that is the render/loader thread, long after the
 * install call returned. These helpers make the target verifiable *before* anything is
 * written, and describe the region so a log line can pinpoint a bad address.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "pl/Export.hpp"

namespace pl::memory {

/** Instruction-width alignment required of an AArch64/Thumb hook target. */
inline constexpr std::size_t kInstructionAlignment = 4;

/**
 * @brief The `/proc/self/maps` region containing an address.
 */
struct MemoryRegionInfo {
  bool mapped = false;
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
  std::string path;        ///< mapping path (e.g. libminecraftpe.so), may be empty.
  std::string symbolName;  ///< nearest exported symbol via dladdr, may be empty.

  bool readable() const noexcept { return mapped && perms[0] == 'r'; }
  bool writable() const noexcept { return mapped && perms[1] == 'w'; }
  bool executable() const noexcept { return mapped && perms[2] == 'x'; }

  /** e.g. "r-xp 0x7a1c000000-0x7a1c400000 libminecraftpe.so (+sym)". */
  std::string describe() const;
};

/**
 * @brief Looks up the memory region that contains @p address.
 *
 * Returns a default (unmapped) info when the address is not mapped, so a caller can
 * distinguish "not mapped" from "mapped but not executable".
 */
PL_EXPORT MemoryRegionInfo queryMemoryRegion(uintptr_t address);

/**
 * @brief True when @p address is non-null and instruction-aligned.
 */
PL_EXPORT bool isHookAddressSane(uintptr_t address);

} // namespace pl::memory
