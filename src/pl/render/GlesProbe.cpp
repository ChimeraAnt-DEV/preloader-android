/**
 * @file GlesProbe.cpp
 * @brief Runtime GLES/EGL probe for the diagnostics report.
 *
 * Extracted from the retired cosmetic mesh renderer: the renderer drew with these entry points, the
 * probe only classifies them. Nothing here draws, allocates GL objects, or dereferences a guessed
 * address - it dlopens the GLES/EGL/Vulkan libraries and resolves the symbols the report reads.
 */

#include "pl/render/GlesProbe.hpp"

#include <atomic>

#include <dlfcn.h>

#include "pl/Logger.hpp"

namespace pl::render {
namespace {

GlesFunctions g_gles;

void *LoadGlesLibrary() {
  for (const char *candidate : {"libGLESv3.so", "libGLESv2.so", "libGLESv1_CM.so"}) {
    void *handle = dlopen(candidate, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(candidate, RTLD_NOW);
    if (handle) return handle;
  }
  return nullptr;
}

void *GlesSymbol(void *handle, const char *name) {
  return handle ? dlsym(handle, name) : nullptr;
}

/// Set when {@link LastGlesError} is called, so a clean run reads 0 without touching GL.
std::atomic<int> g_lastGlesError{0};

} // namespace

bool GlesFunctions::usable() const {
  return GetError && GetIntegerv && Enable && Disable && GenBuffers && BindBuffer &&
         BufferData && GenVertexArrays && BindVertexArray && EnableVertexAttribArray &&
         VertexAttribPointer && UseProgram && GetUniformLocation && UniformMatrix4fv &&
         ActiveTexture && BindTexture && GenTextures && TexParameteri && TexImage2D &&
         DrawElements && Viewport;
}

GlesFunctions ResolveGles() {
  if (g_gles.usable()) return g_gles;

  void *handle = LoadGlesLibrary();
  if (!handle) {
    preloaderLogger.warn("GLES probe: no GLES library loaded (RenderDragon may be Vulkan-only)");
    return g_gles;
  }

#define LOAD(name, member) \
  g_gles.member = reinterpret_cast<decltype(g_gles.member)>(GlesSymbol(handle, name))
  LOAD("glGetError", GetError);
  LOAD("glGetIntegerv", GetIntegerv);
  LOAD("glEnable", Enable);
  LOAD("glDisable", Disable);
  LOAD("glCullFace", CullFace);
  LOAD("glDepthMask", DepthMask);
  LOAD("glGetBooleanv", GetBooleanv);
  LOAD("glGenBuffers", GenBuffers);
  LOAD("glBindBuffer", BindBuffer);
  LOAD("glBufferData", BufferData);
  LOAD("glGenVertexArrays", GenVertexArrays);
  LOAD("glBindVertexArray", BindVertexArray);
  LOAD("glEnableVertexAttribArray", EnableVertexAttribArray);
  LOAD("glVertexAttribPointer", VertexAttribPointer);
  LOAD("glUseProgram", UseProgram);
  LOAD("glGetUniformLocation", GetUniformLocation);
  LOAD("glUniformMatrix4fv", UniformMatrix4fv);
  LOAD("glUniform4f", Uniform4f);
  LOAD("glUniform3f", Uniform3f);
  LOAD("glUniform1i", Uniform1i);
  LOAD("glActiveTexture", ActiveTexture);
  LOAD("glBindTexture", BindTexture);
  LOAD("glGenTextures", GenTextures);
  LOAD("glTexParameteri", TexParameteri);
  LOAD("glTexImage2D", TexImage2D);
  LOAD("glViewport", Viewport);
  LOAD("glDrawElements", DrawElements);
  LOAD("glDrawArrays", DrawArrays);
  LOAD("glBlendFunc", BlendFunc);
  LOAD("glDeleteBuffers", DeleteBuffers);
  LOAD("glDeleteVertexArrays", DeleteVertexArrays);
  LOAD("glDeleteTextures", DeleteTextures);
#undef LOAD

  if (g_gles.usable()) {
    preloaderLogger.info("GLES probe: surface resolved and usable");
  } else {
    preloaderLogger.warn("GLES probe: GLES library loaded but core symbols missing");
  }
  return g_gles;
}

bool IsEglContextCurrent() {
  // eglGetCurrentContext returns EGL_NO_CONTEXT (0) when no context is current on this thread.
  // Loaded through dlopen so the probe never hard-links EGL and a build without it reads false.
  static void *handle = []() -> void * {
    for (const char *name : {"libEGL.so", "libEGL.so.1"}) {
      void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
      if (!h) h = dlopen(name, RTLD_NOW);
      if (h) return h;
    }
    return nullptr;
  }();
  if (!handle) return false;
  using GetCurrentContextFn = void *(*)();
  auto getCurrentContext = reinterpret_cast<GetCurrentContextFn>(dlsym(handle, "eglGetCurrentContext"));
  if (!getCurrentContext) return false;
  return getCurrentContext() != nullptr;
}

GlBackend GlesBackend() {
  if (g_gles.usable()) return GlBackend::Gles;
  // No GLES table: distinguish "Vulkan-only RenderDragon" from "nothing resolved" by probing for a
  // loaded Vulkan library, which is what a Vulkan-only build links.
  for (const char *name : {"libvulkan.so", "libvulkan.so.1"}) {
    void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen(name, RTLD_NOW);
    if (h) return GlBackend::Vulkan;
  }
  return GlBackend::Unknown;
}

int LastGlesError() {
  return g_lastGlesError.load(std::memory_order_relaxed);
}

} // namespace pl::render
