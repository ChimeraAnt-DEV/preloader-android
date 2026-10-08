#pragma once

/**
 * @file NativeRenderHook.hpp
 * @brief Player-render interception and cosmetic matrix anchoring (Task 2).
 *
 * Hooks `LivePlayerRenderer::render` (primary-vtable slot 17, resolved by RTTI name so no per-build
 * code address is baked in) through {@link pl::hooks::DobbyHookManager}, and composes the cosmetic
 * anchor matrix `world * player_bone * cosmetic_offset` from a {@link BoneMatrixSource}.
 *
 * <p><b>Register safety.</b> The exact C++ signature is unrecoverable from the stripped binary, so
 * the detour forwards the full integer argument register set (x0–x7) untouched and dereferences
 * nothing — the same passthrough contract the existing player-render feed uses. That is what keeps
 * an unknown ABI safe; a wrong guess cannot fault the render thread.
 *
 * <p><b>Fail-closed.</b> When no {@link BoneMatrixSource} is installed (the shipped builds do not
 * expose a player bone matrix — game classes export no symbols), the anchor composition reports
 * false and nothing is drawn. The hook still publishes a per-frame tick and call count so the
 * launcher can confirm the seam is live; it never fabricates a matrix.
 */

#include <cstdint>
#include <string_view>

#include "pl/Export.hpp"
#include "pl/cosmetics/NativeModelMatrix.hpp"

namespace pl::cosmetics {

/**
 * @brief Provides the local player's bone matrices for the current frame.
 *
 * The game-data seam. With no provider the render hook draws nothing — an invented bone matrix is
 * worse than no cosmetic, because it would attach the model to a guessed position.
 */
class BoneMatrixSource {
public:
  virtual ~BoneMatrixSource() = default;

  /**
   * @brief Fills `out` with the player-bone matrix for @p bone in player space.
   * @return true when a live matrix was produced this frame
   */
  virtual bool readBoneMatrix(std::string_view bone, Mat4 &out) = 0;
};

/** @brief Installs (or returns) the process-wide bone-matrix source. Pass null to clear it. */
PL_EXPORT void SetBoneMatrixSource(BoneMatrixSource *source);

/** @brief True when a bone-matrix source is installed. */
PL_EXPORT bool HasBoneMatrixSource();

/** @brief Per-frame render statistics published by the hook. */
struct RenderStats {
  std::uint32_t frameTick = 0;  ///< increments once per rendered frame
  std::uint32_t frameCalls = 0; ///< player models drawn this frame (>1 proves non-local coverage)
  std::uint32_t totalCalls = 0; ///< lifetime call count
};

/** @brief The current render statistics. */
PL_EXPORT RenderStats ReadRenderStats();

/**
 * @brief Installs the player-render hook.
 *
 * @param vtableIndex `LivePlayerRenderer` primary-vtable slot; 0 uses the verified default (17).
 * @return true when the detour is live
 */
PL_EXPORT bool InitNativeCosmeticRenderHook(std::size_t vtableIndex = 0);

/** @brief True once the render hook has observed at least one frame. */
PL_EXPORT bool IsNativeCosmeticRenderHookLive();

/**
 * @brief Composes the cosmetic anchor matrix for @p bone with a cosmetic-local offset.
 *
 * Returns false when no bone source is installed or the bone matrix is unavailable this frame.
 */
PL_EXPORT bool BuildCosmeticMatrix(std::string_view bone, const Mat4 &cosmeticOffset, Mat4 &out);

} // namespace pl::cosmetics
