#pragma once

/**
 * @file GlesProbe.hpp
 * @brief Runtime probe of the device's graphics backend, for diagnostics only.
 *
 * <p>The GLES entry-point table and the backend classification used to live in the cosmetic mesh
 * renderer. That renderer is gone (the engine texture swap is the only route a cosmetic takes now),
 * but the diagnostics report still has to answer two questions a device run depends on: which
 * backend the game is drawing with, and whether a GLES surface is even available. This module keeps
 * exactly that probe and nothing else - it resolves symbols, it never draws.
 *
 * <p><b>Fail-closed.</b> If no GLES library can be loaded (Vulkan-only RenderDragon builds), every
 * pointer is null, {@link GlesFunctions::usable} is false, and the backend reads as Vulkan or
 * unknown. Nothing here dereferences a guessed address.
 */

#include <cstdint>

#include "pl/Export.hpp"

namespace pl::render {

/**
 * @brief Minimal GL scalar types.
 *
 * Deliberately self-contained: the preloader does not include a GLES header (the host test runner
 * has none), and the probe only needs the scalar/enum integers to marshal the runtime-resolved
 * GLES function pointers.
 */
using GLenum = std::uint32_t;
using GLboolean = std::uint8_t;
using GLint = std::int32_t;
using GLsizei = std::int32_t;
using GLuint = std::uint32_t;
using GLsizeiptr = std::intptr_t;
using GLfloat = float;

/**
 * @brief The runtime-resolved GLES entry points the probe exposes.
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
  void *Uniform3f = nullptr;
  void *Uniform1i = nullptr;
  void *ActiveTexture = nullptr;
  void *BindTexture = nullptr;
  void *GenTextures = nullptr;
  void *TexParameteri = nullptr;
  void *TexImage2D = nullptr;
  void *Viewport = nullptr;
  void *DrawElements = nullptr;
  void *DrawArrays = nullptr;
  void *BlendFunc = nullptr;
  void *DeleteBuffers = nullptr;
  void *DeleteVertexArrays = nullptr;
  void *DeleteTextures = nullptr;

  /** @brief True when the core query path is available (nothing essential is null). */
  [[nodiscard]] bool usable() const;
};

/**
 * @brief Resolves the active GLES context's entry points via the platform loader.
 *
 * Fail-closed: if no GLES library can be loaded (Vulkan-only RenderDragon builds), every pointer is
 * null and {@link GlesFunctions::usable} is false.
 */
PL_EXPORT GlesFunctions ResolveGles();

/** @brief GL backend classification for the diagnostics report. */
enum class GlBackend : int {
  Unknown = 0,  ///< no GLES library resolved and no Vulkan library found
  Gles = 1,     ///< the GLES function table resolved (an ES renderer)
  Vulkan = 2,   ///< no GLES, but libvulkan.so is loaded (a Vulkan-only RenderDragon build)
};

/**
 * @brief True when an EGL context is current on this thread.
 *
 * Resolved via EGL's {@code eglGetCurrentContext}; when the EGL symbol is unavailable this returns
 * false (fail-closed: never claim a context we cannot confirm).
 */
PL_EXPORT bool IsEglContextCurrent();

/** @brief The GL backend classification, for the diagnostics report. */
PL_EXPORT GlBackend GlesBackend();

/** @brief The last GLES error code observed, or 0 when clean. */
PL_EXPORT int LastGlesError();

} // namespace pl::render
