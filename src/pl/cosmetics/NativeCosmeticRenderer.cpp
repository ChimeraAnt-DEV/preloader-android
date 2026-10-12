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

// One Bedrock model pixel is 1/16 block. Without this the geometry is drawn in raw model pixels —
// 16x too large. Applied to every piece's anchor so the meshes sit at the player's real scale.
constexpr float kModelPixelToBlocks = 1.0F / 16.0F;

std::uint32_t NativeCosmeticRenderer::drawCount() {
  return g_drawCount.load(std::memory_order_relaxed);
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
  if ((flags & 0x7u) == 0) return CosmeticDrawResult::Skipped;

  // Prepare the shader + textures (idempotent, guarded).
  if (!EnsureProgram(gles)) return CosmeticDrawResult::Skipped;
  if (!EnsureWhiteTexture(gles)) return CosmeticDrawResult::Skipped;

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
  if (locTex >= 0 && gles.Uniform1i) {
    reinterpret_cast<glUniform1i_t>(gles.Uniform1i)(locTex, 0);
  }

  // Create the persistent mesh buffers once.
  if (g_meshBuffers.vao == 0) {
    genBuffers(1, &g_meshBuffers.vbo);
    genBuffers(1, &g_meshBuffers.ibo);
    genVao(1, &g_meshBuffers.vao);
    bindVao(g_meshBuffers.vao);
    bindBuffer(kArrayBuffer, g_meshBuffers.vbo);
    bindBuffer(kElementArrayBuffer, g_meshBuffers.ibo);
    enableAttrib(0);
    attribPointer(0, 3, kFloat, GL_FALSE, 20, nullptr);
    enableAttrib(1);
    attribPointer(1, 2, kFloat, GL_FALSE, 20, reinterpret_cast<const void *>(12));
    bindVao(0);
  }

  bool drew = false;

  // Helper: upload a mesh into the persistent buffers and issue a draw with the given texture.
  auto drawOne = [&](const geometry::Mesh &mesh, const Mat4 &mvp, AtlasSlot &slot,
                     const PieceAtlas &atlas, float r, float g, float b, float a) {
    if (mesh.vertices.empty() || mesh.indices.empty()) return false;
    const size_t verts = mesh.vertices.size();
    const size_t vboBytes = verts * 5 * sizeof(float);
    const size_t iboBytes = mesh.indices.size() * sizeof(std::uint32_t);
    if (vboBytes > static_cast<size_t>(PTRDIFF_MAX) || iboBytes > static_cast<size_t>(PTRDIFF_MAX)) {
      return false;
    }

    // Interleaved position (vec3) + uv (vec2), stride 20 bytes, into the reused scratch.
    g_interleavedScratch.resize(verts * 5);
    for (size_t i = 0; i < verts; ++i) {
      g_interleavedScratch[i * 5 + 0] = mesh.vertices[i].position.x;
      g_interleavedScratch[i * 5 + 1] = mesh.vertices[i].position.y;
      g_interleavedScratch[i * 5 + 2] = mesh.vertices[i].position.z;
      g_interleavedScratch[i * 5 + 3] = mesh.vertices[i].uv.x;
      g_interleavedScratch[i * 5 + 4] = mesh.vertices[i].uv.y;
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
    auto pieces = ParseGeometryForPiece(geometryBlob, "geometry.chimera_cape");
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
    auto pieces = ParseGeometryForPiece(geometryBlob, "geometry.chimera_hat");
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
    auto pieces = ParseGeometryForPiece(geometryBlob, "geometry.chimera_pet");
    // Look up the frame's per-bone rotation matrix by name.
    std::unordered_map<std::string, Mat4> rotByBone;
    for (const auto &entry : petBones) rotByBone[entry.first] = entry.second;
    const Mat4 local = Mat4::multiply(petOffset, bedrockToPlayer);
    for (auto &piece : pieces) {
      geometry::Mesh mesh = piece.mesh;
      // Apply the per-bone animation offset about the bone's pivot on the CPU, then upload.
      auto it = rotByBone.find(piece.mesh.bone);
      if (it != rotByBone.end() && piece.hasPivot) {
        mesh = RotateMeshByBoneMatrix(mesh, piece.pivot, it->second);
      }
      drew |= drawOne(mesh, scaledAnchor(local), g_petAtlas, atlases.pet,
                      1.0F, 1.0F, 1.0F, 1.0F);
    }
  }

  bindVao(0);
  RestoreState(gles, state);
  if (drew) {
    preloaderLogger.debug("Native cosmetic renderer: drew cosmetic meshes");
    return CosmeticDrawResult::Drawn;
  }
  preloaderLogger.debug("Native cosmetic renderer: nothing drawn");
  return CosmeticDrawResult::Skipped;
}

} // namespace pl::cosmetics