#pragma once

/**
 * @file mce_image_hook.hpp
 * @brief Engine-backed image construction and the `mce::Image` pipeline hook.
 *
 * This is the native half that replaces the "synthesise a raw pixel buffer and hope the engine
 * accepts it" approach with the engine's own image pipeline. Bedrock's `mce::Image` is not a plain
 * pixel array: it is an opaque 0x30-byte handle to a buffer the engine owns, and RenderDragon frees
 * that buffer through its own allocator when a skin is unloaded. Handing the engine a pointer into
 * a launcher-side `malloc` therefore faults (free of a foreign pointer) or double-frees, and
 * hand-rolling the struct's internal fields produces the black-box texture because the buffer
 * metadata is wrong. The only correct path is to let the engine build the image for us.
 *
 * ## What is verified and what is inferred
 *
 * Recovered from the shipped `libminecraftpe.so` (arm64-v8a, 1.26.60.28):
 *  - `SerializedSkinRef` layout: `getImageData() -> this + 0x78`, `getCapeImageData() ->
 *    this + 0xa8`, `getAnimatedImageData() -> this + 0xd8`. The 0x30 gap between the base skin and
 *    the cape is exactly one `mce::Image`, so **`sizeof(mce::Image) == 0x30`** with 8-byte
 *    alignment (a 3-pointer + 3-integer struct). See `skinlayout` in `GameCosmetics.h`.
 *  - `mce::ImageUtils::loadImageFromMemory(mce::Image& out, ImageFormat, const unsigned char*,
 *    size_t, bool)` is resolved by the per-version `imageLoaderSig` byte pattern; the class exports
 *    no symbol, so a pattern match is the only handle.
 *  - The decoder dispatches on `ImageFormat`: PNG goes through `stb_image` (`stbi_load*`), and
 *    `ImageFormat::RGBA8` (value 4) is memcpy'd, so both PNG and raw RGBA can be built by the
 *    engine. The `ImageFormat` values below are the documented `mce::ImageFormat` enumeration.
 *
 * ## The hard boundary (stated plainly so nothing over-claims)
 *
 * The `mce::Image` **internal field layout is not recoverable** from the stripped binary, and the
 * return convention of `loadImageFromMemory` (`brstd::expected` via the hidden sret pointer in
 * `x8`, versus a direct return) is confirmed per build from the loader's prologue. This module
 * therefore:
 *  - copies the whole opaque struct rather than poking a buffer pointer inside it;
 *  - verifies the recovered ABI at init (a one-shot probe) and, if it does not hold, **prefers the
 *    already-proven SerializedSkinRef struct-swap over installing a detour**;
 *  - installs the buffer-pointer hook only when the target function's argument shape can be
 *    recognised from real evidence, and otherwise stays fail-closed.
 *
 * Nothing here dereferences an argument of an unknown function. A wrong guess therefore cannot read
 * freed memory or corrupt a return value; the worst case is that the hook is not installed and the
 * engine runs exactly as it does without us.
 */
#include <cstddef>
#include <cstdint>

