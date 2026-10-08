/**
 * @file AnimationSolver.cpp
 * @brief Bedrock animation parsing/sampling and the procedural cape motion (Task 3).
 */

#include "pl/cosmetics/animation/AnimationSolver.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pl::cosmetics::animation {
namespace {

using nlohmann::json;

float clampf(double value, double lo, double hi) {
  if (value < lo) return static_cast<float>(lo);
  if (value > hi) return static_cast<float>(hi);
  return static_cast<float>(value);
}

void readValueArray(const json &element, float out[3]) {
  out[0] = out[1] = out[2] = 0.0F;
  if (element.is_array()) {
    for (int i = 0; i < 3 && i < static_cast<int>(element.size()); ++i) {
      if (element.at(i).is_number()) out[i] = element.at(i).get<float>();
    }
  } else if (element.is_number()) {
    out[0] = out[1] = out[2] = element.get<float>();
  }
}

Interpolation parseMode(const json &keyObject) {
  if (keyObject.is_object()) {
    if (keyObject.contains("lerp_mode") && keyObject.at("lerp_mode").is_string()) {
      const std::string mode = keyObject.at("lerp_mode").get<std::string>();
      if (mode == "catmullrom") return Interpolation::CatmullRom;
      if (mode == "step") return Interpolation::Step;
      if (mode == "linear") return Interpolation::Linear;
    }
  }
  return Interpolation::Linear;
}

/** @brief Reads a key's value from either a bare array or a {pre, post} object. */
void parseKeyValue(const json &keyValue, float out[3]) {
  if (keyValue.is_object()) {
    if (keyValue.contains("post")) {
      readValueArray(keyValue.at("post"), out);
      return;
    }
    if (keyValue.contains("pre")) {
      readValueArray(keyValue.at("pre"), out);
      return;
    }
    readValueArray(json::array(), out);
    return;
  }
  readValueArray(keyValue, out);
}

Channel channelFromKey(const std::string &key) {
  if (key == "position") return Channel::Position;
  if (key == "scale") return Channel::Scale;
  return Channel::Rotation;
}

} // namespace

