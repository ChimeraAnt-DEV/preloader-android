#pragma once

/**
 * @file AnimationSolver.hpp
 * @brief Bedrock animation keyframe evaluation and the procedural cape motion (Task 3).
 *
 * Two animation sources drive a cosmetic:
 *
 *  - **Authored keyframes** from a Bedrock animation JSON (`animations` → `bones` → `rotation` /
 *    `position` / `scale`). {@link AnimationSolver} parses them and samples a value at a time,
 *    honouring the per-key interpolation mode (step, linear, Catmull-Rom). Catmull-Rom is the
 *    "smooth" curve Blockbench emits; treating it as linear is the difference between cloth that
 *    eases and cloth that corners.
 *  - **Procedural cloth** for a cape/tail, from the player's movement state. {@link CapeMotion}
 *    mirrors the launcher's Java `CapeAnimationCurve` constants and formulas exactly, so the
 *    preview and the native render agree about how far the cloth leans.
 *
 * Pure C++ (no Android types), so the interpolation and the curve are host-unit-testable.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "pl/Export.hpp"

namespace pl::cosmetics::animation {

/** @brief A keyframe interpolation mode. */
enum class Interpolation {
  Step,       ///< hold the value until the next key
  Linear,     ///< straight line between keys
  CatmullRom, ///< smoothed through neighbouring keys
};

/** @brief One animated channel. */
enum class Channel {
  Rotation,
  Position,
  Scale,
};

/** @brief A keyframe: a time and a 3-component value. */
struct Keyframe {
  float time = 0.0F;
  float value[3] = {0.0F, 0.0F, 0.0F};
  Interpolation interpolation = Interpolation::Linear;
};

/** @brief A per-bone channel: an ordered list of keyframes. */
struct Track {
  std::string bone;
  Channel channel = Channel::Rotation;
  std::vector<Keyframe> keys;
};

/** @brief A parsed animation. */
struct Animation {
  std::string name;
  float lengthSeconds = 0.0F;
  bool loop = false;
  std::vector<Track> tracks;
};

/** @brief Parses Bedrock animation JSON and samples keyframes. */
class AnimationSolver {
public:
  /**
   * @brief Parses an `animation`/`animations` document.
   * @return true when at least one track was parsed; a malformed document returns false and leaves
   *         `out` untouched (fail-closed, matching the geometry parser)
   */
  static bool parse(const std::string &json, Animation &out, std::string *error = nullptr);

  /**
   * @brief Samples a track at @p timeSeconds.
   *
   * A time before the first key or after the last returns the nearest key's value. The segment's
   * interpolation mode is the *starting* key's mode, which is how Bedrock resolves a mixed list.
   */
  static void sample(const Track &track, float timeSeconds, float out[3]);

  /** @brief Interpolates a single segment; exposed for direct testing. */
  static float interpolate(const Keyframe &before, const Keyframe &after, const Keyframe *beforeBefore,
                           const Keyframe *afterAfter, float timeSeconds, int component);
};

/**
 * @brief The procedural cape/tail motion.
 *
 * The constants and formulas mirror the launcher's Java `CapeAnimationCurve`, so the preview and the
 * native render produce the same lean for the same movement state. The chain shares sum to 1 so the
 * total bend equals a single bone's lean.
 */
class PL_EXPORT CapeMotion {
public:
  static constexpr double kWalkLeanDeg = 28.0;
  static constexpr double kFlapLeanDeg = 42.0;
  static constexpr double kJumpFlareDeg = 12.0;
  static constexpr double kVerticalLeanDeg = 10.0;
  static constexpr double kFlutterAmplitudeDeg = 13.0;
  static constexpr double kFlutterFrequency = 60.0;
  static constexpr double kSwayAmplitudeDeg = 10.0;
  static constexpr double kSwayFrequency = 44.0;
  static constexpr double kMaxMoveSpeed = 1.0;
  static constexpr double kMaxVertical = 1.5;
  static constexpr double kSegmentPhaseLagBlocks = 0.022;
  static constexpr double kTurnSwayDeg = 4.0;
  static constexpr int kDefaultSegments = 16;

