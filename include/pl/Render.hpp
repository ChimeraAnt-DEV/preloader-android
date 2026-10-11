#pragma once

/**
 * @file Render.hpp
 * @brief Per-frame render callback + camera matrix API for native mods.
 *
 * The public surface a mod uses to draw into the game's own frame. Two things had to exist before
 * a mod could draw in world space, and this header is both:
 *
 *  1. **A per-frame callback that runs on the render thread with the GL context current.** The
 *     preloader already detours the game's per-player render pass (it is how the native cosmetics
 *     are drawn); this exposes that same seam to mods. The callback runs inside the GL context, so
 *     a mod may issue GL calls directly.
 *  2. **Reliable camera matrices.** A mod needs `view`, `projection` and the camera position to
 *     place geometry in the world. {@link RenderContext} carries the strongest matrices the
 *     preloader could resolve this frame, and {@link ComputeViewProjection} is the fallback a mod
 *     can call itself when the real camera is unavailable.
 *
 * <b>Fail-closed.</b> Every field is optional and carries a `has*` flag. A callback that receives a
 * context with `hasViewProjection == false` must not assume a camera; it should either use
 * {@link ComputeViewProjection} (which needs only a live local player) or skip the frame. Nothing
 * here fabricates a matrix.
 *
 * <b>Threading.</b> {@link RegisterRenderCallback} may be called from any thread (typically a mod's
 * `load`/`enable`). The callback itself always runs on the render thread, once per rendered frame,
 * so it must be fast and must not block or allocate unboundedly.
 */

#include <cstdint>
#include <functional>

#include "pl/Export.hpp"
#include "pl/cosmetics/NativeModelMatrix.hpp"

namespace pl::render {

using pl::cosmetics::Mat4;
using pl::cosmetics::Vec3;

/**
 * @brief The camera state for the current frame.
 *
 * All matrices are column-major, matching OpenGL ES and {@link Mat4}, so they can be uploaded to a
 * uniform without transposing.
 */
struct RenderContext {
  /// World -> clip space. Present only when the game's real camera was resolved this frame.
  Mat4 viewProjection{};
  bool hasViewProjection = false;

  /// World -> view space.
  Mat4 view{};
  bool hasView = false;

  /// View -> clip space.
  Mat4 projection{};
  bool hasProjection = false;

  /// Camera eye position in world coordinates.
  Vec3 cameraPosition{};
  bool hasCameraPosition = false;

  /// Camera forward direction in world coordinates (unit length) when known.
  Vec3 cameraForward{};
  bool hasCameraForward = false;

  /// The GL viewport size in pixels for this frame (0 when unknown).
  int viewportWidth = 0;
  int viewportHeight = 0;

  /// A monotonically increasing frame counter, shared with {@link RenderFrameTick}.
  std::uint64_t frameTick = 0;
};

/** @brief The per-frame callback type. Runs on the render thread with the GL context current. */
using RenderCallback = std::function<void(const RenderContext &)>;

/**
 * @brief Registers a per-frame render callback.
 *
 * The callback is invoked once per rendered frame, on the render thread, after the game has drawn
 * the frame's player pass, so the GL context is current and a mod may issue GL calls. Multiple
 * callbacks may be registered; they run in registration order. Registering the same callable twice
 * registers it twice.
 *
 * @return true when the callback was stored. It does not guarantee the render seam is live on this
 *         build — check {@link IsRenderCallbackLive} for that.
 */
PL_EXPORT bool RegisterRenderCallback(RenderCallback callback);

/**
 * @brief Removes a previously registered callback.
 *
 * A `std::function` is not comparable, so removal is by clearing the registry of the *most recently
 * registered* callback (the common "unregister my one callback" case). To remove every callback,
 * pass `true` for @p all.
 *
 * @return the number of callbacks removed
 */
PL_EXPORT std::size_t UnregisterRenderCallbacks(bool all = false);

/** @brief True once the render seam has observed at least one frame this session. */
PL_EXPORT bool IsRenderCallbackLive();

/** @brief The number of frames the render seam has dispatched. */
PL_EXPORT std::uint64_t RenderFrameTick();

/**
 * @brief Computes a world -> clip matrix from the live local player when the real camera is unknown.
 *
 * This is the documented fallback for {@link RenderContext::viewProjection}. It builds the view
 * from the player's world position and yaw/pitch and the projection from a standard perspective
 * with the given field of view, aspect ratio and clip planes. It is a first-person approximation:
 * it places the eye at the player's head, which is correct in first person and close in third
 * person (where the real camera is offset behind the player). Use the real camera when
 * {@link RenderContext::hasViewProjection} is true; use this only as a fallback.
 *
 * @param fovDegrees vertical field of view (Bedrock's default is 70)
 * @param aspectRatio viewport width / height
 * @param nearPlane,farPlane clip distances
 * @return true when a live local player was available and the matrix was written to @p out
 */
PL_EXPORT bool ComputeViewProjection(float fovDegrees, float aspectRatio, float nearPlane,
                                     float farPlane, Mat4 &out);

/**
 * @brief A perspective projection matrix (column-major), independent of any game state.
 *
 * Exposed so a mod can build its own projection when it knows the FOV and viewport. Written to
 * @p out; always returns true.
 */
PL_EXPORT bool MakePerspective(float fovDegrees, float aspectRatio, float nearPlane,
                               float farPlane, Mat4 &out);

/**
 * @brief A right-handed view matrix looking from @p eye toward @p eye + @p forward.
 *
 * @p up is the world up hint; it is re-orthogonalised against @p forward. Written to @p out.
 */
PL_EXPORT bool MakeView(const Vec3 &eye, const Vec3 &forward, const Vec3 &up, Mat4 &out);

/**
 * @brief Reads the local player's world position.
 *
 * The same feed the in-world voice nametag icons use. A mod that wants to anchor geometry at the
 * player (a schematic placement, a marker) needs this and it was previously internal to the
 * preloader. Returns false when no live player has been seen, so a caller never places anything at
 * a guessed position.
 */
PL_EXPORT bool ReadLocalPlayerPosition(float out[3]);

/**
 * @brief Reads the local player's view rotation in degrees: yaw then pitch.
 * @return true when a live rotation was read.
 */
PL_EXPORT bool ReadLocalPlayerRotation(float out[2]);

} // namespace pl::render
