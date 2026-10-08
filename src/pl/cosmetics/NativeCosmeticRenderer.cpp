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

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>

#include "pl/Logger.hpp"
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
  LOAD("glUniform1i", Uniform1i);
  LOAD("glActiveTexture", ActiveTexture);
  LOAD("glBindTexture", BindTexture);
  LOAD("glGenTextures", GenTextures);
  LOAD("glTexParameteri", TexParameteri);
  LOAD("glTexImage2D", TexImage2D);
  LOAD("glViewport", Viewport);
  LOAD("glDrawElements", DrawElements);
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
using glUniform1i_t = void (*)(GLint, GLint);
using glActiveTexture_t = void (*)(GLenum);
using glBindTexture_t = void (*)(GLenum, GLuint);
using glGenTextures_t = void (*)(GLsizei, GLuint *);
using glTexParameteri_t = void (*)(GLenum, GLenum, GLint);
using glTexImage2D_t = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                const void *);
using glDrawElements_t = void (*)(GLenum, GLsizei, GLenum, const void *);
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

struct SavedState {
  GLint program = 0;
  GLint texture2d = 0;
  GLint vao = 0;
  GLint arrayBuf = 0;
  GLint elemBuf = 0;
  GLboolean blend = GL_FALSE;
  GLboolean depthMask = GL_TRUE;
  GLboolean cull = GL_FALSE;
};

bool SaveState(const GlesFunctions &gles, SavedState &state) {
  if (!gles.GetIntegerv || !gles.GetBooleanv) return false;
  auto geti = reinterpret_cast<glGetIntegerv_t>(gles.GetIntegerv);
  auto getb = reinterpret_cast<glGetBooleanv_t>(gles.GetBooleanv);
  geti(0x8B31 /* GL_CURRENT_PROGRAM */, &state.program);
  geti(0x0BDE /* GL_TEXTURE_BINDING_2D */, &state.texture2d);
  geti(0x8CA6 /* GL_VERTEX_ARRAY_BINDING */, &state.vao);
  geti(0x8892 /* GL_ARRAY_BUFFER_BINDING */, &state.arrayBuf);
  geti(0x8893 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &state.elemBuf);
  getb(0x0BE2 /* GL_BLEND */, &state.blend);
  getb(0x0B24 /* GL_DEPTH_WRITEMASK */, &state.depthMask);
  getb(0x0B44 /* GL_CULL_FACE */, &state.cull);
  return true;
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
  bindTex(kTexture2D, static_cast<GLuint>(state.texture2d));
  if (state.blend) enable(kBlend); else disable(kBlend);
  if (state.cull) enable(kCullFace); else disable(kCullFace);
  depthMask(state.depthMask);
}

// A single program: samples a texture multiplied by uColor. When the cosmetic atlas is unavailable
// a 1x1 white texture is bound, so the same program serves the flat-colour fallback.
constexpr const char *kVS =
    "attribute vec4 aPosition;\n"
    "attribute vec2 aUv;\n"
    "varying vec2 vUv;\n"
    "uniform mat4 uMVP;\n"
    "void main(){ vUv = aUv; gl_Position = uMVP * aPosition; }\n";
constexpr const char *kFS =
    "precision mediump float;\n"
    "varying vec2 vUv;\n"
    "uniform sampler2D uTexture;\n"
    "uniform vec4 uColor;\n"
    "void main(){ gl_FragColor = texture2D(uTexture, vUv) * uColor; }\n";

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

// Uploads the cosmetic atlas once per image dimensions, or reuses the last uploaded one.
GLuint g_atlasTexture = 0;
int g_atlasW = 0;
int g_atlasH = 0;

