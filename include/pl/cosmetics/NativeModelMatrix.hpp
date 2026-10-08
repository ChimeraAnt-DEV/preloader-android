#pragma once

/**
 * @file NativeModelMatrix.hpp
 * @brief Column-major 4x4 matrix and the cosmetic anchoring composition (Task 2).
 *
 * The render seam anchors a cosmetic to a player bone with
 *
 * @f[ M_{final} = M_{world} \times M_{player\_bone} \times M_{cosmetic\_offset} @f]
 *
 * This type implements that composition and the primitive transforms it needs. It is pure (no
 * Android/game types), so the anchoring arithmetic is host-unit-testable — the same reason the
 * geometry parser is: a wrong multiplication order puts a hat behind the head and is worth catching
 * without a device.
 *
 * Storage is column-major (`m[col * 4 + row]`), matching OpenGL/OpenGL ES and glm, so the values can
 * be uploaded to a shader uniform without transposing.
 */

#include <cstddef>

#include "pl/Export.hpp"

namespace pl::cosmetics {

/** @brief A 3-component vector. */
struct Vec3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

/** @brief A 4x4 matrix in column-major order. */
struct PL_EXPORT Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  /** @brief The identity matrix. */
  [[nodiscard]] static Mat4 identity();

  /** @brief A translation matrix. */
  [[nodiscard]] static Mat4 translation(const Vec3 &t);

  /** @brief A rotation of `degrees` about the X (pitch) axis. */
  [[nodiscard]] static Mat4 rotationX(float degrees);

  /** @brief A rotation of `degrees` about the Y (yaw) axis. */
  [[nodiscard]] static Mat4 rotationY(float degrees);

  /** @brief A rotation of `degrees` about the Z (roll) axis. */
  [[nodiscard]] static Mat4 rotationZ(float degrees);

  /**
   * @brief A rotation about @p pivot by `[pitch, yaw, roll]`, applied Z then Y then X.
   *
   * This is Blockbench's bone rotation order, matching the geometry builder, so a bone's authored
   * rotation and the matrix that anchors a cosmetic to it agree.
   */
  [[nodiscard]] static Mat4 rotationAboutPivot(const Vec3 &pivot, const Vec3 &degrees);

  /** @brief The matrix product `a * b` (a is applied after b to a column vector). */
  [[nodiscard]] static Mat4 multiply(const Mat4 &a, const Mat4 &b);

  /** @brief Transforms a point (w = 1). */
  [[nodiscard]] Vec3 transformPoint(const Vec3 &p) const;

  /** @brief The element at `(row, col)`, using the column-major layout. */
  [[nodiscard]] float at(int row, int col) const { return m[col * 4 + row]; }

  /** @brief True when every element is finite. */
  [[nodiscard]] bool isFinite() const;
};

/**
 * @brief The cosmetic anchoring composition: `world * bone * offset`.
 *
 * Named so the formula is one call site and cannot drift to a different order.
 */
[[nodiscard]] Mat4 composeCosmeticMatrix(const Mat4 &world, const Mat4 &playerBone,
                                         const Mat4 &cosmeticOffset);

} // namespace pl::cosmetics
