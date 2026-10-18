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

/**
 * @brief Reads a PNG's IHDR width/height without decoding the image.
 *
 * The image-loader seam hands over raw source bytes with no id, so the only way to recognise a cape
 * (or a skin) is by its shape, which means reading the IHDR. Only the header is inspected; the
 * pixel data is never touched. Returns false for anything that is not a PNG with a complete IHDR.
 */
PL_EXPORT bool PngDimensions(const std::uint8_t *data, std::size_t size, std::uint32_t *width,
                             std::uint32_t *height);

/**
 * @brief True when a width/height pair is a cape panel.
 *
 * A cape texture is a landscape 2:1 panel (vanilla is 64x32), so it is recognised by the exact 2:1
 * ratio rather than by a fixed size: a higher-resolution authored cape is still 2:1, and a square
 * skin atlas is excluded, which is what keeps the skin capture and the cape capture from colliding
 * on the one seam that carries both.
 */
PL_EXPORT bool IsCapeShapedSize(std::uint32_t width, std::uint32_t height);

/**
 * @brief The cape panel dimensions for a payload in a given loader format, or false when it is not
 *        a cape.
 *
 * @param format the {@code mce::ImageFormat} value the engine passed (0 = auto/PNG, 4 = RGBA8)
 * @param data   the source bytes the engine is loading
 * @param size   the byte length
 * @param width  receives the panel width on success
 * @param height receives the panel height on success
 */
PL_EXPORT bool CapePanelForPayload(std::uint32_t format, const std::uint8_t *data,
                                   std::size_t size, std::uint32_t *width,
                                   std::uint32_t *height);

} // namespace pl::cosmetics