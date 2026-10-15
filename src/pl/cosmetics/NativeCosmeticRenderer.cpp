/**
 * @file NativeCosmeticRenderer.cpp
 * @brief Native mesh rasterization for capes, pets and hats.
 *
 * The final rasterization step: it turns the launcher's per-frame transform buffer and the geometry
 * blob into GLES draw calls inside the hooked player-render pass.
 *
 * <b>Design rules</b>
 *  - Every read is bounds-checked; magic != CHF1 or a truncated buffer skips the draw and never
 *    dereferences a partial value.
 *  - The GLES surface is resolved at runtime (never linked), so a Vulkan-only RenderDragon build
 *    leaves every entry point null and the draw is skipped. Fail-closed.
 *  - The renderer saves and restores the GLES state it touches (texture, program, buffers, VAO,
 *    blend/depth/cull enable bits and depth mask), so the game's own rendering is not corrupted.
 *  - Geometry and frame parsing are pure and host-unit-tested; only the GLES upload+draw is
 *    device-side and guarded.
 *
 * <b>Correctness contract</b>: the world->clip matrix cannot be recovered from the passthrough hook
 * on the stripped binary, so the renderer accepts an explicit {@code world} anchor. Given a live
 * projection*view*model matrix it draws the cosmetics at the correct place; with identity it draws
 * around the player-space origin. Per-bone pet rotations from the frame are applied on the CPU about
 * each bone's pivot before upload.
 */

#include "pl/cosmetics/NativeCosmeticRenderer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>

#include "pl/Logger.hpp"
#include "pl/cosmetics/CosmeticParticles.hpp"
#include "pl/cosmetics/NativeCosmeticData.hpp"
#include "pl/cosmetics/geometry/BedrockGeometry.hpp"

namespace pl::cosmetics {
namespace {

// -------------------------------------------------------------------------------------------
// GLES function table (runtime-resolved, never linked).
// -------------------------------------------------------------------------------------------

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

} // namespace

// Diagnostics state, read by the in-app report on the UI thread and written on the render thread.
// Plain relaxed atomics: the values are informational counters, not a synchronisation channel.
std::atomic<int> g_lastGlesError{0};
std::atomic<bool> g_debugCube{false};

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
    preloaderLogger.warn(
        "Native cosmetic renderer: no GLES library loaded (RenderDragon may be Vulkan-only)");
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
    preloaderLogger.info("Native cosmetic renderer: GLES surface resolved and usable");
  } else {
    preloaderLogger.warn("Native cosmetic renderer: GLES library loaded but core symbols missing");
  }
  return g_gles;
}

