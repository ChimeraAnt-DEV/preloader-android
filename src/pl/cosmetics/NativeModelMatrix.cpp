/**
 * @file NativeModelMatrix.cpp
 * @brief 4x4 matrix math for cosmetic anchoring (Task 2).
 */

#include "pl/cosmetics/NativeModelMatrix.hpp"

#include <cmath>
#include <numbers>

namespace pl::cosmetics {
namespace {
constexpr double kDegToRad = std::numbers::pi / 180.0;
} // namespace

Mat4 Mat4::identity() { return Mat4{}; }

Mat4 Mat4::translation(const Vec3 &t) {
  Mat4 result;
  result.m[12] = t.x;
  result.m[13] = t.y;
  result.m[14] = t.z;
  return result;
}

Mat4 Mat4::rotationX(float degrees) {
  const double r = degrees * kDegToRad;
  const float c = static_cast<float>(std::cos(r));
  const float s = static_cast<float>(std::sin(r));
  Mat4 result;
  result.m[5] = c;
  result.m[6] = s;
  result.m[9] = -s;
  result.m[10] = c;
  return result;
}

Mat4 Mat4::rotationY(float degrees) {
  const double r = degrees * kDegToRad;
  const float c = static_cast<float>(std::cos(r));
  const float s = static_cast<float>(std::sin(r));
  Mat4 result;
  result.m[0] = c;
  result.m[2] = -s;
  result.m[8] = s;
  result.m[10] = c;
  return result;
}

Mat4 Mat4::rotationZ(float degrees) {
  const double r = degrees * kDegToRad;
  const float c = static_cast<float>(std::cos(r));
  const float s = static_cast<float>(std::sin(r));
  Mat4 result;
  result.m[0] = c;
  result.m[1] = s;
  result.m[4] = -s;
  result.m[5] = c;
  return result;
}

Mat4 Mat4::rotationAboutPivot(const Vec3 &pivot, const Vec3 &degrees) {
  // Translate to the pivot, rotate Z then Y then X, translate back.
  const Mat4 toPivot = translation(pivot);
  const Mat4 fromPivot = translation(Vec3{-pivot.x, -pivot.y, -pivot.z});
  Mat4 rotation = multiply(rotationX(degrees.x), multiply(rotationY(degrees.y), rotationZ(degrees.z)));
  return multiply(toPivot, multiply(rotation, fromPivot));
}

Mat4 Mat4::multiply(const Mat4 &a, const Mat4 &b) {
  Mat4 result;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0F;
      for (int k = 0; k < 4; ++k) {
        sum += a.at(row, k) * b.at(k, col);
      }
      result.m[col * 4 + row] = sum;
    }
  }
  return result;
}

Vec3 Mat4::transformPoint(const Vec3 &p) const {
  return Vec3{
      m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
      m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
      m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
  };
}

bool Mat4::isFinite() const {
  for (float value : m) {
    if (!std::isfinite(value)) return false;
  }
  return true;
}

Mat4 composeCosmeticMatrix(const Mat4 &world, const Mat4 &playerBone, const Mat4 &cosmeticOffset) {
  return Mat4::multiply(world, Mat4::multiply(playerBone, cosmeticOffset));
}

} // namespace pl::cosmetics
