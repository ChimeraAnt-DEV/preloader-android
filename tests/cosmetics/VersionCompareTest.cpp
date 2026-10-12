/**
 * @file VersionCompareTest.cpp
 * @brief Host test for the Minecraft version comparison the hook rules are gated by.
 *
 * The launcher derives the version code from the APK as `26.52` while the bundled rules are written
 * as `1.26.50.00`; comparing them literally is wrong (`26 > 1`). The comparison drops a leading
 * `1.` first. A regression here silently disables or wrongly enables the native cosmetics hook, so
 * the rule is pinned directly.
 */

#include <cassert>
#include <cstdio>
#include <string>

#include "pl/runtime/VersionCompare.hpp"

using pl::runtime::CompareVersions;

namespace {

int checks = 0;

template <typename A, typename B>
void expect(bool condition, A value, B expected, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s (got %s, expected %s)\n", what, std::to_string(value).c_str(),
                 std::to_string(expected).c_str());
    std::abort();
  }
}

} // namespace

int main() {
  // A leading 1. is dropped, so the same build compares equal.
  expect(CompareVersions("26.52", "1.26.50.00") > 0, 1, 1, "26.52 is newer than 1.26.50.00");
  expect(CompareVersions("26.50", "1.26.50.00") == 0, 0, 0, "26.50 == 1.26.50.00");
  expect(CompareVersions("1.26.60.00", "26.60.0") == 0, 0, 0, "1.26.60.00 == 26.60.0");

  // Component-wise, missing components count as zero.
  expect(CompareVersions("1.21.132", "1.21.132.0") == 0, 0, 0, "1.21.132 == 1.21.132.0");
  expect(CompareVersions("26.52", "26.60.21") < 0, -1, -1, "26.52 < 26.60.21");
  expect(CompareVersions("26.60.30", "26.60.28") > 0, 1, 1, "26.60.30 > 26.60.28");

  // The supported floor holds.
  expect(CompareVersions("1.21.132", "1.21.132") == 0, 0, 0, "floor matches itself");
  expect(CompareVersions("1.21.100", "1.21.132") < 0, -1, -1, "1.21.100 is below the floor");

  std::printf("VersionCompareTest: %d checks passed\n", checks);
  return 0;
}