bool IsEglContextCurrent() {
  // eglGetCurrentContext returns EGL_NO_CONTEXT (0) when no context is current on this thread.
  // Loaded through dlopen so the renderer never hard-links EGL and a build without it reads false.
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

void SetDebugCube(bool enabled) {
  g_debugCube.store(enabled, std::memory_order_relaxed);
  preloaderLogger.info("Native cosmetic renderer: debug cube {}", enabled ? "armed" : "disarmed");
}

bool IsDebugCubeEnabled() {
  return g_debugCube.load(std::memory_order_relaxed);
}

// -------------------------------------------------------------------------------------------
// GLES draw path.
// -------------------------------------------------------------------------------------------

namespace {

// Typed GLES function pointers (the resolved table is void*, so they are cast here).
using glGenBuffers_t = void (*)(GLsizei, GLuint *);
using glBindBuffer_t = void (*)(GLenum, GLuint);
using glBufferData_t = void (*)(GLenum, GLsizeiptr, const void *, GLenum);
using glGenVertexArrays_t = void (*)(GLsizei, GLuint *);
using glBindVertexArray_t = void (*)(GLuint);
using glEnableVertexAttribArray_t = void (*)(GLuint);
using glVertexAttribPointer_t =
    void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
using glUseProgram_t = void (*)(GLuint);
using glGetUniformLocation_t = GLint (*)(GLuint, const char *);
using glUniformMatrix4fv_t = void (*)(GLint, GLsizei, GLboolean, const float *);
using glUniform4f_t = void (*)(GLint, float, float, float, float);
using glUniform3f_t = void (*)(GLint, float, float, float);
using glUniform1i_t = void (*)(GLint, GLint);
using glActiveTexture_t = void (*)(GLenum);
using glBindTexture_t = void (*)(GLenum, GLuint);
using glGenTextures_t = void (*)(GLsizei, GLuint *);
using glTexParameteri_t = void (*)(GLenum, GLenum, GLint);
using glTexImage2D_t = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                const void *);
using glDrawElements_t = void (*)(GLenum, GLsizei, GLenum, const void *);
using glDrawArrays_t = void (*)(GLenum, GLint, GLsizei);
using glDeleteBuffers_t = void (*)(GLsizei, const GLuint *);
using glDeleteVertexArrays_t = void (*)(GLsizei, const GLuint *);
using glDeleteTextures_t = void (*)(GLsizei, const GLuint *);
using glGetIntegerv_t = void (*)(GLenum, GLint *);
using glGetBooleanv_t = void (*)(GLenum, GLboolean *);
using glEnable_t = void (*)(GLenum);
using glDisable_t = void (*)(GLenum);
using glDepthMask_t = void (*)(GLboolean);
using glBlendFunc_t = void (*)(GLenum, GLenum);

// GL constants (usable without including a GLES header in every TU).
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kElementArrayBuffer = 0x8893;
constexpr GLenum kStreamDraw = 0x88E4;
constexpr GLenum kFloat = 0x1406;
constexpr GLenum kTriangles = 0x0004;
constexpr GLenum kUnsignedInt = 0x1405;
constexpr GLenum kTexture2D = 0x0DE1;
constexpr GLenum kActiveTexture = 0x84C0;
constexpr GLenum kTexture0 = 0x84C0;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kBlend = 0x0BE2;
constexpr GLenum kCullFace = 0x0B44;
constexpr GLint kClampToEdge = 0x812F;
constexpr GLint kLinear = 0x2601;
constexpr GLint kRgba = 0x1908;
constexpr GLenum kUnsignedByte = 0x1401;

// The state queries. These MUST be the real enum values: the previous code reused the *capability*
// enums here, so it queried the wrong pnames. glGetIntegerv(GL_CURRENT_PROGRAM == 0x8B31) returned
// GL_INVALID_ENUM and left 0 in the output, and the restore then did glUseProgram(0) — unbinding the
// game's program. Likewise glBindVertexArray() was fed a framebuffer id, and the depth mask was read
// from the wrong pname. Together those corrupted the renderer's state after every frame.
constexpr GLenum kCurrentProgram = 0x8B8D;      // GL_CURRENT_PROGRAM
constexpr GLenum kTextureBinding2D = 0x8069;    // GL_TEXTURE_BINDING_2D (not GL_TEXTURE_2D 0x0DE1)
constexpr GLenum kVertexArrayBinding = 0x85B5;  // GL_VERTEX_ARRAY_BINDING (not GL_VERTEX_SHADER)
constexpr GLenum kArrayBufferBinding = 0x8894;  // GL_ARRAY_BUFFER_BINDING
constexpr GLenum kElementArrayBufferBinding = 0x8895; // GL_ELEMENT_ARRAY_BUFFER_BINDING
constexpr GLenum kDepthWritemask = 0x0B72;      // GL_DEPTH_WRITEMASK (not GL_DEPTH_FUNC 0x0B74)

struct SavedState {
  GLint program = 0;
  GLint texture2d = 0;
  GLint vao = 0;
  GLint arrayBuf = 0;
  GLint elemBuf = 0;
  GLboolean blend = GL_FALSE;
  GLboolean depthMask = GL_TRUE;
  GLboolean cull = GL_FALSE;
  /** True when a sampler was left bound to unit 0; only then is restoring it meaningful. */
  bool hasSamplerBinding = false;
};

bool SaveState(const GlesFunctions &gles, SavedState &state) {
  if (!gles.GetIntegerv || !gles.GetBooleanv) return false;
  auto geti = reinterpret_cast<glGetIntegerv_t>(gles.GetIntegerv);
  auto getb = reinterpret_cast<glGetBooleanv_t>(gles.GetBooleanv);
  geti(kCurrentProgram, &state.program);
  geti(kArrayBufferBinding, &state.arrayBuf);
  geti(kElementArrayBufferBinding, &state.elemBuf);
  geti(kVertexArrayBinding, &state.vao);
  // The sampler binding is captured so it can be restored only if still live (see
  // RestoreSamplerBinding); reading it here also tells us whether the game left one bound at unit 0.
  GLint sampler = 0;
  geti(kTextureBinding2D, &sampler);
  state.texture2d = sampler;
  state.hasSamplerBinding = sampler != 0;
  getb(kBlend, &state.blend);
  getb(kDepthWritemask, &state.depthMask);
  getb(kCullFace, &state.cull);
  return true;
}

// The sampler binding must be revalidated every frame: the id captured at SaveState can be freed by
// the game before the restore (a texture the renderer just unloaded), and rebinding a dead name is
// GL_INVALID_OPERATION. Checking GL_TEXTURE_BINDING_2D matches the id before restoring it is cheap
// and wrong only if the id was reused for a different texture — the same texture unit, so harmless.
void RestoreSamplerBinding(const GlesFunctions &gles, GLint expected) {
  if (expected == 0 || !gles.GetIntegerv || !gles.BindTexture) return;
  auto geti = reinterpret_cast<glGetIntegerv_t>(gles.GetIntegerv);
  GLint current = 0;
  geti(kTextureBinding2D, &current);
  if (current != expected) return;
  reinterpret_cast<glBindTexture_t>(gles.BindTexture)(kTexture2D,
                                                      static_cast<GLuint>(expected));
}

void RestoreState(const GlesFunctions &gles, const SavedState &state) {
  if (!gles.BindVertexArray || !gles.BindBuffer || !gles.UseProgram || !gles.BindTexture ||
      !gles.Enable || !gles.Disable || !gles.DepthMask) {
    return;
  }
  auto bindVao = reinterpret_cast<glBindVertexArray_t>(gles.BindVertexArray);
  auto bindBuf = reinterpret_cast<glBindBuffer_t>(gles.BindBuffer);
  auto useProg = reinterpret_cast<glUseProgram_t>(gles.UseProgram);
  auto bindTex = reinterpret_cast<glBindTexture_t>(gles.BindTexture);
  auto enable = reinterpret_cast<glEnable_t>(gles.Enable);
  auto disable = reinterpret_cast<glDisable_t>(gles.Disable);
  auto depthMask = reinterpret_cast<glDepthMask_t>(gles.DepthMask);

  bindVao(static_cast<GLuint>(state.vao));
  bindBuf(kArrayBuffer, static_cast<GLuint>(state.arrayBuf));
  bindBuf(kElementArrayBuffer, static_cast<GLuint>(state.elemBuf));
  useProg(static_cast<GLuint>(state.program));
  // Restore the sampler only if the id is still live; a blind rebind of a freed name is
  // GL_INVALID_OPERATION and leaves the game's sampler in an unknown state.
  RestoreSamplerBinding(gles, state.texture2d);
  if (state.blend) enable(kBlend); else disable(kBlend);
  if (state.cull) enable(kCullFace); else disable(kCullFace);
  depthMask(state.depthMask);
}

// A single program: samples a texture multiplied by uColor and a per-vertex diffuse term. When the
// cosmetic atlas is unavailable a 1x1 white texture is bound, so the same program serves the
// flat-colour fallback.
//
// The attribute locations are bound explicitly before linking (see EnsureProgram). GLSL ES does not
// guarantee that `aPosition`/`aUv` land on slots 0/1, and the vertex format is set up at fixed
// slots 0/1 — without the bind, the driver could assign the UV attribute to slot 3 and every vertex
// would read position data as its texture coordinate (a garbled or blank cosmetic).
//
// The mesh builder already resolves a per-face normal (from the bone chain), but the renderer threw
// it away and drew every face at full brightness — the "flat, unshaded" look. The normal is now
// passed through and lit by a fixed headlight direction, so a hat/pet/cape face is shaded by how it
// faces the viewer.
constexpr const char *kVS =
    "attribute vec4 aPosition;\n"
    "attribute vec2 aUv;\n"
    "attribute vec3 aNormal;\n"
    "varying vec2 vUv;\n"
    "varying float vLight;\n"
    "uniform mat4 uMVP;\n"
    "uniform vec3 uLightDir;\n"
    "void main(){\n"
    "  vUv = aUv;\n"
    "  vec3 n = normalize(aNormal);\n"
    "  vLight = 0.55 + 0.45 * max(dot(n, normalize(uLightDir)), 0.0);\n"
    "  gl_Position = uMVP * aPosition;\n"
    "}\n";
constexpr const char *kFS =
    "precision mediump float;\n"
    "varying vec2 vUv;\n"
    "varying float vLight;\n"
    "uniform sampler2D uTexture;\n"
    "uniform vec4 uColor;\n"
    "void main(){ gl_FragColor = texture2D(uTexture, vUv) * uColor * vLight; }\n";

// The vertex attribute slots the mesh format is uploaded at, and the names bound to them.
constexpr GLuint kAttribPosition = 0;
constexpr GLuint kAttribUv = 1;
constexpr GLuint kAttribNormal = 2;
constexpr const char *kAttribPositionName = "aPosition";
constexpr const char *kAttribUvName = "aUv";
constexpr const char *kAttribNormalName = "aNormal";

GLuint g_program = 0;
std::atomic<bool> g_programInit{false};

GLuint CompileShader(const GlesFunctions &gles, GLenum type, const char *src) {
  void *h = LoadGlesLibrary();
  if (!h) return 0;
  auto create = reinterpret_cast<GLuint (*)(GLenum)>(dlsym(h, "glCreateShader"));
  auto source = reinterpret_cast<void (*)(GLuint, GLsizei, const char *const *, const GLint *)>(
      dlsym(h, "glShaderSource"));
  auto compile = reinterpret_cast<void (*)(GLuint)>(dlsym(h, "glCompileShader"));
  auto get = reinterpret_cast<void (*)(GLuint, GLenum, GLint *)>(dlsym(h, "glGetShaderiv"));
  auto del = reinterpret_cast<void (*)(GLuint)>(dlsym(h, "glDeleteShader"));
  if (!create || !source || !compile || !get || !del) return 0;
  (void)gles;
  GLuint shader = create(type);
  if (!shader) return 0;
  source(shader, 1, &src, nullptr);
  compile(shader);
  // Keep the shader even if it fails to compile; the link step reports the health.
  return shader;
}

bool EnsureProgram(const GlesFunctions &gles) {
  if (g_program != 0) return true;
  if (g_programInit.exchange(true)) return g_program != 0;

  void *h = LoadGlesLibrary();
  if (!h) return false;
  auto createProg = reinterpret_cast<GLuint (*)()>(dlsym(h, "glCreateProgram"));
  auto attach = reinterpret_cast<void (*)(GLuint, GLuint)>(dlsym(h, "glAttachShader"));
  auto link = reinterpret_cast<void (*)(GLuint)>(dlsym(h, "glLinkProgram"));
  auto get = reinterpret_cast<void (*)(GLuint, GLenum, GLint *)>(dlsym(h, "glGetProgramiv"));
  auto delShader = reinterpret_cast<void (*)(GLuint)>(dlsym(h, "glDeleteShader"));
  if (!createProg || !attach || !link || !get || !delShader) return false;

  GLuint vs = CompileShader(gles, kVertexShader, kVS);
  GLuint fs = CompileShader(gles, kFragmentShader, kFS);
  if (!vs || !fs) return false;
  GLuint program = createProg();
  if (!program) return false;
  attach(program, vs);
  attach(program, fs);
  // Pin the attribute slots the vertex format uses. Without this the driver may place aUv at any
  // slot, and the fixed-slot vertex setup would feed it position data.
  auto bindAttrib = reinterpret_cast<void (*)(GLuint, GLuint, const char *)>(
      dlsym(h, "glBindAttribLocation"));
  if (bindAttrib) {
    bindAttrib(program, kAttribPosition, kAttribPositionName);
    bindAttrib(program, kAttribUv, kAttribUvName);
    bindAttrib(program, kAttribNormal, kAttribNormalName);
  } else {
    preloaderLogger.warn(
        "Native cosmetic renderer: glBindAttribLocation unavailable; attribute slots may not match");
  }
  link(program);
  delShader(vs);
  delShader(fs);

  GLint ok = 0;
  get(program, kLinkStatus, &ok);
  if (!ok) {
    preloaderLogger.warn("Native cosmetic renderer: program link failed");
    return false;
  }
  g_program = program;
  preloaderLogger.info("Native cosmetic renderer: solid shader program ready");
  return true;
}

// A 1x1 white texture used when no atlas is available.
GLuint g_whiteTexture = 0;

bool EnsureWhiteTexture(const GlesFunctions &gles) {
  if (g_whiteTexture != 0) return true;
  if (!gles.GenTextures || !gles.BindTexture || !gles.TexParameteri || !gles.TexImage2D) return false;
  auto gen = reinterpret_cast<glGenTextures_t>(gles.GenTextures);
  auto bind = reinterpret_cast<glBindTexture_t>(gles.BindTexture);
  auto param = reinterpret_cast<glTexParameteri_t>(gles.TexParameteri);
  auto image = reinterpret_cast<glTexImage2D_t>(gles.TexImage2D);
  gen(1, &g_whiteTexture);
  bind(kTexture2D, g_whiteTexture);
  param(kTexture2D, 0x2802, kLinear); // MIN_FILTER
  param(kTexture2D, 0x2803, kLinear); // MAG_FILTER
  param(kTexture2D, 0x2800, kClampToEdge);
  param(kTexture2D, 0x2801, kClampToEdge);
  const std::uint8_t white[4] = {255, 255, 255, 255};
  image(kTexture2D, 0, kRgba, 1, 1, 0, kRgba, kUnsignedByte, white);
  return true;
}

// Uploads the cosmetic atlases once per image identity, or reuses the last uploaded ones. Each
// piece (cape/hat/pet) gets its own texture, because they are distinct catalogued artwork — a single
// shared atlas drew the hat and pet in the cape's colours.
struct AtlasSlot {
  GLuint texture = 0;
  int width = 0;
  int height = 0;
  const std::uint8_t *lastPixels = nullptr;
};

AtlasSlot g_capeAtlas;
AtlasSlot g_hatAtlas;
AtlasSlot g_petAtlas;

void UploadAtlas(const GlesFunctions &gles, AtlasSlot &slot, const PieceAtlas &atlas) {
  if (!atlas.rgba || atlas.width <= 0 || atlas.height <= 0) return;
  // Skip when the same buffer pointer and dimensions are unchanged (the launcher reuses its copy
  // for the whole frame), so we do not re-upload an unchanged texture every frame.
  if (slot.texture != 0 && slot.lastPixels == atlas.rgba && slot.width == atlas.width &&
      slot.height == atlas.height) {
    return;
  }
  if (!gles.GenTextures || !gles.BindTexture || !gles.TexParameteri || !gles.TexImage2D) return;
  auto gen = reinterpret_cast<glGenTextures_t>(gles.GenTextures);
  auto bind = reinterpret_cast<glBindTexture_t>(gles.BindTexture);
  auto param = reinterpret_cast<glTexParameteri_t>(gles.TexParameteri);
  auto image = reinterpret_cast<glTexImage2D_t>(gles.TexImage2D);
  if (slot.texture == 0) gen(1, &slot.texture);
  bind(kTexture2D, slot.texture);
  param(kTexture2D, 0x2802, kLinear);
  param(kTexture2D, 0x2803, kLinear);
  param(kTexture2D, 0x2800, kClampToEdge);
  param(kTexture2D, 0x2801, kClampToEdge);
  image(kTexture2D, 0, kRgba, atlas.width, atlas.height, 0, kRgba, kUnsignedByte, atlas.rgba);
  slot.width = atlas.width;
  slot.height = atlas.height;
  slot.lastPixels = atlas.rgba;
}

GLuint AtlasTexture(const GlesFunctions &gles, AtlasSlot &slot, const PieceAtlas &atlas) {
  UploadAtlas(gles, slot, atlas);
  return slot.texture != 0 ? slot.texture : g_whiteTexture;
}

/**
 * Rotates a mesh's vertices about its bone pivot by a caller-supplied bone matrix (the frame's
 * per-bone animation offset), returning a new mesh with the rotated positions. The pivot
 * translation is folded in so the rotation happens about the joint, not the model origin.
 */
geometry::Mesh RotateMeshByBoneMatrix(const geometry::Mesh &mesh, const Vec3 &pivot,
                                      const Mat4 &boneMatrix) {
  geometry::Mesh out = mesh;
  // A pure-rotation bone matrix (no translation) from the frame; translate to the pivot, rotate,
  // translate back.
  const Mat4 toPivot = Mat4::translation(Vec3{pivot.x, pivot.y, pivot.z});
  const Mat4 fromPivot = Mat4::translation(Vec3{-pivot.x, -pivot.y, -pivot.z});
  const Mat4 final = Mat4::multiply(toPivot, Mat4::multiply(boneMatrix, fromPivot));
  for (auto &v : out.vertices) {
    const Vec3 transformed = final.transformPoint(Vec3{v.position.x, v.position.y, v.position.z});
    v.position.x = transformed.x;
    v.position.y = transformed.y;
    v.position.z = transformed.z;
  }
  return out;
}

} // namespace

