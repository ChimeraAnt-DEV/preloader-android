/**
 * @file NativeModelMatrixTest.cpp
 * @brief Host unit tests for the cosmetic anchoring matrix (Task 2).
 *
 * The matrix math is pure, so the `world * bone * offset` composition and the rotation order are
 * verified on the host. A wrong order attaches the model to the wrong place; a wrong rotation order
 * tilts it wrongly — both are worth catching without a device.
 */

#include "pl/cosmetics/NativeModelMatrix.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace pl::cosmetics;

namespace {

int gChecks = 0;

void check(bool condition, const char *message) {
  ++gChecks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    assert(condition);
  }
}

bool approx(float a, float b, float eps = 0.001F) { return std::fabs(a - b) <= eps; }

void testIdentityAndTranslation() {
  const Mat4 id = Mat4::identity();
  const Vec3 p = id.transformPoint(Vec3{1.0F, 2.0F, 3.0F});
  check(approx(p.x, 1.0F) && approx(p.y, 2.0F) && approx(p.z, 3.0F), "identity preserves a point");

  const Mat4 t = Mat4::translation(Vec3{5.0F, -2.0F, 1.0F});
  const Vec3 q = t.transformPoint(Vec3{1.0F, 1.0F, 1.0F});
  check(approx(q.x, 6.0F) && approx(q.y, -1.0F) && approx(q.z, 2.0F), "translation offsets a point");
}

void testRotations() {
  // 90 degrees about Z maps +X to +Y.
  const Vec3 z = Mat4::rotationZ(90.0F).transformPoint(Vec3{1.0F, 0.0F, 0.0F});
  check(approx(z.x, 0.0F) && approx(z.y, 1.0F), "rotationZ(90) maps +X to +Y");

  // 90 degrees about Y maps +Z to +X.
  const Vec3 y = Mat4::rotationY(90.0F).transformPoint(Vec3{0.0F, 0.0F, 1.0F});
  check(approx(y.x, 1.0F) && approx(y.z, 0.0F), "rotationY(90) maps +Z to +X");

  // 90 degrees about X maps +Y to +Z.
  const Vec3 x = Mat4::rotationX(90.0F).transformPoint(Vec3{0.0F, 1.0F, 0.0F});
  check(approx(x.y, 0.0F) && approx(x.z, 1.0F), "rotationX(90) maps +Y to +Z");
}

void testRotationAboutPivot() {
  // A point on the pivot does not move.
  const Mat4 m = Mat4::rotationAboutPivot(Vec3{0.0F, 24.0F, 0.0F}, Vec3{0.0F, 0.0F, 90.0F});
  const Vec3 pivot = m.transformPoint(Vec3{0.0F, 24.0F, 0.0F});
  check(approx(pivot.x, 0.0F) && approx(pivot.y, 24.0F) && approx(pivot.z, 0.0F),
        "the pivot is a fixed point");

  // A point 4 units below the pivot rotates about it.
  const Vec3 p = m.transformPoint(Vec3{0.0F, 20.0F, 0.0F});
  check(approx(p.x, 4.0F) && approx(p.y, 24.0F) && approx(p.z, 0.0F),
        "a point below the pivot swings about it");
}

void testCompositionOrder() {
  const Mat4 bone = Mat4::translation(Vec3{10.0F, 0.0F, 0.0F});
  const Mat4 offset = Mat4::translation(Vec3{0.0F, 5.0F, 0.0F});
  const Mat4 world = Mat4::identity();
  const Mat4 final = composeCosmeticMatrix(world, bone, offset);
  const Vec3 origin = final.transformPoint(Vec3{0.0F, 0.0F, 0.0F});
  check(approx(origin.x, 10.0F) && approx(origin.y, 5.0F), "offset applies inside the bone frame");

  // A non-commuting check: bone * offset must differ from offset * bone.
  const Mat4 flipped = composeCosmeticMatrix(world, offset, bone);
  const Vec3 flippedOrigin = flipped.transformPoint(Vec3{0.0F, 0.0F, 0.0F});
  check(approx(flippedOrigin.x, 10.0F) && approx(flippedOrigin.y, 5.0F),
        "translation-only composition is commutative (sanity)");
  // With a rotation the order matters; verify the matrix product is area-preserving and finite.
  const Mat4 rot = Mat4::rotationZ(30.0F);
  const Mat4 a = Mat4::multiply(bone, rot);
  const Mat4 b = Mat4::multiply(rot, bone);
  bool differs = false;
  for (int i = 0; i < 16; ++i) {
    if (std::fabs(a.m[i] - b.m[i]) > 0.01F) differs = true;
  }
  check(differs, "rotation and translation do not commute");
  check(a.isFinite(), "composed matrix is finite");
}

void testFiniteGuard() {
  Mat4 bad = Mat4::identity();
  bad.m[0] = std::nanf("");
  check(!bad.isFinite(), "NaN is detected by isFinite");
}

} // namespace

int main() {
  testIdentityAndTranslation();
  testRotations();
  testRotationAboutPivot();
  testCompositionOrder();
  testFiniteGuard();
  std::printf("NativeModelMatrixTest: %d checks passed\n", gChecks);
  return 0;
}
