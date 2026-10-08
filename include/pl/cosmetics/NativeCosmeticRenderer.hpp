#pragma once

/**
 * @file NativeCosmeticRenderer.hpp
 * @brief Native mesh rasterization for capes, pets and hats (Task: rasterization).
 *
 * Consumes the launcher's per-frame {@code CosmeticFrame} buffer and the geometry blob (the
 * length-prefixed JSON entries the Java side publishes) and, inside the hooked player-render pass,
 * draws the cosmetic meshes into the game's active render context.
 *
 * <p><b>No resource packs.</b> Everything lives in the preloader: the bone matrices are built from
 * the frame's per-segment/per-bone rotations, the vertices are transformed by those matrices plus a
 * world/player anchor, and the draw is issued through a runtime-resolved GLES function table so the
 * module never hard-links against a specific EGL version.
 *
 * <p><b>Fail-closed by design.</b> Every read is bounds-checked; a missing/invalid frame (magic !=
 * CHF1), an empty/unknown geometry blob, a null texture, or a GLES context that cannot be resolved
 * simply skips the draw. Nothing here dereferences a guessed pointer into the stripped binary; the
 * render-context resolution is signature-scanned and validated before use.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pl/Export.hpp"
#include "pl/cosmetics/NativeCosmeticData.hpp"
#include "pl/cosmetics/NativeModelMatrix.hpp"
#include "pl/cosmetics/animation/AnimationSolver.hpp"
#include "pl/cosmetics/geometry/BedrockGeometry.hpp"

namespace pl::cosmetics {

/**
 * @brief Minimal GL scalar types.
 *
 * Deliberately self-contained: the preloader does not include a GLES header (the host test runner
 * has none), and the renderer only needs the scalar/enum integers to marshal the runtime-resolved
 * GLES function pointers. The numeric values used with them are the standard GL constants.
 */
using GLenum = std::uint32_t;
using GLboolean = std::uint8_t;
using GLint = std::int32_t;
using GLsizei = std::int32_t;
using GLuint = std::uint32_t;
using GLsizeiptr = std::intptr_t;
using GLfloat = float;

inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;

} // namespace pl::cosmetics

namespace pl::cosmetics {

/** @brief Outcome of one cosmetic-frame render attempt. */
enum class CosmeticDrawResult {
  Skipped,  ///< nothing drawn (no frame, no geometry, no live context, or a validation failure)
  Drawn,    ///< one or more mesh pieces were drawn this frame
};

/**
 * @brief The runtime-resolved GLES entry points the renderer needs.
 *
 * Resolved once from the game's active GLES library; a null entry point makes that primitive a
 * no-op, so a build with a partial context still fails closed. Using function pointers keeps the
 * preloader from hard-linking to a GLES version that may not exist on the device's renderer.
 */
struct GlesFunctions {
  void *GetError = nullptr;
  void *GetIntegerv = nullptr;
  void *Enable = nullptr;
  void *Disable = nullptr;
  void *CullFace = nullptr;
  void *DepthMask = nullptr;
  void *GetBooleanv = nullptr;
  void *GenBuffers = nullptr;
  void *BindBuffer = nullptr;
  void *BufferData = nullptr;
  void *GenVertexArrays = nullptr;
  void *BindVertexArray = nullptr;
  void *EnableVertexAttribArray = nullptr;
  void *VertexAttribPointer = nullptr;
  void *UseProgram = nullptr;
  void *GetUniformLocation = nullptr;
  void *UniformMatrix4fv = nullptr;
  void *Uniform4f = nullptr;
  void *Uniform1i = nullptr;
  void *ActiveTexture = nullptr;
  void *BindTexture = nullptr;
  void *GenTextures = nullptr;
  void *TexParameteri = nullptr;
  void *TexImage2D = nullptr;
  void *Viewport = nullptr;
  void *DrawElements = nullptr;
  void *BlendFunc = nullptr;
  void *DeleteBuffers = nullptr;
  void *DeleteVertexArrays = nullptr;
  void *DeleteTextures = nullptr;
  void *GetShaderiv = nullptr;
  void *GetProgramiv = nullptr;
  void *CreateShader = nullptr;
  void *ShaderSource = nullptr;
  void *CompileShader = nullptr;
  void *CreateProgram = nullptr;
  void *AttachShader = nullptr;
  void *LinkProgram = nullptr;
  void *DeleteShader = nullptr;

  /** @brief True when the core draw path is available (nothing essential is null). */
  [[nodiscard]] bool usable() const;
};

/**
 * @brief Parses the launcher's per-frame transform buffer (the {@code CosmeticFrame} wire layout).
 *
 * Thin wrapper over {@link ParseCosmeticFrameData}; kept for API stability.
 */
inline bool ParseCosmeticFrame(std::span<const std::uint8_t> frame,
                               std::uint32_t &flags,
                               std::vector<float> &capeLeanSway,
                               int &accessoryKind, float &headPitchDeg, float &headYawDeg,
                               std::vector<std::pair<std::string, Mat4>> &petBones,
                               Mat4 &petOffset) {
  return ParseCosmeticFrameData(frame, flags, capeLeanSway, accessoryKind, headPitchDeg,
                                headYawDeg, petBones, petOffset);
}

/**
 * @brief Parses the geometry blob (concatenated length-prefixed {@code id,json} entries) and builds
 *        renderable meshes for a given piece id.
 *
 * Thin wrapper over {@link ParseGeometryForPieceData}; kept for API stability.
 */
inline std::vector<PieceMesh> ParseGeometryForPiece(std::span<const std::uint8_t> blob,
                                                    std::string_view pieceId) {
  return ParseGeometryForPieceData(blob, pieceId);
}

/**
 * @brief Resolves the active GLES context's entry points via the platform loader.
 *
 * Fail-closed: if no GLES library can be loaded (Vulkan-only RenderDragon builds), every pointer is
 * null and {@link GlesFunctions::usable} is false.
 */
PL_EXPORT GlesFunctions ResolveGles();

/**
 * @brief The native cosmetic mesh rasterizer.
 */
class PL_EXPORT NativeCosmeticRenderer {
public:
  /** @brief Resolves the GLES surface once (idempotent). Returns false when it is not usable. */
  static bool init();

  /**
   * @brief Renders the equipped cosmetics for the current frame.
   *
   * Called from the hooked player-render pass. Safely skips when there is nothing to draw, and
   * saves/restores the GLES state it touches so the game's own rendering is unaffected.
   *
   * @param frame       the launcher's {@code CosmeticFrame} buffer
   * @param geometryBlob the geometry blob the launcher published
   * @param atlasPixels  the cosmetic texture atlas (RGBA), or null when none
   * @param atlasWidth,atlasHeight the atlas dimensions
   * @param world        the current world-view-projection matrix to anchor the cosmetics (player),
   *                     or identity when unavailable
   */
  static CosmeticDrawResult drawFrame(std::span<const std::uint8_t> frame,
                                      std::span<const std::uint8_t> geometryBlob,
                                      const std::uint8_t *atlasPixels,
                                      int atlasWidth, int atlasHeight, const Mat4 &world);
};

} // namespace pl::cosmetics