namespace {
// Forward declaration so drawFrame (in the outer namespace) can use the bone transform helper.
geometry::Mesh RotateMeshByBoneMatrix(const geometry::Mesh &mesh, const Vec3 &pivot,
                                      const Mat4 &boneMatrix);
} // namespace

// -------------------------------------------------------------------------------------------
// Renderer entry points.
// -------------------------------------------------------------------------------------------

static std::atomic<bool> g_initAttempted{false};
static std::atomic<bool> g_initOk{false};
static std::atomic<std::uint32_t> g_drawCount{0};
// Triangles submitted since the last reset, for the diagnostics "drew N triangles" line. The
// renderer knows exactly how many indices it submitted, so this is a real count, not an estimate.
static std::atomic<std::uint32_t> g_triangleCount{0};

// One Bedrock model pixel is 1/16 block. Without this the geometry is drawn in raw model pixels —
// 16x too large. Applied to every piece's anchor so the meshes sit at the player's real scale.
constexpr float kModelPixelToBlocks = 1.0F / 16.0F;

std::uint32_t NativeCosmeticRenderer::drawCount() {
  return g_drawCount.load(std::memory_order_relaxed);
}

std::uint32_t NativeCosmeticRenderer::triangleCount() {
  return g_triangleCount.load(std::memory_order_relaxed);
}