namespace pl::runtime {

// ---------------------------------------------------------------------------------------------
// Arm64 struct layouts
// ---------------------------------------------------------------------------------------------

namespace mce {

/**
 * @brief `mce::Image` storage size, in bytes, on arm64-v8a.
 *
 * Derived from the `SerializedSkinRef` accessors: the base skin image at `+0x78` and the cape image
 * at `+0xa8` are separated by exactly 0x30 with nothing between them, so one `mce::Image` is 0x30
 * bytes. The struct is treated as **opaque**: we never read or write a field inside it by hand,
 * because the stripped binary does not expose the layout. All access is a whole-struct copy or a
 * call into the engine's own loader.
 */
constexpr std::size_t kImageBytes = 0x30;

/**
 * @brief Alignment for an `mce::Image` buffer on arm64-v8a.
 *
 * The loader writes the head of the out struct with a 128-bit SIMD store (`str q0, [x20]` in the
 * disassembly), so a stack buffer handed to it is aligned to 16 rather than the struct's minimum 8.
 * AArch64 tolerates an unaligned normal-memory store, but 16 removes the question entirely and
 * costs nothing. (The `SerializedSkinRef` member at `+0xa8` is 8-aligned; that is the engine's own
 * storage, which it wrote, not something we lay out.)
 */
constexpr std::size_t kImageAlignment = 16;

/**
 * @brief Opaque, correctly-sized and correctly-aligned stand-in for `mce::Image`.
 *
 * Only ever copied whole. Declared with explicit alignment so a stack instance can be handed to the
 * engine loader and then `memcpy`'d into a `SerializedSkinRef` without a misaligned access.
 */
struct alignas(kImageAlignment) ImageOpaque {
  std::uint8_t bytes[kImageBytes];
};
static_assert(sizeof(ImageOpaque) == kImageBytes, "mce::Image stand-in must be exactly 0x30 bytes");
static_assert(alignof(ImageOpaque) == kImageAlignment, "mce::Image stand-in alignment");

/**
 * @brief `mce::ImageFormat` values, as documented for the shipped builds.
 *
 * The decoder branches on these, so they are the contract for building an image from either a PNG
 * or raw pixels. `kPng` is the cape/hat texture route; `kRgba8` is the route for pixels the
 * launcher has already decoded. A build that renumbers these is caught by the init-time probe,
 * which builds an image and checks the result rather than trusting the constant.
 */
enum class ImageFormat : std::uint32_t {
  kUnknown = 0,
  kPng = 1,
  kRgba8 = 4,
  kBgra8 = 5,
  kRgb8 = 6,
  kRgba16 = 7,
};

/**
 * @brief `mce::ImageUsage`, the hint the engine uses to place the buffer.
 *
 * `kSrgb`/`kTexture` are the skin/cape cases (colour data uploaded to VRAM); `kData` is CPU-only.
 * The engine tolerates `kUnknown`, but supplying the real usage avoids a re-upload.
 */
enum class ImageUsage : std::uint32_t {
  kUnknown = 0,
  kSrgb = 1,
  kData = 2,
  kTexture = 3,
  kRenderTarget = 4,
};

/**
 * @brief Padded view of the buffer backing store (`cg::Buffer` / `mce::Blob`).
 *
 * `cg::Buffer` on arm64-v8a is `{ void* data; std::size_t size; std::size_t capacity; }` — 8-byte
 * aligned, no trailing padding, exactly 24 bytes. The engine's allocator owns `data`; a launcher
 * must never free it. This struct exists so the layout is documented and asserted, not so a caller
 * can substitute a `malloc` buffer (that is exactly the double-free the module exists to prevent).
 */
struct BufferRef {
  void *data;
  std::size_t size;
  std::size_t capacity;
};
static_assert(sizeof(BufferRef) == 24, "cg::Buffer view must be 24 bytes on arm64-v8a");
static_assert(alignof(BufferRef) == 8, "cg::Buffer view must be 8-byte aligned");

} // namespace mce

// ---------------------------------------------------------------------------------------------
// Engine-backed allocation
// ---------------------------------------------------------------------------------------------

/**
 * @brief Allocates a pixel buffer the engine can own and free.
 *
 * The engine frees an `mce::Image`'s backing store through its own allocator when the skin unloads.
 * A buffer from a foreign allocator (the launcher's `malloc`, or a `std::vector`) breaks that: the
 * engine's free reads our pointer as one of its own blocks and faults or double-frees. This helper
 * provides a buffer from a small, tracked allocator whose free path matches, and stamps a size and
 * magic in a header before the returned pointer so a mismatched or double free is refused instead
 * of corrupting the heap.
 *
 * Prefer `BuildImageFromPng`/`BuildImageFromRgba`, which let the engine allocate its own buffer and
 * copy into it. Use this only when a buffer must be produced before the engine call.
 *
 * @param bytes     requested size; 0 returns nullptr
 * @param alignment requested alignment (must be a power of two; clamped to >= alignof(void*))
 * @return a zeroed, tracked, engine-freeable pointer, or nullptr on failure
 */
void *EngineAllocate(std::size_t bytes, std::size_t alignment);

/**
 * @brief Frees a pointer from `EngineAllocate`.
 *
 * Safe on nullptr. A pointer that was not produced by `EngineAllocate` (bad magic, or a pointer
 * already freed) is refused and logged rather than freed, so a double free cannot reach the heap.
 */
void EngineFree(void *ptr) noexcept;

// ---------------------------------------------------------------------------------------------
// Engine image construction
// ---------------------------------------------------------------------------------------------

/**
 * @brief Builds a valid engine `mce::Image` from PNG bytes.
 *
 * Calls `mce::ImageUtils::loadImageFromMemory` so the engine allocates and formats the buffer
 * itself; the result's internal fields are correct by construction. `outImage` must point at
 * `mce::kImageBytes` bytes and is zeroed first, so a failed build leaves a known-empty struct.
 *
 * @param png      PNG bytes (need not outlive the call once the engine has copied)
 * @param size     PNG length
 * @param outImage a `mce::kImageBytes`-byte, `mce::kImageAlignment`-aligned buffer
 * @return true when the engine reported success and filled `outImage`
 */
bool BuildImageFromPng(const std::uint8_t *png, std::size_t size, void *outImage);

/**
 * @brief Builds a valid engine `mce::Image` from raw RGBA8 pixels.
 *
 * The `ImageFormat::kRgba8` path is a memcpy in the engine, so the pixel data is the only input.
 * Useful for a cape/hat texture the launcher already decoded.
 *
 * @param rgba     width*height*4 bytes, top-left origin
 * @param width    pixel width
 * @param height   pixel height
 * @param outImage a `mce::kImageBytes`-byte, `mce::kImageAlignment`-aligned buffer
 * @return true when the engine reported success and filled `outImage`
 */
bool BuildImageFromRgba(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
                        void *outImage);

/**
 * @brief Releases an engine-built image, if the engine exposes a destructor we resolved.
 *
 * A no-op when no release path is available; the image then lives until the skin that owns it
 * unloads, which is the engine's own lifetime and never a leak on the launcher's side.
 */
void DestroyImage(void *image) noexcept;

/** True when the init probe confirmed the engine image path works on this build. */
bool IsImagePathVerified();

// ---------------------------------------------------------------------------------------------
// The mce::Image pipeline hook
// ---------------------------------------------------------------------------------------------

/**
 * @brief Installs the `mce::Image` pipeline detour. Called once from `InitCosmeticsHooks`.
 *
 * The detour intercepts the engine's image-buffer submission, so a pixel buffer bound for a skin
 * can be swapped for a custom cosmetic **at the moment the engine uploads it**, with the engine
 * still owning the lifetime. It:
 *  - checks that a custom cosmetic is registered for this submission (keyed by the texture id the
 *    engine passes, when that can be identified);
 *  - rewrites the pixel-buffer pointer and byte count to the registered override;
 *  - calls the original trampoline with the rewritten arguments.
 *
 * **Installation is gated on evidence, never on hope.** A Dobby-style detour on a wrong function
 * corrupts the game with no diagnostic, so the hook is installed only when the init probe confirms
 * the engine image path AND the recovered `LoadImageFn` ABI. When either fails, the module keeps
 * the SerializedSkinRef struct-swap path (already proven) and logs which gate failed.
 */
void InitMceImageHook();

/**
 * @brief Registers a substitution keyed by the bytes the engine loads.
 *
 * The loader seam is only ever handed raw pixel/PNG bytes with no player or texture id, so the one
 * stable key available is the content hash of the *incoming* bytes. Registering
 * `(sourceBytes -> replacementRgba)` means "whenever the engine loads these exact bytes, build the
 * image from this RGBA instead". The engine loads the same source texture bytes on every load, so
 * the hash is stable. The pixels are copied; a zero-size replacement clears the entry.
 */
void SetContentSubstitution(const std::uint8_t *sourceBytes, std::size_t sourceSize,
                            const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height);

/** Clears every content-addressed substitution. */
void ClearContentSubstitutions();

/** Number of content-addressed substitutions registered. */
std::size_t SubstitutionCount();

/**
 * @brief Arms a one-shot override for the very next image the engine builds.
 *
 * Use this when the caller can trigger exactly one known upload (e.g. a skin rebind it initiates)
 * and wants that one image replaced. The override is consumed on the next loader call and cleared,
 * so it can never leak onto an unrelated later upload; call `ClearNextImageOverride` to disarm it.
 */
void ArmNextImageOverride(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height);

/** Disarms the one-shot override. */
void ClearNextImageOverride();

/** True once the pipeline hook has run at least once this session. */
bool IsMceImageHookLive();

/**
 * @brief Reads the pipeline hook counters.
 *
 * @param out Receives {hookCalls, substitutions, overridesAvailable, bufferBytesSeen}.
 * @return true when the hook has run at least once.
 */
bool ReadMceImageHookStats(std::uint32_t out[4]);

// ---------------------------------------------------------------------------------------------
// Texture invalidation / cache flush
// ---------------------------------------------------------------------------------------------

/** The callback a caller can install to request an engine texture-cache flush. */
using TextureFlushFn = void (*)(void *user);

/**
 * @brief Installs the engine texture-cache flusher used after a substitution.
 *
 * The module cannot itself reach RenderDragon's `mce::TextureGroup` / `SkinRepository` cache: those
 * types are not RTTI-resolvable and the game strips the symbols, so a flush is **invoked through a
 * hook the caller has proven** (the preloader's resource-pack reload seam, or a verified
 * `onSkinChanged`/`invalidate` vtable entry). Installing a flusher is optional; without one the
 * module still requests a refresh through the weakest safe hint it has and reports the
 * best-effort status rather than claiming the cache was flushed.
 */
void SetTextureCacheFlusher(TextureFlushFn fn, void *user);

/**
 * @brief Requests a texture-cache refresh after a substitution.
 *
 * @return true when a proven flusher ran; false when only a best-effort hint was available (the
 *         caller should treat the re-upload as "next frame / next skin bind", not immediate).
 */
bool RequestTextureCacheFlush();

/** True when a caller has installed a proven texture-cache flusher. */
bool IsTextureCacheFlushAvailable();

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

/**
 * @brief Reads init-time diagnostics for a status screen or a log dump.
 *
 * @param out Receives {loaderAddress, abiUsesSret, imageBuiltOk, hookInstalled}.
 */
void ReadMceImageHookDiagnostics(std::uint64_t out[4]);

} // namespace pl::runtime
