#pragma once

/**
 * @file NativeCosmeticData.hpp
 * @brief Pure parsing of the cosmetic frame + geometry blobs (host-testable).
 *
 * These functions turn the launcher's {@code CosmeticFrame} wire buffer and the geometry blob into
 * plain data structures. They are deliberately Android-free and GLES-free, so the same exact
 * buffers the device renderer consumes are unit-tested on the build host: a wrong field order or a
 * length-prefix bug is caught here rather than only as a garbled draw on a device.
 *
 * All reads are bounds-checked and fail closed — a truncated or invalid buffer returns false / an
 * empty result and never dereferences a partial value.
 */

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pl/Export.hpp"
#include "pl/cosmetics/NativeModelMatrix.hpp"
#include "pl/cosmetics/geometry/BedrockGeometry.hpp"

namespace pl::cosmetics {

/**
 * @brief Parses the launcher's per-frame transform buffer (the {@code CosmeticFrame} wire layout).
 *
 * @return true when the buffer is valid (magic == CHF1) and fully consumed.
 */
PL_EXPORT bool ParseCosmeticFrameData(std::span<const std::uint8_t> frame, std::uint32_t &flags,
                            std::vector<float> &capeLeanSway, int &accessoryKind,
                            float &headPitchDeg, float &headYawDeg,
                            std::vector<std::pair<std::string, Mat4>> &petBones,
                            Mat4 &petOffset);

/**
 * @brief A parsed geometry mesh plus its bone's pivot (for per-bone animation offsets).
 */
struct PieceMesh {
  pl::cosmetics::geometry::Mesh mesh; ///< vertices/indices, already triangulated
  Vec3 pivot{0, 0, 0};                ///< the owning bone's pivot, for rotation offsets
  bool hasPivot = false;
};

/**
 * @brief Parses the geometry blob (concatenated length-prefixed {@code id,json} entries) and builds
 *        renderable meshes for a given piece id.
 */
PL_EXPORT std::vector<PieceMesh> ParseGeometryForPieceData(std::span<const std::uint8_t> blob,
                                                 std::string_view pieceId);

} // namespace pl::cosmetics