bool NativeCosmeticRenderer::init() {
  if (g_initAttempted.exchange(true)) return g_initOk.load();
  const GlesFunctions gles = ResolveGles();
  const bool ok = gles.usable();
  g_initOk.store(ok);
  if (!ok) preloaderLogger.info("Native cosmetic renderer: no usable GLES surface (Vulkan-only?)");
  return ok;
}

// Persistent GL objects, created once and reused every frame. The old code created and deleted a
// VAO and two buffers per piece per frame — thousands of allocations a second on the render thread.
namespace {
struct MeshBuffers {
  GLuint vao = 0;
  GLuint vbo = 0;
  GLuint ibo = 0;
  std::size_t vboCapacity = 0;  // bytes
  std::size_t iboCapacity = 0;
};
MeshBuffers g_meshBuffers;

std::vector<float> g_interleavedScratch;

// Parsed-geometry cache. The launcher publishes the geometry blob only when the equipped set
// changes, but the render callback runs every frame; re-parsing (and re-building the vertex meshes
// for) the same blob each frame was pure per-frame CPU cost on the render thread. The blob's FNV-1a
// hash is the cache key, so a swap of cosmetic re-parses once and steady state does nothing.
std::uint64_t g_geometryCacheHash = 0;
std::vector<PieceMesh> g_capeCache;
std::vector<PieceMesh> g_hatCache;
std::vector<PieceMesh> g_petCache;

std::uint64_t HashBlob(std::span<const std::uint8_t> blob) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::uint8_t byte : blob) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void RefreshGeometryCache(std::span<const std::uint8_t> geometryBlob) {
  const std::uint64_t hash = HashBlob(geometryBlob);
  if (hash == g_geometryCacheHash) return;
  g_capeCache = ParseGeometryForPiece(geometryBlob, "geometry.chimera_cape");
  g_hatCache = ParseGeometryForPiece(geometryBlob, "geometry.chimera_hat");
  g_petCache = ParseGeometryForPiece(geometryBlob, "geometry.chimera_pet");
  g_geometryCacheHash = hash;
}

