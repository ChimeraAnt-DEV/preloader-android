/**
 * @file RuleProbeTest.cpp
 * @brief Host test for the probe-backed rule gate (item 17).
 *
 * The gate is the rule that a feature is enabled only when the matching rule both declares the
 * slot and carries a probe that resolved it. A rule with a slot but no passing probe must leave
 * the feature off — that is what stops a pre-probe (hand-derived) rule from detouring a stale slot.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/runtime/RuleProbe.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;
using pl::runtime::EvaluateFeatureGate;
using pl::runtime::FeatureGate;
using pl::runtime::FeatureGateLabel;
using pl::runtime::HookFeature;
using pl::runtime::RuleDeclaresSlot;
using pl::runtime::RuleProbeResolved;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

json probedRule(int renderSlot, bool renderProbe) {
  json r = json::object();
  r["min"] = std::string("1.26.60.28");
  r["max"] = std::string("1.26.60.28");
  if (renderSlot >= 0) r["playerRenderVtableIndex"] = renderSlot;
  json probes = json::object();
  json p = json::object();
  p["resolved"] = renderProbe;
  if (!renderProbe) p["reason"] = std::string("no primary vtable slot resolved");
  probes["playerRenderVtableIndex"] = p;
  r["probes"] = probes;
  return r;
}

}  // namespace

int main() {
  // A slot with a resolved probe is enabled.
  expect(EvaluateFeatureGate(probedRule(17, true), HookFeature::PlayerRender) ==
             FeatureGate::Enabled,
         "slot + resolved probe enables the feature");
  expect(RuleDeclaresSlot(probedRule(17, true), HookFeature::PlayerRender),
         "the slot is read back");
  expect(RuleProbeResolved(probedRule(17, true), HookFeature::PlayerRender),
         "the probe is read back");

  // A slot whose probe reports unresolved must NOT enable the feature.
  expect(EvaluateFeatureGate(probedRule(17, false), HookFeature::PlayerRender) ==
             FeatureGate::ProbeUnresolved,
         "an unresolved probe leaves the feature off");

  // No slot at all.
  expect(EvaluateFeatureGate(probedRule(-1, true), HookFeature::PlayerRender) ==
             FeatureGate::NoRuleSlot,
         "no slot means the feature has no rule");

  // A legacy rule (a slot but no probes block) keeps working: its slot was verified against the
  // builds it names, and demanding a probes block of a rule that predates the field would disable
  // working features. Only a rule that *carries* a probes block is probe-enforced.
  json legacy = json::object();
  legacy["playerRenderVtableIndex"] = 17;
  expect(EvaluateFeatureGate(legacy, HookFeature::PlayerRender) == FeatureGate::Enabled,
         "a legacy rule with a slot and no probes block stays enabled");

  // A probes block that omits this seam is also ProbeMissing, not "assume it worked".
  json partial = json::object();
  partial["playerRenderVtableIndex"] = 17;
  json probes = json::object();
  json other = json::object();
  other["resolved"] = true;
  probes["packetReadVtableIndex"] = other;
  partial["probes"] = probes;
  expect(EvaluateFeatureGate(partial, HookFeature::PlayerRender) == FeatureGate::ProbeMissing,
         "a probes block missing this seam stays off");

  // A signed slot value counts as declared (a rule built programmatically stores it signed).
  json signedRule = probedRule(-1, true);
  signedRule["playerRenderVtableIndex"] = 17;  // signed int literal
  expect(RuleDeclaresSlot(signedRule, HookFeature::PlayerRender),
         "a signed slot value is declared");
  expect(EvaluateFeatureGate(signedRule, HookFeature::PlayerRender) == FeatureGate::Enabled,
         "a signed slot with a resolved probe is enabled");

  // Each gate has a distinct, non-empty label for the diagnostics report.
  expect(std::string(FeatureGateLabel(FeatureGate::Enabled)) !=
             std::string(FeatureGateLabel(FeatureGate::ProbeUnresolved)),
         "gate labels are distinct");
  expect(std::string(FeatureGateLabel(FeatureGate::NoRuleSlot)).size() > 0,
         "gate labels are non-empty");

  // The other features read their own keys and are independent.
  json multi = probedRule(17, true);
  multi["packetReadVtableIndex"] = 40;
  json mp = json::object();
  mp["resolved"] = true;
  multi["probes"]["packetReadVtableIndex"] = mp;
  expect(EvaluateFeatureGate(multi, HookFeature::PacketRead) == FeatureGate::Enabled,
         "a second probed feature is enabled independently");
  expect(EvaluateFeatureGate(multi, HookFeature::SkinCape) == FeatureGate::NoRuleSlot,
         "an unlisted feature is off");

  std::printf("RuleProbeTest: %d checks passed\n", checks);
  return 0;
}
