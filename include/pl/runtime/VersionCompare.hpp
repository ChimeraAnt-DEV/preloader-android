#pragma once

/**
 * @file VersionCompare.hpp
 * @brief Pure Minecraft version comparison, host-testable.
 *
 * The preloader's hook rules are gated by version ranges, and the launcher derives the version code
 * from the APK as `26.52` while the bundled rules are written as `1.26.50.00`. Comparing those
 * literally is wrong (`26 > 1`, so a rule that should match is skipped), so the comparison drops a
 * leading `1.` before comparing component-wise. That rule is subtle enough to be worth a unit test
 * with no device, which is why it lives here rather than inline in the rules parser.
 */

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <vector>

namespace pl::runtime {

/** @brief Splits a version string into numeric components, ignoring separators. */
inline std::vector<int> ParseVersionParts(std::string_view value) {
  std::vector<int> parts;
  long current = 0;
  bool inNumber = false;
  auto pushPart = [&] {
    if (!inNumber) return;
    parts.push_back(static_cast<int>(
        std::min<long>(current, std::numeric_limits<int>::max())));
    current = 0;
    inNumber = false;
  };
  for (unsigned char ch : value) {
    if (std::isdigit(ch)) {
      current = std::min<long>(current * 10 + static_cast<long>(ch - '0'),
                               std::numeric_limits<int>::max());
      inNumber = true;
    } else {
      pushPart();
    }
  }
  pushPart();
  return parts;
}

/**
 * @brief Version parts with a leading `1.` dropped, so `26.52` and `1.26.50.00` compare in the
 *        same space (both become `26.52` / `26.50.00`).
 */
inline std::vector<int> NormalizeVersionParts(std::string_view value) {
  auto parts = ParseVersionParts(value);
  if (!parts.empty() && parts.front() == 1) {
    parts.erase(parts.begin());
  }
  return parts;
}

/** @brief Compare two versions component-wise with missing components as zero. */
inline int CompareVersions(std::string_view left, std::string_view right) {
  const auto leftParts = NormalizeVersionParts(left);
  const auto rightParts = NormalizeVersionParts(right);
  const std::size_t count = std::max(leftParts.size(), rightParts.size());
  for (std::size_t i = 0; i < count; ++i) {
    const int leftPart = i < leftParts.size() ? leftParts[i] : 0;
    const int rightPart = i < rightParts.size() ? rightParts[i] : 0;
    if (leftPart < rightPart) return -1;
    if (leftPart > rightPart) return 1;
  }
  return 0;
}

} // namespace pl::runtime