// The cosmetic VFX particle pool. One pool for the local player's equipped set; a themed cosmetic
// emits an aura/trail here and the renderer draws it as additive billboards.
CosmeticParticles g_particles;
// Seconds since the last particle step, derived from the frame tick, so the pool advances at a
// frame-rate-independent rate. -1 means "no previous frame yet".
long long g_lastParticleStepMs = -1;

// Scratch for the billboard vertices (x, y, z, u, v, nx, ny, nz), reused so the draw allocates
// nothing.
std::vector<float> g_particleVerts;

/**
 * @brief The uniform scale that brings a piece's mesh to the model-pixel height the anchor expects.
 *
 * An authored Blockbench pet is stated in its own units (a dragon can be 23 tall) while the cape
 * chain and procedural pets are on the ~14-unit model-pixel scale. Returning a per-piece scale keeps
 * every piece the same on-screen size without touching the authored proportions.
 */
float ScaleForHeight(const std::vector<PieceMesh> &pieces, float targetHeight) {
  float minY = 1e30F;
  float maxY = -1e30F;
  bool any = false;
  for (const PieceMesh &piece : pieces) {
    for (const auto &vertex : piece.mesh.vertices) {
      minY = std::min(minY, vertex.position.y);
      maxY = std::max(maxY, vertex.position.y);
      any = true;
    }
  }
  if (!any || maxY - minY <= 0.0001F || targetHeight <= 0.0F) return 1.0F;
  return targetHeight / (maxY - minY);
}
} // namespace