bool AnimationSolver::parse(const std::string &jsonText, Animation &out, std::string *error) {
  auto fail = [&](const char *reason) {
    if (error != nullptr) *error = reason;
    return false;
  };
  if (jsonText.empty()) return fail("empty document");
  json root = json::parse(jsonText, nullptr, false);
  if (root.is_discarded() || !root.is_object()) return fail("not a JSON object");

  const json *animations = nullptr;
  if (root.contains("animations") && root.at("animations").is_object()) {
    animations = &root.at("animations");
  } else if (root.contains("animation") && root.at("animation").is_object()) {
    animations = &root.at("animation");
  }
  if (animations == nullptr) return fail("no animations object");

  Animation animation;
  for (auto it = animations->begin(); it != animations->end(); ++it) {
    if (!it.value().is_object()) continue;
    const json &anim = it.value();
    animation.name = it.key();
    animation.loop = anim.value("loop", false);
    animation.lengthSeconds = anim.value("animation_length", 0.0F);

    if (!anim.contains("bones") || !anim.at("bones").is_object()) continue;
    for (auto boneIt = anim.at("bones").begin(); boneIt != anim.at("bones").end(); ++boneIt) {
      if (!boneIt.value().is_object()) continue;
      for (auto channelIt = boneIt.value().begin(); channelIt != boneIt.value().end(); ++channelIt) {
        const std::string channelKey = channelIt.key();
        if (channelKey != "rotation" && channelKey != "position" && channelKey != "scale") continue;
        if (!channelIt.value().is_object()) continue;

        Track track;
        track.bone = boneIt.key();
        track.channel = channelFromKey(channelKey);
        for (auto keyIt = channelIt.value().begin(); keyIt != channelIt.value().end(); ++keyIt) {
          Keyframe keyframe;
          try {
            keyframe.time = std::stof(keyIt.key());
          } catch (...) {
            continue;
          }
          keyframe.interpolation = parseMode(keyIt.value());
          parseKeyValue(keyIt.value(), keyframe.value);
          track.keys.push_back(keyframe);
        }
        std::sort(track.keys.begin(), track.keys.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
        if (!track.keys.empty()) animation.tracks.push_back(std::move(track));
      }
    }
    // One document typically holds one animation; keep the first with tracks.
    if (!animation.tracks.empty()) break;
  }

  if (animation.tracks.empty()) return fail("no tracks");
  out = std::move(animation);
  return true;
}

float AnimationSolver::interpolate(const Keyframe &before, const Keyframe &after,
                                   const Keyframe *beforeBefore, const Keyframe *afterAfter,
                                   float timeSeconds, int component) {
  const float span = after.time - before.time;
  if (span <= 0.0F) return after.value[component];

  float t = (timeSeconds - before.time) / span;
  if (t < 0.0F) t = 0.0F;
  if (t > 1.0F) t = 1.0F;

  switch (before.interpolation) {
    case Interpolation::Step:
      return before.value[component];
    case Interpolation::Linear:
      return before.value[component] + (after.value[component] - before.value[component]) * t;
    case Interpolation::CatmullRom: {
      const float p0 = beforeBefore ? beforeBefore->value[component] : before.value[component];
      const float p1 = before.value[component];
      const float p2 = after.value[component];
      const float p3 = afterAfter ? afterAfter->value[component] : after.value[component];
      const float t2 = t * t;
      const float t3 = t2 * t;
      return 0.5F * ((2.0F * p1) + (-p0 + p2) * t +
                     (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * t2 +
                     (-p0 + 3.0F * p1 - 3.0F * p2 + p3) * t3);
    }
  }
  return before.value[component];
}

void AnimationSolver::sample(const Track &track, float timeSeconds, float out[3]) {
  out[0] = out[1] = out[2] = 0.0F;
  if (track.keys.empty()) return;
  if (timeSeconds <= track.keys.front().time) {
    for (int i = 0; i < 3; ++i) out[i] = track.keys.front().value[i];
    return;
  }
  if (timeSeconds >= track.keys.back().time) {
    for (int i = 0; i < 3; ++i) out[i] = track.keys.back().value[i];
    return;
  }
  for (std::size_t i = 0; i + 1 < track.keys.size(); ++i) {
    if (timeSeconds <= track.keys[i + 1].time) {
      const Keyframe *beforeBefore = i > 0 ? &track.keys[i - 1] : nullptr;
      const Keyframe *afterAfter = i + 2 < track.keys.size() ? &track.keys[i + 2] : nullptr;
      for (int c = 0; c < 3; ++c) {
        out[c] = interpolate(track.keys[i], track.keys[i + 1], beforeBefore, afterAfter, timeSeconds, c);
      }
      return;
    }
  }
  for (int i = 0; i < 3; ++i) out[i] = track.keys.back().value[i];
}

double CapeMotion::segmentShare(int index, int total) {
  if (total <= 1) return 1.0;
  if (index < 1) index = 1;
  if (index > total) index = total;
  return (2.0 * index) / static_cast<double>(total * (total + 1));
}

double CapeMotion::leanDegrees(double moveSpeed, bool jumping, double verticalSpeed,
                               double distanceMoved, double capeFlap) {
  const double speed = clampf(moveSpeed, 0.0, kMaxMoveSpeed);
  const double flap = clampf(capeFlap, 0.0, 1.0);
  const double vertical = clampf(verticalSpeed, -kMaxVertical, kMaxVertical);
  double lean = speed * kWalkLeanDeg + flap * kFlapLeanDeg;
  if (jumping) lean += kJumpFlareDeg;
  lean += vertical * kVerticalLeanDeg;
  lean += std::sin(distanceMoved * kFlutterFrequency) * kFlutterAmplitudeDeg * speed;
  return -lean;
}

double CapeMotion::swayDegrees(double moveSpeed, double distanceMoved) {
  const double speed = clampf(moveSpeed, 0.0, kMaxMoveSpeed);
  return std::sin(distanceMoved * kSwayFrequency) * kSwayAmplitudeDeg * speed;
}

double CapeMotion::segmentLeanDegrees(int index, int total, double moveSpeed, bool jumping,
                                      double verticalSpeed, double distanceMoved, double capeFlap) {
  const double share = segmentShare(index, total);
  const double phase = (index - 1) * kSegmentPhaseLagBlocks;
  return share * leanDegrees(moveSpeed, jumping, verticalSpeed, distanceMoved - phase, capeFlap);
}

double CapeMotion::segmentSwayDegrees(int index, int total, double moveSpeed, double distanceMoved,
                                      double bodyYawDegrees) {
  const double share = segmentShare(index, total);
  const double phase = (index - 1) * kSegmentPhaseLagBlocks;
  const double speed = clampf(moveSpeed, 0.0, kMaxMoveSpeed);
  const double turn = std::sin(bodyYawDegrees * std::numbers::pi / 180.0) * kTurnSwayDeg * speed;
  return share * (swayDegrees(moveSpeed, distanceMoved - phase) + turn);
}

// ---------------------------------------------------------------------------------------------
// ClothSolver
// ---------------------------------------------------------------------------------------------

void ClothSolver::reset(float anchorX, float anchorY, float anchorZ) {
  for (int r = 0; r < kRows; ++r) {
    const float v = r / static_cast<float>(kRows - 1);
    for (int c = 0; c < kCols; ++c) {
      const float u = c / static_cast<float>(kCols - 1) - 0.5F;
      const int i = r * kCols + c;
      mRestX[i] = u * kWidthBlocks;
      mRestY[i] = -v * kHeightBlocks;
      mPx[i] = anchorX + mRestX[i];
      mPy[i] = anchorY + mRestY[i];
      mPz[i] = anchorZ;
      mPrevX[i] = mPx[i];
      mPrevY[i] = mPy[i];
      mPrevZ[i] = mPz[i];
    }
  }
  mAccumulator = 0.0F;
  mClock = 0.0F;
  mInitialised = true;
}

void ClothSolver::step(float dtSeconds, float anchorX, float anchorY, float anchorZ, float forwardX,
                       float forwardZ, float speedBlocks) {
  if (!mInitialised) {
    reset(anchorX, anchorY, anchorZ);
    return;
  }
  // Clamp so a stall cannot bank more work than a few slices.
  const float clamped = dtSeconds < kFixedDt * kMaxSubsteps ? dtSeconds : kFixedDt * kMaxSubsteps;
  mAccumulator += clamped;
  int steps = 0;
  while (mAccumulator >= kFixedDt && steps < kMaxSubsteps) {
    integrate(kFixedDt, anchorX, anchorY, anchorZ, forwardX, forwardZ, speedBlocks);
    mAccumulator -= kFixedDt;
    ++steps;
  }
  if (steps == kMaxSubsteps) mAccumulator = 0.0F;
}

void ClothSolver::integrate(float dt, float anchorX, float anchorY, float anchorZ, float forwardX,
                            float forwardZ, float speedBlocks) {
  const float dampingPerStep = static_cast<float>(std::pow(kDamping, dt));
  mClock += dt;
  const float breezeX = std::sin(mClock * kBreezeFrequency) * kBreezeStrength;
  const float windX = -forwardX * speedBlocks * 0.11F + breezeX * 0.11F;
  const float windZ = -forwardZ * speedBlocks * 0.11F;

  for (int i = 0; i < kParticleCount; ++i) {
    if (isAnchored(i)) {
      mPx[i] = anchorX + mRestX[i];
      mPy[i] = anchorY + mRestY[i];
      mPz[i] = anchorZ;
      mPrevX[i] = mPx[i];
      mPrevY[i] = mPy[i];
      mPrevZ[i] = mPz[i];
      continue;
    }

    const float vx = (mPx[i] - mPrevX[i]) * dampingPerStep;
    const float vy = (mPy[i] - mPrevY[i]) * dampingPerStep;
    const float vz = (mPz[i] - mPrevZ[i]) * dampingPerStep;

    mPrevX[i] = mPx[i];
    mPrevY[i] = mPy[i];
    mPrevZ[i] = mPz[i];

    const float pullX = (anchorX + mRestX[i] - mPx[i]) * kStiffness * dt;
    const float pullY = (anchorY + mRestY[i] - mPy[i]) * kStiffness * dt;
    const float pullZ = (anchorZ - mPz[i]) * kStiffness * dt;

    mPx[i] += vx + pullX + windX * dt;
    mPy[i] += vy + pullY - kGravity * dt * dt;
    mPz[i] += vz + pullZ + windZ * dt;
  }

  // One relaxation pass over the structural links: enough to stop the cloth shearing apart, O(n).
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kCols; ++c) {
      const int i = r * kCols + c;
      if (c + 1 < kCols) link(i, i + 1, kMaxStretch);
      if (r + 1 < kRows) link(i, i + kCols, kMaxStretch);
    }
  }
}