  /** @brief The fraction of the total rotation a segment carries (1-based); shares sum to 1. */
  static double segmentShare(int index, int total);

  /** @brief A segment's X rotation in degrees; negative leans back. */
  static double segmentLeanDegrees(int index, int total, double moveSpeed, bool jumping,
                                   double verticalSpeed, double distanceMoved, double capeFlap = 0.0);

  /** @brief A segment's Z rotation in degrees. */
  static double segmentSwayDegrees(int index, int total, double moveSpeed, double distanceMoved,
                                   double bodyYawDegrees);

  /** @brief The cape bone's X rotation in degrees. */
  static double leanDegrees(double moveSpeed, bool jumping, double verticalSpeed,
                            double distanceMoved, double capeFlap = 0.0);

  /** @brief The cape bone's Z rotation in degrees. */
  static double swayDegrees(double moveSpeed, double distanceMoved);
};

/**
 * @brief A Verlet cloth solver for a cape/tail (Task 3).
 *
 * The game's Molang cape cannot express real folds (a pack has no physics driver — only queries), so
 * the native path solves the cloth instead. Each particle keeps its previous position; a step
 * integrates damping, a pull toward the anchor-space rest pose, gravity and wind, then a single
 * relaxation pass over the structural links. The constants and the rest layout mirror the launcher's
 * Java `CapeSimulator` (9x11, gravity 14, damping 0.22, stiffness 52, max stretch 1.35, breeze
 * 12/1.4), so the preview's richer cloth and the native cloth agree about the shape.
 *
 * Pure C++ — position buffers and an explicit step, so the settle behaviour is host-unit-testable.
 */
class PL_EXPORT ClothSolver {
public:
  static constexpr int kCols = 9;
  static constexpr int kRows = 11;
  static constexpr int kParticleCount = kCols * kRows;
  static constexpr float kWidthBlocks = 10.0F / 16.0F;
  static constexpr float kHeightBlocks = 1.0F;
  static constexpr float kFixedDt = 1.0F / 60.0F;
  static constexpr int kMaxSubsteps = 4;
  static constexpr float kGravity = 14.0F;
  static constexpr float kDamping = 0.22F;
  static constexpr float kStiffness = 52.0F;
  static constexpr float kMaxStretch = 1.35F;
  static constexpr float kBreezeStrength = 12.0F;
  static constexpr float kBreezeFrequency = 1.4F;

  /** @brief Resets the cloth to its rest shape beneath the anchor. */
  void reset(float anchorX, float anchorY, float anchorZ);

  /**
   * @brief Advances the cloth.
   * @param dtSeconds   real elapsed time; consumed in fixed slices
   * @param anchor      the shoulder point this frame, already moved by the body
   * @param forwardX/Z  the body's facing direction
   * @param speedBlocks the body's speed, used to billow the cape behind it
   */
  void step(float dtSeconds, float anchorX, float anchorY, float anchorZ, float forwardX,
            float forwardZ, float speedBlocks);

  /** @brief True once {@link reset} or a first {@link step} has initialised the cloth. */
  [[nodiscard]] bool initialised() const { return mInitialised; }

  /** @brief The position of particle @p index (row-major). */
  void position(int index, float &x, float &y, float &z) const;

private:
  void integrate(float dt, float anchorX, float anchorY, float anchorZ, float forwardX,
                 float forwardZ, float speedBlocks);
  void link(int a, int b, float maxStretch);
  static bool isAnchored(int index) { return index < kCols; }
  static float restLen(int a, int b);

  float mPx[kParticleCount]{};
  float mPy[kParticleCount]{};
  float mPz[kParticleCount]{};
  float mPrevX[kParticleCount]{};
  float mPrevY[kParticleCount]{};
  float mPrevZ[kParticleCount]{};
  float mRestX[kParticleCount]{};
  float mRestY[kParticleCount]{};
  float mAccumulator = 0.0F;
  float mClock = 0.0F;
  bool mInitialised = false;
};

} // namespace pl::cosmetics::animation
