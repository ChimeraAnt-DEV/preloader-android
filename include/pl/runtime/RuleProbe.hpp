#pragma once

/**
 * @file RuleProbe.hpp
 * @brief Probe-backed rule gating: a rule may only turn a feature on when its probe passed.
 *
 * <p>A generated rule (see `tools/gen_rules.py`) carries a `probes` block naming, per seam,
 * whether the seam resolved to executable code in the build the rule was generated from, and what a
 * success proves. A rule that pins a slot index without a passing probe is exactly the failure the
 * earlier rounds hit: the slot resolved on the machine it was derived on and detoured a data pointer
 * (or nothing) on another. This module is the pure decision that a feature is enabled only when
 * <em>both</em> the rule declares the slot and the rule's own probe for that seam passed.
 *
 * <p>Kept Android-free and JSON-only so it is host-tested with the real rule shape and no device,
 * the same reason {@link GameHookRuleMerge.hpp} lives here.
 */

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace pl::runtime {

/** @brief A native feature whose enablement a rule can gate. */
enum class HookFeature {
    /** The player-render hook (`playerRenderVtableIndex`). */
    PlayerRender,
    /** The player-join packet handler (`packetReadVtableIndex`). */
    PacketRead,
    /** The skin/cape accessor (`skinCapeVtableIndex`). */
    SkinCape,
    /** The texture-binder seam (`textureBindVtableIndex`). */
    TextureBind,
    /** The particle-engine seam (`particleEngineVtableIndex`). */
    ParticleEngine,
};

/** @brief Why a feature is or is not enabled. The exact reason is what the diagnostics report shows. */
enum class FeatureGate {
    /** The rule declares the slot and, when it carries a probes block, that probe resolved. */
    Enabled,
    /** The merged rule carries no slot for this feature. */
    NoRuleSlot,
    /**
     * The rule carries a probes block that omits this seam. A generated rule omits the slot too
     * when the seam did not probe, so reaching this means a rule declares the slot but its probes
     * block does not cover it - an inconsistent rule. Fail closed.
     */
    ProbeMissing,
    /** The rule's probe for this seam explicitly reports it did not resolve. */
    ProbeUnresolved,
};

/** @brief The rule key and probe key for a feature. Both are the same string by construction. */
inline const char *RuleKeyForFeature(HookFeature feature) {
    switch (feature) {
        case HookFeature::PlayerRender: return "playerRenderVtableIndex";
        case HookFeature::PacketRead: return "packetReadVtableIndex";
        case HookFeature::SkinCape: return "skinCapeVtableIndex";
        case HookFeature::TextureBind: return "textureBindVtableIndex";
        case HookFeature::ParticleEngine: return "particleEngineVtableIndex";
    }
    return "";
}

/**
 * @brief True when the merged rule declares a slot index for @p feature.
 *
 * Accepts signed and unsigned integers: a value parsed from the rules text is unsigned, but a rule
 * built programmatically (as the host test does) stores it signed. Treating a signed value as
 * absent silently dropped the slot - the class of bug the merge exists to prevent.
 */
inline bool RuleDeclaresSlot(const nlohmann::json &mergedRule, HookFeature feature) {
    if (!mergedRule.is_object()) {
        return false;
    }
    auto it = mergedRule.find(RuleKeyForFeature(feature));
    if (it == mergedRule.end()) {
        return false;
    }
    return it->is_number_unsigned() || it->is_number_integer();
}

/**
 * @brief Reads the rule's own probe for @p feature.
 *
 * @return true when `probes[key].resolved` is boolean true; false when it is false, absent, or the
 *         probes block is missing entirely. A missing probe is deliberately not "assume it worked":
 *         a hand-edited rule with no probe must not turn a feature on.
 */
inline bool RuleProbeResolved(const nlohmann::json &mergedRule, HookFeature feature) {
    if (!mergedRule.is_object()) {
        return false;
    }
    auto probesIt = mergedRule.find("probes");
    if (probesIt == mergedRule.end() || !probesIt->is_object()) {
        return false;
    }
    auto probeIt = probesIt->find(RuleKeyForFeature(feature));
    if (probeIt == probesIt->end() || !probeIt->is_object()) {
        return false;
    }
    auto resolvedIt = probeIt->find("resolved");
    return resolvedIt != probeIt->end() && resolvedIt->is_boolean() && resolvedIt->get<bool>();
}

/**
 * @brief The pure gate: a feature is enabled when the rule declares the slot, and - when the rule
 *        carries a probes block - that probe for the seam resolved.
 *
 * <p>Rules are of two shapes. A legacy/hand-derived rule has a slot but no probes block; its slot
 * index was verified against the builds it names, so it is trusted as before (a probes block cannot
 * be demanded of rules that predate it without disabling working features). A generated rule
 * carries a probes block and pins a slot only for a seam that probed to executable code, so for
 * those the probe is authoritative: a declared slot whose probe did not resolve, or that the probes
 * block omits, leaves the feature off.
 *
 * @return the exact reason, so the caller can report which link broke (item 18) rather than a bare
 *         "unavailable".
 */
inline FeatureGate EvaluateFeatureGate(const nlohmann::json &mergedRule, HookFeature feature) {
    if (!RuleDeclaresSlot(mergedRule, feature)) {
        return FeatureGate::NoRuleSlot;
    }
    // A rule with no probes block is a legacy/hand-derived rule: its slot is the whole claim.
    if (!mergedRule.is_object() || !mergedRule.contains("probes")) {
        return FeatureGate::Enabled;
    }
    auto probesIt = mergedRule.find("probes");
    if (!probesIt->is_object() || !probesIt->contains(RuleKeyForFeature(feature))) {
        // The rule carries a probes block but does not cover this seam while still declaring its
        // slot: inconsistent, so fail closed rather than trust an unprobed index.
        return FeatureGate::ProbeMissing;
    }
    if (!RuleProbeResolved(mergedRule, feature)) {
        return FeatureGate::ProbeUnresolved;
    }
    return FeatureGate::Enabled;
}

/** @brief A stable, human-readable label for a gate, for the diagnostics report. */
inline const char *FeatureGateLabel(FeatureGate gate) {
    switch (gate) {
        case FeatureGate::Enabled: return "enabled";
        case FeatureGate::NoRuleSlot: return "no slot in the matching rule";
        case FeatureGate::ProbeMissing: return "rule carries no probe for this seam";
        case FeatureGate::ProbeUnresolved: return "rule probe reports the seam did not resolve";
    }
    return "unknown";
}

} // namespace pl::runtime
