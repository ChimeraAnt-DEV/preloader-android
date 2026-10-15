#pragma once

/**
 * @file GameHookRuleMerge.hpp
 * @brief Pure, host-testable resolution of the per-version hook rules.
 *
 * <p>The rules file carries one entry per verified build, and a running version can match more than
 * one entry (a narrow `1.21.132..1.26.39.99` range and a wide `min: 1.26.30.00` range both cover
 * `1.26.33.1`). Selecting the <em>first</em> matching entry loses every field that entry omits — on
 * `1.26.33.1` the first match carries no `playerRenderVtableIndex`, so the render slot fell through
 * to a default and the hook installed without a slot. This module resolves each field from the
 * first matching entry that actually declares it, so a field only a broader entry carries is still
 * inherited.
 *
 * <p>Extracted from `GameHookRules.cpp` (which pulls in the Android logger and so cannot be built on
 * the host) precisely so the merge can be unit-tested with the real rule shape and no device — the
 * same reason `VersionCompare.hpp` lives here.
 */

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "pl/runtime/VersionCompare.hpp"

namespace pl::runtime {

/** @brief True when a rule's `min`/`max` range contains `version`. */
inline bool RuleMatchesVersion(const nlohmann::json &rule,
                               const std::string &minecraftVersion) {
  if (!rule.is_object()) {
    return false;
  }

  const std::string minVersion =
      rule.contains("min") && rule["min"].is_string() ? rule["min"].get<std::string>() : "";
  const std::string maxVersion =
      rule.contains("max") && rule["max"].is_string() ? rule["max"].get<std::string>() : "";

  if (minecraftVersion.empty()) {
    return minVersion.empty() && maxVersion.empty();
  }

  if (!minVersion.empty() && CompareVersions(minecraftVersion, minVersion) < 0) {
    return false;
  }
  if (!maxVersion.empty() && CompareVersions(minecraftVersion, maxVersion) > 0) {
    return false;
  }
  return true;
}

/** @brief The keys whose value is an integer vtable index / slot (resolved first-wins on merge). */
inline constexpr const char *kRuleIndexKeys[] = {
    "isShowingMenuVtableIndex", "playerRenderVtableIndex", "skinCapeVtableIndex",
    "packetReadVtableIndex",    "particleEngineVtableIndex",
};

/** @brief The signature keys carried inside a rule's `sigs` object. */
inline constexpr const char *kRuleSigKeys[] = {
    "pauseMenuDtorSig", "pauseMenuOpenSig", "hudScreenDtorSig",
    "hudScreenOpenSig", "isShowingMenuSig", "imageLoaderSig",
};

inline bool HasNonBlankString(const nlohmann::json &object, const char *key) {
  auto it = object.find(key);
  return it != object.end() && it->is_string() && !it->get<std::string>().empty();
}

/**
 * @brief True when @p it is a JSON integer index/slot.
 *
 * Accepts both unsigned and signed integers: a value parsed from the rules text is unsigned, but a
 * caller that builds a rule programmatically (as the host test does) stores it signed. Treating a
 * signed value as "absent" silently dropped the slot, which is the class of bug this merge exists
 * to prevent.
 */
inline bool IsIndexValue(const nlohmann::json &value) {
  return value.is_number_unsigned() || value.is_number_integer();
}

/**
 * @brief Merges every rule matching @p minecraftVersion into one effective rule object.
 *
 * <p>Each field is taken from the first matching rule that declares it, so file order still decides
 * which signatures win while a field no matching-but-first rule carries is inherited from a later
 * one. The result has the same shape as a single rule (`sigs` + scalar slot keys), so the existing
 * parser consumes it unchanged.
 *
 * @return the merged rule, or nullopt when no rule matches the version.
 */
inline std::optional<nlohmann::json> MergeHookRuleForVersion(const nlohmann::json &root,
                                                             const std::string &minecraftVersion) {
  if (!root.is_object()) {
    return std::nullopt;
  }
  auto rulesIt = root.find("rules");
  if (rulesIt == root.end() || !rulesIt->is_array()) {
    return std::nullopt;
  }

  nlohmann::json merged = nlohmann::json::object();
  nlohmann::json mergedSigs = nlohmann::json::object();
  bool matched = false;

  for (const auto &rule : *rulesIt) {
    if (!RuleMatchesVersion(rule, minecraftVersion)) {
      continue;
    }
    matched = true;

    const nlohmann::json *sigs = &rule;
    auto sigsIt = rule.find("sigs");
    if (sigsIt != rule.end() && sigsIt->is_object()) {
      sigs = &(*sigsIt);
    }
    for (const char *key : kRuleSigKeys) {
      if (!mergedSigs.contains(key) && HasNonBlankString(*sigs, key)) {
        mergedSigs[key] = (*sigs)[key];
      }
    }
    for (const char *key : kRuleIndexKeys) {
      auto it = rule.find(key);
      if (merged.contains(key) || it == rule.end() || !IsIndexValue(*it)) {
        continue;
      }
      merged[key] = *it;
    }
    if (!merged.contains("min") && rule.contains("min") && rule["min"].is_string()) {
      merged["min"] = rule["min"];
    }
    if (!merged.contains("max") && rule.contains("max") && rule["max"].is_string()) {
      merged["max"] = rule["max"];
    }
  }

  if (!matched) {
    return std::nullopt;
  }
  merged["sigs"] = mergedSigs;
  return merged;
}

} // namespace pl::runtime
