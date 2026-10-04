#include "pl/runtime/optifine/OptifineDeviceRules.h"

#include <cstdint>

namespace pl::runtime {

int CountPerformanceCores(const std::uint64_t *maxFreqKhz, int count) {
  if (maxFreqKhz == nullptr || count <= 0) return 0;

  std::uint64_t fastest = 0;
  for (int i = 0; i < count; ++i) {
    if (maxFreqKhz[i] > fastest) fastest = maxFreqKhz[i];
  }
  // No core reported a frequency: the caller cannot tell fast from slow, so it must not pin.
  if (fastest == 0) return 0;

  int performance = 0;
  for (int i = 0; i < count; ++i) {
    if (maxFreqKhz[i] == 0) continue;
    // Compare as integers to avoid floating-point edge cases: freq*10 >= fastest*9 is the
    // same as freq/fastest >= 0.9, with no rounding surprises at exactly 0.9.
    if (maxFreqKhz[i] * 10ULL >= fastest * 9ULL) ++performance;
  }
  return performance;
}

bool ShouldPinToPerformanceCores(int performanceCoreCount) {
  return performanceCoreCount >= kOptifineMinPerformanceCores;
}

std::uint64_t BuildCoreMask(const int *coreIndices, int count) {
  if (coreIndices == nullptr || count <= 0) return 0;
  std::uint64_t mask = 0;
  for (int i = 0; i < count; ++i) {
    const int index = coreIndices[i];
    if (index >= 0 && index < kOptifineMaxCores) {
      mask |= (1ULL << index);
    }
  }
  return mask;
}

int SelectTargetRefreshRate(const int *supportedHz, int count, int currentHz) {
  if (supportedHz == nullptr || count <= 0) return 0;

  int highest = 0;
  for (int i = 0; i < count; ++i) {
    if (supportedHz[i] > highest) highest = supportedHz[i];
  }
  // Nothing to unlock: the panel's best is not above what the game already uses.
  if (highest <= currentHz) return 0;
  return highest;
}

} // namespace pl::runtime
