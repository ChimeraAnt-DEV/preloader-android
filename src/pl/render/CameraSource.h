#pragma once

#include <cstdint>

#include "pl/cosmetics/NativeModelMatrix.hpp"

namespace pl::render {

using pl::cosmetics::Mat4;

/**
 * @brief Resolves the game's live view/projection matrices, once, and reads them per frame.
 *
 * The game strips its symbols, so there is no exported camera accessor to call. The approach here
 * mirrors the cosmetic render context: scan the game module's readable ranges for a plausible
 * perspective projection matrix (the characteristic 1/tan(fov) diagonal and -1 in m[11]), then
 * treat the 16 floats immediately preceding it as a candidate view matrix, validating that they
 * form a rigid transform (bottom row 0,0,0,1 and an orthonormal upper-left 3x3). When both
 * validate, `projection * view` is the world->clip matrix a mod can draw with.
 *
 * <p><b>Cost.</b> The scan walks hundreds of megabytes, so it runs exactly once, off the render
 * thread (from callback registration), and caches the address. Per frame this only reads 32 floats
 * from the cached address and re-validates them, which is cheap enough for the render thread.
 *
 * <p><b>Fail-closed.</b> If no plausible pair is found, or the cached address stops looking like a
 * camera, the resolution reports unavailable and the caller falls back to
 * {@code ComputeViewProjection}. It never fabricates a matrix.
 */
class CameraSource {
public:
  struct Frame {
    Mat4 viewProjection{};
    Mat4 view{};
    Mat4 projection{};
    bool hasViewProjection = false;
    bool hasView = false;
    bool hasProjection = false;
  };

  /// Runs the one-time scan. Safe to call repeatedly; only the first call scans.
  static void ResolveOnce();

  /// True when the one-time scan found a candidate camera block.
  static bool HasResolvedAddress();

  /// Reads and validates the camera for this frame. Cheap; safe on the render thread.
  static Frame ReadFrame();

private:
  static bool ValidateProjection(const float *m);
  static bool ValidateView(const float *m);
};

} // namespace pl::render