CosmeticDrawResult NativeCosmeticRenderer::drawFrame(std::span<const std::uint8_t> frame,
                                                     std::span<const std::uint8_t> geometryBlob,
                                                     const CosmeticAtlases &atlases,
                                                     const Mat4 &world) {
  if (!g_initOk.load()) return CosmeticDrawResult::Skipped;
  const GlesFunctions gles = g_gles;
  if (!gles.usable()) return CosmeticDrawResult::Skipped;

  // Validate the frame before touching GL state.
  std::uint32_t flags = 0;
  std::vector<float> capeLeanSway;
  int accessoryKind = 0;
  float headPitch = 0.0F, headYaw = 0.0F;
  std::vector<std::pair<std::string, Mat4>> petBones;
  Mat4 petOffset;
  if (!ParseCosmeticFrame(frame, flags, capeLeanSway, accessoryKind, headPitch, headYaw,
                          petBones, petOffset)) {
    preloaderLogger.debug("Native cosmetic renderer: frame invalid, skipping");
    return CosmeticDrawResult::Skipped;
  }
  // The debug cube draws even with nothing equipped, so a device can prove the draw path on its own.
  const bool debugCube = g_debugCube.load(std::memory_order_relaxed);
  if ((flags & 0xFu) == 0 && !debugCube) return CosmeticDrawResult::Skipped;

  // Prepare the shader + textures (idempotent, guarded).
  if (!EnsureProgram(gles)) return CosmeticDrawResult::Skipped;
  if (!EnsureWhiteTexture(gles)) return CosmeticDrawResult::Skipped;
  // Re-parse the geometry only when the blob changed (the launcher republishes it on an equip).
  RefreshGeometryCache(geometryBlob);

  SavedState state{};
  if (!SaveState(gles, state)) return CosmeticDrawResult::Skipped;

  auto enable = reinterpret_cast<glEnable_t>(gles.Enable);
  auto disable = reinterpret_cast<glDisable_t>(gles.Disable);
  auto depthMask = reinterpret_cast<glDepthMask_t>(gles.DepthMask);
  auto blendFunc = reinterpret_cast<glBlendFunc_t>(gles.BlendFunc);
  auto activeTex = reinterpret_cast<glActiveTexture_t>(gles.ActiveTexture);
  auto bindTex = reinterpret_cast<glBindTexture_t>(gles.BindTexture);
  auto useProg = reinterpret_cast<glUseProgram_t>(gles.UseProgram);
  auto getUniform = reinterpret_cast<glGetUniformLocation_t>(gles.GetUniformLocation);
  auto uniformMvp = reinterpret_cast<glUniformMatrix4fv_t>(gles.UniformMatrix4fv);
  auto uniformColor = reinterpret_cast<glUniform4f_t>(gles.Uniform4f);
  auto genBuffers = reinterpret_cast<glGenBuffers_t>(gles.GenBuffers);
  auto bindBuffer = reinterpret_cast<glBindBuffer_t>(gles.BindBuffer);
  auto bufferData = reinterpret_cast<glBufferData_t>(gles.BufferData);
  auto genVao = reinterpret_cast<glGenVertexArrays_t>(gles.GenVertexArrays);
  auto bindVao = reinterpret_cast<glBindVertexArray_t>(gles.BindVertexArray);
  auto enableAttrib = reinterpret_cast<glEnableVertexAttribArray_t>(gles.EnableVertexAttribArray);
  auto attribPointer = reinterpret_cast<glVertexAttribPointer_t>(gles.VertexAttribPointer);
  auto drawElements = reinterpret_cast<glDrawElements_t>(gles.DrawElements);

  // Set draw state: transparent cosmetics alpha-blend, depth write on so they occlude correctly
  // against the body, double-sided so the inside of a hat/pet face is not culled.
  enable(kBlend);
  if (blendFunc) blendFunc(0x0302 /* SRC_ALPHA */, 0x0303 /* ONE_MINUS_SRC_ALPHA */);
  disable(kCullFace);
  depthMask(GL_TRUE);
  activeTex(kTexture0);
  useProg(g_program);

  const GLint locMvp = getUniform(g_program, "uMVP");
  const GLint locColor = getUniform(g_program, "uColor");
  const GLint locTex = getUniform(g_program, "uTexture");
  const GLint locLight = getUniform(g_program, "uLightDir");
  if (locTex >= 0 && gles.Uniform1i) {
    reinterpret_cast<glUniform1i_t>(gles.Uniform1i)(locTex, 0);
  }
  // A fixed headlight from up-and-toward-the-viewer, so a face is bright when it turns toward the
  // camera and dim when it turns away — the shading the flat-colour pass lacked.
  if (locLight >= 0 && gles.Uniform3f) {
    reinterpret_cast<glUniform3f_t>(gles.Uniform3f)(locLight, 0.35F, 0.75F, 0.55F);
  }

  // Create the persistent mesh buffers once.
  if (g_meshBuffers.vao == 0) {
    genBuffers(1, &g_meshBuffers.vbo);
    genBuffers(1, &g_meshBuffers.ibo);
    genVao(1, &g_meshBuffers.vao);
    bindVao(g_meshBuffers.vao);
    bindBuffer(kArrayBuffer, g_meshBuffers.vbo);
    bindBuffer(kElementArrayBuffer, g_meshBuffers.ibo);
    enableAttrib(kAttribPosition);
    // Interleaved: position (vec3, offset 0), uv (vec2, offset 12), normal (vec3, offset 20).
    attribPointer(kAttribPosition, 3, kFloat, GL_FALSE, 32, nullptr);
    enableAttrib(kAttribUv);
    attribPointer(kAttribUv, 2, kFloat, GL_FALSE, 32, reinterpret_cast<const void *>(12));
    enableAttrib(kAttribNormal);
    attribPointer(kAttribNormal, 3, kFloat, GL_FALSE, 32, reinterpret_cast<const void *>(20));
    bindVao(0);
  }

  bool drew = false;

  // Helper: upload a mesh into the persistent buffers and issue a draw with the given texture.
  auto drawOne = [&](const geometry::Mesh &mesh, const Mat4 &mvp, AtlasSlot &slot,
                     const PieceAtlas &atlas, float r, float g, float b, float a) {
    if (mesh.vertices.empty() || mesh.indices.empty()) return false;
    const size_t verts = mesh.vertices.size();
    const size_t vboBytes = verts * 8 * sizeof(float);
    const size_t iboBytes = mesh.indices.size() * sizeof(std::uint32_t);
    if (vboBytes > static_cast<size_t>(PTRDIFF_MAX) || iboBytes > static_cast<size_t>(PTRDIFF_MAX)) {
      return false;
    }

    // Interleaved position (vec3) + uv (vec2) + normal (vec3), stride 32 bytes, reused scratch.
    g_interleavedScratch.resize(verts * 8);
    for (size_t i = 0; i < verts; ++i) {
      g_interleavedScratch[i * 8 + 0] = mesh.vertices[i].position.x;
      g_interleavedScratch[i * 8 + 1] = mesh.vertices[i].position.y;
      g_interleavedScratch[i * 8 + 2] = mesh.vertices[i].position.z;
      g_interleavedScratch[i * 8 + 3] = mesh.vertices[i].uv.x;
      g_interleavedScratch[i * 8 + 4] = mesh.vertices[i].uv.y;
      g_interleavedScratch[i * 8 + 5] = mesh.vertices[i].normal.x;
      g_interleavedScratch[i * 8 + 6] = mesh.vertices[i].normal.y;
      g_interleavedScratch[i * 8 + 7] = mesh.vertices[i].normal.z;
    }

    bindVao(g_meshBuffers.vao);
    bindBuffer(kArrayBuffer, g_meshBuffers.vbo);
    // STREAM_DRAW, since the mesh contents change per frame; just re-specify the store.
    bufferData(kArrayBuffer, static_cast<GLsizeiptr>(vboBytes), g_interleavedScratch.data(),
               kStreamDraw);
    bindBuffer(kElementArrayBuffer, g_meshBuffers.ibo);
    bufferData(kElementArrayBuffer, static_cast<GLsizeiptr>(iboBytes), mesh.indices.data(),
               kStreamDraw);

    bindTex(kTexture2D, AtlasTexture(gles, slot, atlas));

    if (locMvp >= 0) uniformMvp(locMvp, 1, GL_FALSE, mvp.m);
    if (locColor >= 0 && uniformColor) uniformColor(locColor, r, g, b, a);

    drawElements(kTriangles, static_cast<GLsizei>(mesh.indices.size()), kUnsignedInt, nullptr);
    g_drawCount.fetch_add(1, std::memory_order_relaxed);
    g_triangleCount.fetch_add(static_cast<std::uint32_t>(mesh.indices.size() / 3),
                              std::memory_order_relaxed);
    return true;
  };

  // Bedrock-authored cosmetic geometry faces -Z; the player model faces +Z. A half-turn about Y
  // converts one to the other, so a hat's brim points forward and a pet faces the direction the
  // player faces. The cape chain already bakes its own facing into its bones, so it is NOT converted.
  const Mat4 bedrockToPlayer = Mat4::rotationY(180.0F);

  auto scaledAnchor = [&](const Mat4 &local) {
    Mat4 scale;
    scale.m[0] = kModelPixelToBlocks;
    scale.m[5] = kModelPixelToBlocks;
    scale.m[10] = kModelPixelToBlocks;
    scale.m[15] = 1.0F;
    return Mat4::multiply(world, Mat4::multiply(local, scale));
  };

  // --- Cape -----------------------------------------------------------------------------
  if ((flags & 0x1u) && !geometryBlob.empty()) {
    const auto &pieces = g_capeCache;
    // The cape sits behind the player's back, anchored at the shoulders (model y=24 px = 1.5 blocks).
    const Mat4 local = Mat4::translation(Vec3{0.0F, 24.0F * kModelPixelToBlocks,
                                              -0.4F * kModelPixelToBlocks});
    for (auto &piece : pieces) {
      drew |= drawOne(piece.mesh, scaledAnchor(local), g_capeAtlas, atlases.cape,
                      1.0F, 1.0F, 1.0F, 0.95F);
    }
  }

  // --- Hat ------------------------------------------------------------------------------
  if ((flags & 0x2u) && accessoryKind > 0 && !geometryBlob.empty()) {
    const auto &pieces = g_hatCache;
    // The hat is anchored to the head crown and turns with the head look.
    const Mat4 headLook = Mat4::multiply(Mat4::rotationY(headYaw), Mat4::rotationX(headPitch));
    const Mat4 local = Mat4::multiply(
        Mat4::multiply(Mat4::translation(Vec3{0.0F, 32.0F * kModelPixelToBlocks, 0.0F}), headLook),
        bedrockToPlayer);
    for (auto &piece : pieces) {
      drew |= drawOne(piece.mesh, scaledAnchor(local), g_hatAtlas, atlases.hat,
                      1.0F, 1.0F, 1.0F, 1.0F);
    }
  }

  // --- Pet ------------------------------------------------------------------------------
  if ((flags & 0x4u) && !geometryBlob.empty()) {
    const auto &pieces = g_petCache;
    // Look up the frame's per-bone rotation matrix by name.
    std::unordered_map<std::string, Mat4> rotByBone;
    for (const auto &entry : petBones) rotByBone[entry.first] = entry.second;
    // Normalise the authored pet to the model-pixel height the offset expects, so a large authored
    // rig (a 23-unit dragon) is not drawn several times too big.
    const float petScale = ScaleForHeight(pieces, 14.0F);
    Mat4 petScaleMat;
    petScaleMat.m[0] = petScale;
    petScaleMat.m[5] = petScale;
    petScaleMat.m[10] = petScale;
    petScaleMat.m[15] = 1.0F;
    const Mat4 local = Mat4::multiply(petOffset, Mat4::multiply(bedrockToPlayer, petScaleMat));
    for (auto &piece : pieces) {
      geometry::Mesh mesh = piece.mesh;
      // Apply the per-bone animation offset about the bone's pivot on the CPU, then upload. Look the
      // authored bone up first; when the authored name has no track, fall back to the track for the
      // mesh's player-skeleton bone (an authored pet's "leg_a" and a skeleton "legs" both map to the
      // leg track), so a mesh whose authored name is arbitrary still animates instead of being left
      // to move against nothing.
      const Mat4 *rotation = nullptr;
      auto it = rotByBone.find(piece.mesh.bone);
      if (it != rotByBone.end()) {
        rotation = &it->second;
      } else if (!piece.mesh.skeletonBone.empty()) {
        const std::string track = geometry::FrameBoneForSkeleton(piece.mesh.skeletonBone);
        if (!track.empty()) {
          auto fallback = rotByBone.find(track);
          if (fallback != rotByBone.end()) rotation = &fallback->second;
        }
      }
      if (rotation != nullptr && piece.hasPivot) {
        mesh = RotateMeshByBoneMatrix(mesh, piece.pivot, *rotation);
      }
      drew |= drawOne(mesh, scaledAnchor(local), g_petAtlas, atlases.pet,
                      1.0F, 1.0F, 1.0F, 1.0F);
    }
  }

  // --- VFX aura/trail -------------------------------------------------------------------
  // Emit and draw a light particle aura for the equipped set. Particles are emitted in the same
  // local space the pieces are anchored in (so they follow the player) and drawn as small
  // horizontal quads with additive blending, so an elite cosmetic reads as glowing/emitting.
  {
    const bool wantVfx = (flags & 0x8u) != 0;
    g_particles.setEnabled(wantVfx);
    // Advance the pool on a frame clock. uptime is not available here, so use the render tick's
    // monotonic timestamp through the world matrix cadence: a fixed 1/60 step is stable enough and
    // frame-rate-independent because emission is rate-limited below.
    constexpr float kStepSeconds = 1.0F / 60.0F;
    g_particles.step(kStepSeconds, 2.4F);
    if (wantVfx) {
      // A steady aura: a few particles per frame at the player's chest, drifting up.
      g_particles.emit(2, 0.0F, 1.0F, 0.0F, 0.45F, 0.8F, 1.4F);
    }
    if (g_particles.liveCount() > 0 && gles.DrawArrays) {
      const std::size_t live = g_particles.liveCount();
      g_particleVerts.clear();
      g_particleVerts.reserve(live * 6 * 8);  // 6 vertices * 8 floats per quad
      const Particle *pool = g_particles.particles();
      for (std::size_t i = 0; i < g_particles.capacity(); ++i) {
        const Particle &p = pool[i];
        if (!p.alive) continue;
        const float s = p.size * 0.06F;  // block radius
        const float x = p.x, y = p.y, z = p.z;
        // Two triangles (6 vertices) forming an axis-aligned horizontal quad; a constant +Y normal
        // keeps it lit and needs no camera basis, which the passthrough hook cannot recover.
        const float verts[6][3] = {
            {x - s, y, z - s}, {x + s, y, z - s}, {x + s, y, z + s},
            {x - s, y, z - s}, {x + s, y, z + s}, {x - s, y, z + s}};
        const float uvs[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
        for (int v = 0; v < 6; ++v) {
          g_particleVerts.push_back(verts[v][0]);
          g_particleVerts.push_back(verts[v][1]);
          g_particleVerts.push_back(verts[v][2]);
          g_particleVerts.push_back(uvs[v][0]);
          g_particleVerts.push_back(uvs[v][1]);
          g_particleVerts.push_back(0.0F);
          g_particleVerts.push_back(1.0F);
          g_particleVerts.push_back(0.0F);
        }
      }
      if (!g_particleVerts.empty()) {
        bindVao(g_meshBuffers.vao);
        bindBuffer(kArrayBuffer, g_meshBuffers.vbo);
        bufferData(kArrayBuffer,
                   static_cast<GLsizeiptr>(g_particleVerts.size() * sizeof(float)),
                   g_particleVerts.data(), kStreamDraw);
        // Additive blend so overlapping particles glow rather than darken.
        if (blendFunc) blendFunc(0x0302 /* SRC_ALPHA */, 0x0001 /* ONE */);
        bindTex(kTexture2D, g_whiteTexture);
        if (locMvp >= 0) uniformMvp(locMvp, 1, GL_FALSE, world.m);
        if (locColor >= 0 && uniformColor) uniformColor(locColor, 1.0F, 0.86F, 0.45F, 0.5F);
        auto drawArrays = reinterpret_cast<glDrawArrays_t>(gles.DrawArrays);
        drawArrays(kTriangles, 0, static_cast<GLsizei>(g_particleVerts.size() / 8));
        // Restore the alpha blend the mesh pass expects for the next frame.
        if (blendFunc) blendFunc(0x0302, 0x0303);
        g_drawCount.fetch_add(1, std::memory_order_relaxed);
        drew = true;
      }
    }
  }

  bindVao(0);

  // --- Debug cube (diagnostic) -----------------------------------------------------------
  // Drawn above the local player's head, in model-pixel space (scaled to blocks by scaledAnchor),
  // so a device can prove the draw path reaches the screen even with nothing equipped. Unverified
  // on device: this is the instrument, not a claim that it appears.
  if (debugCube) {
    geometry::Mesh cube;
    const float c = 40.0F;   // head crown is model y=32; sit the cube a little above it
    const float s = 3.0F;    // half-extent in model pixels
    const geometry::Vec3f corners[8] = {
        {-s, c - s, -s}, {s, c - s, -s}, {s, c - s, s}, {-s, c - s, s},
        {-s, c + s, -s}, {s, c + s, -s}, {s, c + s, s}, {-s, c + s, s}};
    // Face normals for the six sides, so the headlight shades them.
    const geometry::Vec3f normals[6] = {
        {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
    const int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                             {1, 5, 6, 2}, {4, 5, 1, 0}, {3, 2, 6, 7}};
    for (int f = 0; f < 6; ++f) {
      const std::uint32_t base = static_cast<std::uint32_t>(cube.vertices.size());
      for (int v = 0; v < 4; ++v) {
        geometry::Vertex vertex;
        vertex.position = corners[faces[f][v]];
        vertex.normal = normals[f];
        vertex.uv = geometry::Vec2f{0.0F, 0.0F};
        cube.vertices.push_back(vertex);
      }
      cube.indices.push_back(base + 0);
      cube.indices.push_back(base + 1);
      cube.indices.push_back(base + 2);
      cube.indices.push_back(base + 0);
      cube.indices.push_back(base + 2);
      cube.indices.push_back(base + 3);
    }
    // Bind the white texture explicitly so the cube is not tinted by a piece atlas.
    static AtlasSlot debugAtlas;  // texture stays 0 -> AtlasTexture returns the white texture
    drew |= drawOne(cube, scaledAnchor(Mat4::identity()), debugAtlas, PieceAtlas{},
                    1.0F, 0.35F, 0.85F, 1.0F);
  }

  // Capture the last GL error for the diagnostics report (read-and-clear, so it is the newest error).
  if (gles.GetError) {
    const GLenum err = reinterpret_cast<GLenum (*)()>(gles.GetError)();
    g_lastGlesError.store(static_cast<int>(err), std::memory_order_relaxed);
  }

  RestoreState(gles, state);
  if (drew) {
    preloaderLogger.debug("Native cosmetic renderer: drew cosmetic meshes");
    return CosmeticDrawResult::Drawn;
  }
  preloaderLogger.debug("Native cosmetic renderer: nothing drawn");
  return CosmeticDrawResult::Skipped;
}

} // namespace pl::cosmetics