#pragma once

/**
 * @file RenderDispatch.h
 * @brief Internal seam between the render hook and the public render-callback registry.
 *
 * The player-render detour (NativeRenderHook.cpp) owns the only render-thread, GL-context-current
 * point in the process, so it is what dispatches the public callbacks registered through
 * `pl/Render.hpp`. This header carries just that one call across the two translation units.
 */

namespace pl::render::detail {

/// Invokes every registered render callback for this frame. Render thread only.
void DispatchRenderCallbacks();

} // namespace pl::render::detail