void EnsureAtlas(const GlesFunctions &gles, const std::uint8_t *rgba, int width, int height) {
  if (!rgba || width <= 0 || height <= 0) return;
  if (g_atlasTexture != 0 && g_atlasW == width && g_atlasH == height) return;
  if (!gles.GenTextures || !gles.BindTexture || !gles.TexParameteri || !gles.TexImage2D) return;
  auto gen = reinterpret_cast<glGenTextures_t>(gles.GenTextures);
  auto bind = reinterpret_cast<glBindTexture_t>(gles.BindTexture);
  auto param = reinterpret_cast<glTexParameteri_t>(gles.TexParameteri);
  auto image = reinterpret_cast<glTexImage2D_t>(gles.TexImage2D);
  if (g_atlasTexture == 0) gen(1, &g_atlasTexture);
  bind(kTexture2D, g_atlasTexture);
  param(kTexture2D, 0x2802, kLinear);
  param(kTexture2D, 0x2803, kLinear);
  param(kTexture2D, 0x2800, kClampToEdge);
  param(kTexture2D, 0x2801, kClampToEdge);
  image(kTexture2D, 0, kRgba, width, height, 0, kRgba, kUnsignedByte, rgba);
  g_atlasW = width;
  g_atlasH = height;
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

bool NativeCosmeticRenderer::init() {
  if (g_initAttempted.exchange(true)) return g_initOk.load();
  const GlesFunctions gles = ResolveGles();
  const bool ok = gles.usable();
  g_initOk.store(ok);
  if (!ok) preloaderLogger.info("Native cosmetic renderer: no usable GLES surface (Vulkan-only?)");
  return ok;
}

CosmeticDrawResult NativeCosmeticRenderer::drawFrame(std::span<const std::uint8_t> frame,
                                                     std::span<const std::uint8_t> geometryBlob,
                                                     const std::uint8_t *atlasPixels,
                                                     int atlasWidth, int atlasHeight,
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
  if ((flags & 0x7u) == 0) return CosmeticDrawResult::Skipped;

  // Prepare the shader + textures (idempotent, guarded).
  if (!EnsureProgram(gles)) return CosmeticDrawResult::Skipped;
  if (!EnsureWhiteTexture(gles)) return CosmeticDrawResult::Skipped;
  EnsureAtlas(gles, atlasPixels, atlasWidth, atlasHeight);

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
  auto deleteBuffers = reinterpret_cast<glDeleteBuffers_t>(gles.DeleteBuffers);
  auto deleteVao = reinterpret_cast<glDeleteVertexArrays_t>(gles.DeleteVertexArrays);

  // Set draw state: transparent cosmetics alpha-blend, depth write on so they occlude correctly
  // against the body, double-sided so the inside of a hat/pet face is not culled.
  enable(kBlend);
  if (blendFunc) blendFunc(0x0302 /* SRC_ALPHA */, 0x0303 /* ONE_MINUS_SRC_ALPHA */);
  disable(kCullFace);
  depthMask(GL_TRUE);
  activeTex(kTexture0);
  useProg(g_program);

  auto bindAtlas = reinterpret_cast<glBindTexture_t>(gles.BindTexture);
  const GLuint texture = g_atlasTexture != 0 ? g_atlasTexture : g_whiteTexture;
  bindAtlas(kTexture2D, texture);
  const GLint locMvp = getUniform(g_program, "uMVP");
  const GLint locColor = getUniform(g_program, "uColor");
  const GLint locTex = getUniform(g_program, "uTexture");

  bool drew = false;

  // Helper: upload a mesh and issue a draw.
  auto drawOne = [&](const geometry::Mesh &mesh, const Mat4 &mvp, float r, float g, float b, float a) {
    if (mesh.vertices.empty() || mesh.indices.empty()) return false;
    const size_t verts = mesh.vertices.size();

    // Interleaved position (vec3) + uv (vec2), stride 20 bytes.
    std::vector<float> interleaved(verts * 5);
    for (size_t i = 0; i < verts; ++i) {
      interleaved[i * 5 + 0] = mesh.vertices[i].position.x;
      interleaved[i * 5 + 1] = mesh.vertices[i].position.y;
      interleaved[i * 5 + 2] = mesh.vertices[i].position.z;
      interleaved[i * 5 + 3] = mesh.vertices[i].uv.x;
      interleaved[i * 5 + 4] = mesh.vertices[i].uv.y;
    }

    GLuint vbo = 0, ibo = 0, vao = 0;
    genBuffers(1, &vbo);
    bindBuffer(kArrayBuffer, vbo);
    bufferData(kArrayBuffer, static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
               interleaved.data(), kStreamDraw);
    genBuffers(1, &ibo);
    bindBuffer(kElementArrayBuffer, ibo);
    bufferData(kElementArrayBuffer,
               static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
               mesh.indices.data(), kStreamDraw);
    genVao(1, &vao);
    bindVao(vao);
    bindBuffer(kArrayBuffer, vbo);
    bindBuffer(kElementArrayBuffer, ibo);
    enableAttrib(0);
    attribPointer(0, 3, kFloat, GL_FALSE, 20, nullptr);
    enableAttrib(1);
    attribPointer(1, 2, kFloat, GL_FALSE, 20,
                  reinterpret_cast<const void *>(12));

    if (locMvp >= 0) uniformMvp(locMvp, 1, GL_FALSE, mvp.m);
    if (locColor >= 0 && uniformColor) uniformColor(locColor, r, g, b, a);
    if (locTex >= 0 && gles.Uniform1i) {
      reinterpret_cast<glUniform1i_t>(gles.Uniform1i)(locTex, 0);
    }

    drawElements(kTriangles, static_cast<GLsizei>(mesh.indices.size()), kUnsignedInt, nullptr);

    bindVao(0);
    deleteVao(1, &vao);
    deleteBuffers(1, &ibo);
    deleteBuffers(1, &vbo);
    return true;
  };

  // --- Cape -----------------------------------------------------------------------------
  if ((flags & 0x1u) && !geometryBlob.empty()) {
    auto pieces = ParseGeometryForPiece(geometryBlob, "geometry.chimera_cape");
    for (auto &piece : pieces) {
      // The cape sits behind the player's back, anchored at the shoulders in model units.
      Mat4 capeMvp = Mat4::multiply(world, Mat4::translation(Vec3{0, 24.0F, -0.4F}));
      drew |= drawOne(piece.mesh, capeMvp, 1.0F, 1.0F, 1.0F, 0.95F);
    }
  }

  // --- Hat ------------------------------------------------------------------------------
  if ((flags & 0x2u) && accessoryKind > 0 && !geometryBlob.empty()) {
    auto pieces = ParseGeometryForPiece(geometryBlob, "geometry.chimera_hat");
    for (auto &piece : pieces) {
      // The hat is anchored to the head crown and turns with the head look.
      Mat4 headLook = Mat4::multiply(Mat4::rotationY(headYaw), Mat4::rotationX(headPitch));
      Mat4 hatMvp = Mat4::multiply(world,
                                   Mat4::multiply(Mat4::translation(Vec3{0, 32.0F, 0}), headLook));
      drew |= drawOne(piece.mesh, hatMvp, 1.0F, 1.0F, 1.0F, 1.0F);
    }
  }

  // --- Pet ------------------------------------------------------------------------------
  if ((flags & 0x4u) && !geometryBlob.empty()) {
    auto pieces = ParseGeometryForPiece(geometryBlob, "geometry.chimera_pet");
    // Look up the frame's per-bone rotation matrix by name.
    std::unordered_map<std::string, Mat4> rotByBone;
    for (const auto &entry : petBones) rotByBone[entry.first] = entry.second;
    for (auto &piece : pieces) {
      geometry::Mesh mesh = piece.mesh;
      // Apply the per-bone animation offset about the bone's pivot on the CPU, then upload.
      auto it = rotByBone.find(piece.mesh.bone);
      if (it != rotByBone.end() && piece.hasPivot) {
        mesh = RotateMeshByBoneMatrix(mesh, piece.pivot, it->second);
      }
      Mat4 petMvp = Mat4::multiply(world, petOffset);
      drew |= drawOne(mesh, petMvp, 1.0F, 1.0F, 1.0F, 1.0F);
    }
  }

  RestoreState(gles, state);
  preloaderLogger.debug("Native cosmetic renderer: {}", drew ? "drew cosmetics" : "nothing drawn");
  return drew ? CosmeticDrawResult::Drawn : CosmeticDrawResult::Skipped;
}

} // namespace pl::cosmetics