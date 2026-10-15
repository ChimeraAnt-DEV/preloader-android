/**
 * @file GameHookRuleMergeTest.cpp
 * @brief Host test for the per-field rule merge used by the hook install.
 *
 * The shipped rules can match a running version more than once (a narrow range and a wider one),
 * and first-match selection dropped every field the first match omitted — which is exactly how
 * 1.26.33.1 lost its `playerRenderVtableIndex` and installed the render hook without a slot. The
 * merge inherits a missing field from another matching rule; that rule is pinned here so a
 * regression cannot silently re-disable the hook on 1.26.33.x.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/runtime/GameHookRuleMerge.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;
using pl::runtime::MergeHookRuleForVersion;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

json rule(const char *min, const char *max, int renderSlot, bool withMenuIndex) {
  json r = json::object();
  if (min) r["min"] = std::string(min);
  if (max) r["max"] = std::string(max);
  if (withMenuIndex) r["isShowingMenuVtableIndex"] = 151;
  if (renderSlot >= 0) r["playerRenderVtableIndex"] = renderSlot;
  json sigs = json::object();
  sigs["pauseMenuDtorSig"] = "AABB";
  sigs["pauseMenuOpenSig"] = "CCDD";
  sigs["hudScreenDtorSig"] = "EEFF";
  sigs["hudScreenOpenSig"] = "1122";
  sigs["isShowingMenuSig"] = "3344";
  r["sigs"] = sigs;
  return r;
}

json shippedShape() {
  json root = json::object();
  root["revision"] = 5;
  json rules = json::array();
  rules.push_back(rule("1.26.50.00", nullptr, 17, true));
  rules.push_back(rule("1.26.40.00", nullptr, 17, true));
  rules.push_back(rule("1.26.30.00", nullptr, -1, false));       // no slot, no menu index
  rules.push_back(rule("1.21.132", "1.26.39.99", 17, false));    // carries the slot
  root["rules"] = rules;
  return root;
}

} // namespace

int main() {
  const json root = shippedShape();

  // 1.26.33.1 matches both the unbounded 1.26.30 rule and the narrow 1.21.132..1.26.39.99 rule.
  // The first omits playerRenderVtableIndex; the merge must inherit it from the second.
  {
    auto merged = MergeHookRuleForVersion(root, "1.26.33.1");
    expect(merged.has_value(), "1.26.33.1 matches a rule");
    expect(merged->contains("playerRenderVtableIndex"), "slot inherited from the narrower rule");
    expect((*merged)["playerRenderVtableIndex"].get<int>() == 17, "slot value inherited");
    // In the shipped file no rule covering 1.26.33.1 carries isShowingMenuVtableIndex; the merged
    // rule must not invent one (the sigs' isShowingMenuSig is what satisfies the parser instead).
    expect(!merged->contains("isShowingMenuVtableIndex"),
           "no menu index is invented when no matching rule declares one");
    auto sigs = (*merged)["sigs"];
    expect(sigs["pauseMenuDtorSig"].get<std::string>() == "AABB", "signatures from the first matching rule");
    expect(sigs["isShowingMenuSig"].get<std::string>() == "3344", "menu signature present");
  }

  // The 26.52 APK-derived form must compare in the same space as 1.26.50.00.
  {
    auto merged = MergeHookRuleForVersion(root, "26.52");
    expect(merged.has_value(), "26.52 matches the 1.26.50.00 rule");
    expect((*merged)["playerRenderVtableIndex"].get<int>() == 17, "26.52 slot");
  }

  // A version below the floor matches nothing.
  expect(!MergeHookRuleForVersion(root, "1.20.0").has_value(), "1.20.0 matches no rule");

  // A file whose narrow rule lost its slot still merges without one, which is what the Java
  // coverage guard refuses to install in the first place.
  {
    json bad = json::object();
    bad["revision"] = 5;
    json rules = json::array();
    rules.push_back(rule("1.26.30.00", nullptr, -1, false));
    rules.push_back(rule("1.21.132", "1.26.39.99", -1, false));
    bad["rules"] = rules;
    auto merged = MergeHookRuleForVersion(bad, "1.26.33.1");
    expect(merged.has_value(), "matches");
    expect(!merged->contains("playerRenderVtableIndex"), "no slot when no matching rule carries one");
  }

  std::printf("GameHookRuleMergeTest: %d checks passed\n", checks);
  return 0;
}