float ClothSolver::restLen(int a, int b) {
  const float dx = (b % kCols - a % kCols) * (kWidthBlocks / (kCols - 1));
  const float dy = (b / kCols - a / kCols) * (kHeightBlocks / (kRows - 1));
  return std::sqrt(dx * dx + dy * dy);
}

void ClothSolver::link(int a, int b, float maxStretch) {
  const float dx = mPx[b] - mPx[a];
  const float dy = mPy[b] - mPy[a];
  const float dz = mPz[b] - mPz[a];
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float limit = restLen(a, b) * maxStretch;
  if (dist <= limit || dist < 1e-5F) return;

  const float correction = (dist - limit) / dist;
  const bool anchorA = isAnchored(a);
  const bool anchorB = isAnchored(b);
  if (anchorA && anchorB) return;

  // Split the correction between the ends, but never move an anchored particle.
  float weightA = anchorA ? 0.0F : (anchorB ? 1.0F : 0.5F);
  float weightB = anchorB ? 0.0F : (anchorA ? 1.0F : 0.5F);
  mPx[a] += dx * correction * weightA;
  mPy[a] += dy * correction * weightA;
  mPz[a] += dz * correction * weightA;
  mPx[b] -= dx * correction * weightB;
  mPy[b] -= dy * correction * weightB;
  mPz[b] -= dz * correction * weightB;
}

void ClothSolver::position(int index, float &x, float &y, float &z) const {
  if (index < 0 || index >= kParticleCount) {
    x = y = z = 0.0F;
    return;
  }
  x = mPx[index];
  y = mPy[index];
  z = mPz[index];
}

} // namespace pl::cosmetics::animation
