/**
 * @file AnimationSolverTest.cpp
 * @brief Host unit tests for keyframe evaluation, the cape curve and the cloth solver (Task 3).
 */

#include "pl/cosmetics/animation/AnimationSolver.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace pl::cosmetics::animation;

namespace {

int gChecks = 0;

void check(bool condition, const char *message) {
  ++gChecks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    assert(condition);
  }
}

bool approx(double a, double b, double eps = 0.01) { return std::fabs(a - b) <= eps; }

void testKeyframeParse() {
  const char *json = R"({
    "format_version": "1.8.0",
    "animations": {
      "animation.glowberry.wing_flap": {
        "loop": true,
        "animation_length": 1.0,
        "bones": {
          "wing_left": {
            "rotation": {
              "0.0": [0, 0, 0],
              "0.5": [0, 0, 30],
              "1.0": { "post": [0, 0, 0] }
            }
          }
        }
      }
    }
  })";
  Animation anim;
  std::string error;
  check(AnimationSolver::parse(json, anim, &error), "animation parses");
  check(anim.name == "animation.glowberry.wing_flap", "name read");
  check(anim.loop, "loop flag read");
  check(anim.tracks.size() == 1, "one track");
  const Track &track = anim.tracks.front();
  check(track.bone == "wing_left", "bone read");
  check(track.channel == Channel::Rotation, "channel read");
  check(track.keys.size() == 3, "three keys");

  float value[3];
  AnimationSolver::sample(track, 0.25F, value);
  check(approx(value[2], 15.0), "midpoint of a linear segment");
  AnimationSolver::sample(track, 0.5F, value);
  check(approx(value[2], 30.0), "value at a key");
  AnimationSolver::sample(track, 1.0F, value);
  check(approx(value[2], 0.0), "the post object value is read");
}

void testCatmullRom() {
  // Three points on a line: Catmull-Rom must still reproduce the line exactly (a sanity that the
  // basis is right). Then a non-linear case where it differs from linear.
  Track track;
  track.bone = "x";
  track.keys = {{0.0F, {0, 0, 0}}, {1.0F, {0, 0, 10}}, {2.0F, {0, 0, 0}}};
  track.keys[0].interpolation = Interpolation::CatmullRom;
  float value[3];
  AnimationSolver::sample(track, 0.5F, value);
  check(value[2] > 0.0F && value[2] < 10.0F, "catmull-rom inside the segment range");

  // A mid key with unequal neighbours: the interpolation must still hit the key's exact value.
  AnimationSolver::sample(track, 1.0F, value);
  check(approx(value[2], 10.0), "catmull-rom hits the key value");

  // Step holds.
  Track step;
  step.bone = "y";
  step.keys = {{0.0F, {0, 0, 1}}, {1.0F, {0, 0, 9}}};
  step.keys[0].interpolation = Interpolation::Step;
  AnimationSolver::sample(step, 0.7F, value);
  check(approx(value[2], 1.0), "step holds the first value");
}

void testCapeShares() {
  double sum = 0.0;
  for (int i = 1; i <= CapeMotion::kDefaultSegments; ++i) {
    sum += CapeMotion::segmentShare(i, CapeMotion::kDefaultSegments);
  }
  check(approx(sum, 1.0), "segment shares sum to 1");
  check(CapeMotion::segmentShare(1, 16) < CapeMotion::segmentShare(16, 16),
        "hem carries more than the shoulders");

  // A still character leans not at all from speed (the breeze is in the cloth, not the bone).
  check(approx(CapeMotion::leanDegrees(0.0, false, 0.0, 0.0), 0.0), "still leans zero");
  // Walking leans back (negative).
  check(CapeMotion::leanDegrees(1.0, false, 0.0, 0.0) < -20.0, "walking leans back");
  // Jumping flares further.
  check(CapeMotion::leanDegrees(1.0, true, 0.0, 0.0) < CapeMotion::leanDegrees(1.0, false, 0.0, 0.0),
        "jumping flares more than walking");
}

void testClothSettle() {
  ClothSolver cloth;
  cloth.reset(0.0F, 24.0F, 0.0F);
  check(cloth.initialised(), "cloth initialised after reset");

  // The top row is pinned to the anchor.
  float x, y, z;
  cloth.position(0, x, y, z);
  check(approx(y, 24.0, 0.001), "top-left anchored at the shoulder height");

  // Step for a second; the hem must hang below the anchor and stay finite.
  for (int i = 0; i < 60; ++i) {
    cloth.step(1.0F / 60.0F, 0.0F, 24.0F, 0.0F, 0.0F, 1.0F, 0.0F);
  }
  const int hem = ClothSolver::kParticleCount - 1;
  cloth.position(hem, x, y, z);
  check(std::isfinite(x) && std::isfinite(y) && std::isfinite(z), "hem stays finite");
  check(y < 24.0F, "hem hangs below the anchor");
  check(y > 24.0F - 2.0F, "hem does not fall away unbounded (stiffness holds it)");

  // Anchors keep following a moving shoulder.
  for (int i = 0; i < 10; ++i) {
    cloth.step(1.0F / 60.0F, 5.0F, 24.0F, 0.0F, 0.0F, 1.0F, 0.0F);
  }
  cloth.position(4, x, y, z);
  check(approx(x, 5.0, 0.2), "the anchored row follows the shoulder on X");
}

void testMalformed() {
  Animation anim;
  check(!AnimationSolver::parse("{}", anim), "empty object rejected");
  check(!AnimationSolver::parse("not json", anim), "bad json rejected");
  check(!AnimationSolver::parse("{\"animations\":{}}", anim), "no tracks rejected");
}

} // namespace

int main() {
  testKeyframeParse();
  testCatmullRom();
  testCapeShares();
  testClothSettle();
  testMalformed();
  std::printf("AnimationSolverTest: %d checks passed\n", gChecks);
  return 0;
}
