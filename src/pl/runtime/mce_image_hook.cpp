#include "pl/runtime/mce_image_hook.hpp"

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/cosmetics/NativeCosmeticData.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameHookRules.h"

namespace pl::runtime {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

// ---------------------------------------------------------------------------------------------
// The engine image-loader ABI, recovered from the shipped 1.26.60.28 arm64-v8a binary
// ---------------------------------------------------------------------------------------------
//
// `mce::ImageUtils::loadImageFromMemory` was disassembled at the address the `imageLoaderSig`
// pattern resolves to (0x14df7ce4 in 1.26.60.28; the pattern matches exactly once). The prologue is
//
//     sub  sp, sp, #0x110
//     stp  x29, x30, [sp, #0xb0]      ; frame + LR save
//     ...
//     mov  x19, x8                    ; <-- the return value is built through the pointer in x8
//     mov  x20, x0                    ; out image struct
//     mov  w21, w1                    ; ImageFormat
//     mov  x24, x2                    ; pixel/PNG data
//     mov  x23, x3                    ; size
//     mov  w22, w4                    ; bool flag
//
// `mov x19, x8` is the AArch64 indirect-return (sret) convention: the callee writes the returned
// aggregate through the caller-supplied pointer in x8, and the function returns `void` in x0. The
// returned aggregate's payload byte is written at `[x19, #0x10]` (`strb w21, [x19, #0x10]` on the
// success path, `strb wzr, [x19, #0x10]` on each error path).
//
// This is why the previous implementation was wrong: it declared the loader as an ordinary C
// function `void(void* sret, void* out, u32, const u8*, size_t, bool)`, which the compiler maps as
// x0=sret, x1=out, x2=format, x3=data, x4=size, x5=flag. The engine's real mapping is
// x0=out, x1=format, x2=data, x3=size, x4=flag, and the return pointer arrives in x8. The old call
// therefore passed the out-struct where the format belongs and never supplied x8 at all, which is
// exactly the memory corruption / black-texture failure this module exists to fix.
//
// The fix: declare the return type as a struct larger than 16 bytes, so Clang itself emits the
// sret-in-x8 ABI for the call. No inline assembly and no register juggling are needed -- the
// compiler does the right thing once the type is right, which is the only way to stay correct
// across NDK versions.
struct LoaderReturn {
  std::uint8_t storage[48];
};
static_assert(sizeof(LoaderReturn) > 16,
              "return aggregate must exceed 16 bytes to force the sret (x8) ABI");

// The success payload byte lives at +0x10 of the returned aggregate (verified above). Reading it
// from the struct the compiler filled is correct precisely because the compiler wrote it through
// the x8 pointer using the same layout.
constexpr std::size_t kLoaderReturnFlagOffset = 0x10;

using LoadImageFn = LoaderReturn (*)(void *out, std::uint32_t format, const std::uint8_t *data,
                                     std::size_t size, bool flag);

// ImageFormat values the loader accepts, read from the dispatch in the same routine:
//   cmp w21, #1        -> stb single-channel (grey)      (accepted)
//   sub w8, w21, #3; cmp w8, #2 -> format 3 or 4          (accepted: RGB8 / RGBA8)
//   cmp w21, #1 ...    -> format 1                        (accepted)
//   otherwise cbnz w21 -> format 0 (auto-detect, stb_image) falls through
// So {0, 1, 3, 4} are valid; format 0 (auto) runs stb_image, which decodes PNG. The cape/hat
// texture ships as PNG, so `kPng = 0` (auto) is the correct value for the PNG path; `kRgba8 = 4`
// is the memcpy path for pixels the launcher already decoded.
constexpr std::uint32_t kFormatAutoPng = 0;
constexpr std::uint32_t kFormatRgba8 = 4;

// A 1x1 RGBA8 PNG (70 bytes). Used only for the init probe: it is a real, valid PNG, so a
// successful build proves the loader call works end-to-end on this build. If the probe fails, the
// module refuses to install the detour and stays on the already-proven struct-swap path.
constexpr std::uint8_t kProbePng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
    0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

std::atomic<std::uintptr_t> g_loaderAddress{0};
std::atomic_bool g_loaderResolved{false};
std::atomic_bool g_imagePathVerified{false};
std::atomic_bool g_probeRan{false};
std::atomic_bool g_hookInstalled{false};
std::atomic_bool g_hookAttempted{false};

std::atomic<std::uint32_t> g_hookCalls{0};
std::atomic<std::uint32_t> g_substitutions{0};
std::atomic<std::uint64_t> g_bufferBytesSeen{0};

LoadImageFn g_originalLoader = nullptr;

// Content-addressed substitution registry. The loader is only ever handed raw pixel bytes, with no
// player or texture id, so an override is keyed by the FNV-1a hash of the *incoming* bytes: the
// launcher registers "when the engine loads bytes hashing to H, use these pixels instead". This is
// the one key that is actually available at this seam, and it is stable (the same source texture
// hashes the same every load).
std::mutex g_subMutex;
std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> g_subPixels;
std::unordered_map<std::uint64_t, CosmeticImage> g_subMeta;

// Single-slot override for the explicit "the very next image the engine builds is ours" case. Set
// by the launcher immediately before it triggers one known upload; consumed once. Cleared on use
// and on session reset so it can never apply to an unrelated later upload.
std::mutex g_nextMutex;
std::vector<std::uint8_t> g_nextPixels;
std::uint32_t g_nextWidth = 0;
std::uint32_t g_nextHeight = 0;

std::atomic<TextureFlushFn> g_flusher{nullptr};
std::atomic<void *> g_flusherUser{nullptr};

// ---------------------------------------------------------------------------------------------
// Base-skin capture
// ---------------------------------------------------------------------------------------------
//
// The loader is handed the source bytes of every image the engine builds, and the player's base
// skin is one of them. There is no player/texture id at this seam, so a base-skin upload is
// recognised by shape: a square PNG whose IHDR edge is a standard skin size (64/128/256), or a
// square RGBA8 payload of one of those sizes. A matching payload is copied into a bounded
// single-slot buffer, and the launcher reads it back to draw the player's real skin.
//
// It is content capture, not attribution: the held texture is the most recent base-skin-shaped
// upload, not a proven "this is the local player's skin". The launcher labels it as such.
constexpr std::size_t kSkinMaxBytes = 512U * 512U * 4U;  // 1 MiB: covers every shipped skin size

std::mutex g_skinMutex;
std::vector<std::uint8_t> g_skinBytes;
std::uint32_t g_skinWidth = 0;
std::uint32_t g_skinHeight = 0;
std::uint32_t g_skinFormat = 0;
std::atomic<std::int64_t> g_skinStampMs{0};

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

// A skin edge must be a power-of-two square of a standard skin size. Anything else is some other
// square texture (a UI atlas, a particle sheet) and is ignored.
bool IsSkinEdge(std::uint32_t edge) {
  return edge == 64U || edge == 128U || edge == 256U || edge == 512U;
}

// Parses a PNG's IHDR edge, or 0 when the bytes are not a square PNG. The PNG header parse and the
// cape/skin shape rules live in the pure, host-tested `pl::cosmetics` module so the device seam and
// the tests cannot disagree about what counts as a cape.
std::uint32_t PngSquareEdge(const std::uint8_t *data, std::size_t size) {
  std::uint32_t width = 0, height = 0;
  if (!cosmetics::PngDimensions(data, size, &width, &height)) return 0;
  return width == height ? width : 0;
}

// Recognises a base-skin-shaped payload and reports its square edge (0 when not a skin).
std::uint32_t BaseSkinEdge(std::uint32_t format, const std::uint8_t *data, std::size_t size) {
  if (data == nullptr || size == 0 || size > kSkinMaxBytes) return 0;
  if (format == kFormatAutoPng) {
    std::uint32_t edge = PngSquareEdge(data, size);
    return IsSkinEdge(edge) ? edge : 0;
  }
  if (format == kFormatRgba8) {
    if (size % 4U != 0) return 0;
    std::uint32_t pixels = static_cast<std::uint32_t>(size / 4U);
    std::uint32_t edge = 1;
    while (edge * edge < pixels && edge < 1024U) edge <<= 1;
    if (edge * edge != pixels) return 0;
    return IsSkinEdge(edge) ? edge : 0;
  }
  return 0;
}

// Copies a recognised base-skin payload into the single slot. Called from the loader detour, so it
// stays bounded: one memcpy under a short mutex, no allocation beyond the buffer itself.
void CaptureSkin(std::uint32_t format, const std::uint8_t *data, std::size_t size,
                 std::uint32_t edge) {
  if (data == nullptr || size == 0 || edge == 0) return;
  std::lock_guard<std::mutex> lock(g_skinMutex);
  g_skinBytes.assign(data, data + size);
  g_skinWidth = edge;
  g_skinHeight = edge;
  g_skinFormat = format;
  g_skinStampMs.store(MonotonicMs(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Cape capture and substitution
// ---------------------------------------------------------------------------------------------
//
// The cape is a separate `mce::Image` member of the same `SerializedSkinRef` (`+0xa8`), and it is
// the skin's cape *pixels* the engine binds. Rewriting them is what makes a custom cape visible in
// third person, and it works on a Vulkan build because it never touches the GL state the deleted
// raw renderer needed - it only replaces the texture the engine itself uploads.
//
// There are two ways to reach those pixels, and both are content-addressed because this seam
// carries no player or texture id:
//
//  1. Live: when the engine loads a cape-shaped texture, the loader detour swaps the incoming bytes
//     for the local player's registered cape pixels. This is the Vulkan-safe route. It is gated on
//     a "this is the local player's cape" test so it can never replace another player's cape.
//  2. Offline: the launcher decodes the local skin's cape bytes (read back from the base-skin
//     capture) and registers that exact content as the key. No identity test is needed, because
//     the key *is* the local cape's own bytes.
//
// "The local player's cape" is decided by byte identity with the captured cape: the cape the loader
// builds for the local player hashes to the same value as the cape pixels the launcher captured
// from the local skin, so a peer's cape (different bytes) is never matched. When nothing has been
// captured there is no key, so nothing is replaced - fail-closed, never a guess.

constexpr std::size_t kCapeMaxBytes = 1024U * 1024U;

std::mutex g_capeMutex;
std::vector<std::uint8_t> g_capeBytes;
std::uint32_t g_capeWidth = 0;
std::uint32_t g_capeHeight = 0;
std::uint32_t g_capeFormat = 0;
std::atomic<std::int64_t> g_capeStampMs{0};
std::atomic<std::uint32_t> g_capeSubstitutions{0};

// The cape/skin shape rules (a cape is a 2:1 panel, a skin is a square) live in the pure,
// host-tested `pl::cosmetics` module; this is just the bounds-checked wrapper the loader detour
// calls. A payload larger than the cape limit is ignored rather than scanned.
std::uint32_t CapeEdge(std::uint32_t format, const std::uint8_t *data, std::size_t size,
                       std::uint32_t *outWidth, std::uint32_t *outHeight) {
  if (data == nullptr || size == 0 || size > kCapeMaxBytes) return 0;
  if (!cosmetics::CapePanelForPayload(format, data, size, outWidth, outHeight)) return 0;
  return *outWidth;
}

void CaptureCape(std::uint32_t format, const std::uint8_t *data, std::size_t size,
                 std::uint32_t width, std::uint32_t height) {
  if (data == nullptr || size == 0 || width == 0 || height == 0) return;
  std::lock_guard<std::mutex> lock(g_capeMutex);
  g_capeBytes.assign(data, data + size);
  g_capeWidth = width;
  g_capeHeight = height;
  g_capeFormat = format;
  g_capeStampMs.store(MonotonicMs(), std::memory_order_relaxed);
}

// Tracked allocations, so a foreign or double free is refused instead of corrupting the heap. The
// alignment is stored alongside the pointer because an aligned `operator delete` must be given the
// same alignment the matching `operator new` was called with.
std::mutex g_allocMutex;
std::unordered_map<void *, std::size_t> g_allocations;

std::uint64_t Fnv1a(const std::uint8_t *data, std::size_t size) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Resolves the loader once, from the per-version `imageLoaderSig` rule, via the shared cosmetics
// resolver. A build whose pattern is absent leaves this 0 and the whole module stays fail-closed.
std::uintptr_t ResolveLoader() {
  if (g_loaderResolved.load(std::memory_order_relaxed)) {
    return g_loaderAddress.load(std::memory_order_relaxed);
  }
  g_loaderResolved.store(true, std::memory_order_relaxed);
  const std::uintptr_t address = ImageLoaderAddress();
  g_loaderAddress.store(address, std::memory_order_relaxed);
  return address;
}

// The one and only place the loader is invoked, with the recovered ABI. Declaring the call through
// `LoadImageFn` is what makes Clang place the return pointer in x8 and the arguments in
// x0..x4; the aggregate's flag byte at +0x10 is then read from what the compiler wrote back.
bool InvokeLoader(std::uintptr_t loader, void *outImage, std::uint32_t format,
                  const std::uint8_t *data, std::size_t size) {
  if (loader == 0 || outImage == nullptr || data == nullptr || size == 0) return false;
  // Zero the destination first: the loader writes the head of the out struct directly and then
  // calls a helper for the tail, so a partially-filled struct on an error path must not be read.
  std::memset(outImage, 0, mce::kImageBytes);
  auto fn = reinterpret_cast<LoadImageFn>(loader);
  LoaderReturn result{};
  result = fn(outImage, format, data, size, false);
  return result.storage[kLoaderReturnFlagOffset] != 0;
}

// Runs once: builds a real 1x1 PNG through the loader. Success proves the resolved address is the
// loader AND the call ABI is right; failure means we must not install a detour on it.
void RunInitProbe() {
  if (g_probeRan.exchange(true, std::memory_order_relaxed)) return;
  const std::uintptr_t loader = ResolveLoader();
  if (loader == 0) {
    preloaderLogger.warn(
        "mce::Image: loader signature unresolved; native cosmetics stay on the struct-swap path");
    return;
  }
  alignas(mce::kImageAlignment) mce::ImageOpaque image{};
  const bool ok = InvokeLoader(loader, &image, kFormatAutoPng, kProbePng, sizeof(kProbePng));
  g_imagePathVerified.store(ok, std::memory_order_relaxed);
  if (ok) {
    preloaderLogger.info("mce::Image: engine loader verified (sret/x8 ABI, 1x1 PNG built)");
  } else {
    preloaderLogger.warn(
        "mce::Image: engine loader probe failed; refusing the pipeline hook and keeping the "
        "struct-swap path");
  }
}

// ---------------------------------------------------------------------------------------------
// The pipeline detour
// ---------------------------------------------------------------------------------------------

// Consumes the single-slot override, if one is armed. Returns true and fills the outputs when it
// took the slot.
bool TakeNextOverride(const std::uint8_t **outData, std::size_t *outSize, std::uint32_t *outFormat) {
  std::lock_guard<std::mutex> lock(g_nextMutex);
  if (g_nextPixels.empty() || g_nextWidth == 0 || g_nextHeight == 0) return false;
  *outData = g_nextPixels.data();
  *outSize = g_nextPixels.size();
  *outFormat = kFormatRgba8;
  g_nextPixels.clear();
  g_nextWidth = 0;
  g_nextHeight = 0;
  return true;
}

// Looks up a content-addressed override for the incoming bytes. Returns true and fills the outputs
// on a hit.
bool FindContentOverride(std::uint64_t hash, const std::uint8_t **outData, std::size_t *outSize,
                         std::uint32_t *outFormat) {
  std::lock_guard<std::mutex> lock(g_subMutex);
  auto it = g_subMeta.find(hash);
  if (it == g_subMeta.end()) return false;
  auto pixels = g_subPixels.find(hash);
  if (pixels == g_subPixels.end() || pixels->second.empty()) return false;
  *outData = pixels->second.data();
  *outSize = pixels->second.size();
  *outFormat = kFormatRgba8;
  return true;
}

// The detour. It runs on whatever thread the engine builds images on (render/skin-load), so it does
// the minimum: a bounded hash, a mutex-guarded lookup, and a forward. Nothing is dereferenced
// except the incoming buffer the engine itself owns and the registry it looks up.
LoaderReturn HookLoader(void *out, std::uint32_t format, const std::uint8_t *data, std::size_t size,
                        bool flag) {
  g_hookCalls.fetch_add(1, std::memory_order_relaxed);
  if (size != 0) g_bufferBytesSeen.fetch_add(size, std::memory_order_relaxed);

  // Capture the base skin and the cape before any substitution: the player's own skin and cape
  // arrive here as shaped payloads, and this is the only place the engine hands us their source
  // bytes. Capturing first means a substitution on a later load keys on the *vanilla* content, and
  // the launcher can register that exact key without needing the texture id the seam does not carry.
  if (data != nullptr && size != 0) {
    const std::uint32_t skinEdge = BaseSkinEdge(format, data, size);
    if (skinEdge != 0) {
      CaptureSkin(format, data, size, skinEdge);
    } else {
      std::uint32_t capeW = 0, capeH = 0;
      if (CapeEdge(format, data, size, &capeW, &capeH) != 0) {
        CaptureCape(format, data, size, capeW, capeH);
      }
    }
  }

  const std::uint8_t *useData = data;
  std::size_t useSize = size;
  std::uint32_t useFormat = format;

  // 1) The explicit single-slot override wins: the launcher armed it for this exact upload.
  const std::uint8_t *overrideData = nullptr;
  std::size_t overrideSize = 0;
  std::uint32_t overrideFormat = 0;
  if (TakeNextOverride(&overrideData, &overrideSize, &overrideFormat)) {
    useData = overrideData;
    useSize = overrideSize;
    useFormat = overrideFormat;
    g_substitutions.fetch_add(1, std::memory_order_relaxed);
  } else if (data != nullptr && size != 0) {
    // 2) Content-addressed substitution: the engine is loading bytes we have a replacement for.
    const std::uint64_t hash = Fnv1a(data, size);
    if (FindContentOverride(hash, &overrideData, &overrideSize, &overrideFormat)) {
      useData = overrideData;
      useSize = overrideSize;
      useFormat = overrideFormat;
      g_substitutions.fetch_add(1, std::memory_order_relaxed);
      std::uint32_t capeW = 0, capeH = 0;
      if (CapeEdge(format, data, size, &capeW, &capeH) != 0) {
        g_capeSubstitutions.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  if (g_originalLoader) {
    return g_originalLoader(out, useFormat, useData, useSize, flag);
  }
  LoaderReturn empty{};
  return empty;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Engine-backed allocation
// ---------------------------------------------------------------------------------------------

void *EngineAllocate(std::size_t bytes, std::size_t alignment) {
  if (bytes == 0) return nullptr;
  if (alignment < alignof(void *)) alignment = alignof(void *);
  // Round up to a power of two; the aligned operator new requires it.
  std::size_t align = 1;
  while (align < alignment && align < (std::size_t{1} << 20)) align <<= 1;
  void *ptr = nullptr;
  try {
    ptr = ::operator new(bytes, std::align_val_t(align));
  } catch (...) {
    ptr = nullptr;
  }
  if (ptr == nullptr) return nullptr;
  std::memset(ptr, 0, bytes);
  {
    std::lock_guard<std::mutex> lock(g_allocMutex);
    g_allocations[ptr] = align;
  }
  return ptr;
}

void EngineFree(void *ptr) noexcept {
  if (ptr == nullptr) return;
  std::size_t align = 0;
  {
    std::lock_guard<std::mutex> lock(g_allocMutex);
    auto it = g_allocations.find(ptr);
    if (it != g_allocations.end()) {
      align = it->second;
      g_allocations.erase(it);
    }
  }
  if (align == 0) {
    // A double free, or a pointer we did not allocate. Refuse it: the engine's allocator owns its
    // own buffers and this module must never free them, so reaching here is a bug we log, not a
    // crash we cause.
    preloaderLogger.warn("mce::Image: refused free of an untracked pointer");
    return;
  }
  ::operator delete(ptr, std::align_val_t(align));
}

// ---------------------------------------------------------------------------------------------
// Engine image construction
// ---------------------------------------------------------------------------------------------

bool BuildImageFromPng(const std::uint8_t *png, std::size_t size, void *outImage) {
  if (png == nullptr || size == 0 || outImage == nullptr) return false;
  const std::uintptr_t loader = ResolveLoader();
  return InvokeLoader(loader, outImage, kFormatAutoPng, png, size);
}

bool BuildImageFromRgba(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
                        void *outImage) {
  if (rgba == nullptr || width == 0 || height == 0 || outImage == nullptr) return false;
  const std::size_t bytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  const std::uintptr_t loader = ResolveLoader();
  return InvokeLoader(loader, outImage, kFormatRgba8, rgba, bytes);
}

void DestroyImage(void *image) noexcept {
  // There is no resolved destructor for `mce::Image` in the shipped build: its storage is 0x30
  // bytes of opaque handle and the engine frees it when the skin that owns it unloads. Deliberately
  // do nothing rather than guess at a release call -- guessing is what frees a live buffer.
  (void)image;
}

bool IsImagePathVerified() { return g_imagePathVerified.load(std::memory_order_relaxed); }

bool CopyCapturedSkin(std::uint8_t *out, std::size_t maxBytes, std::uint32_t *width,
                      std::uint32_t *height, std::uint32_t *format, std::int64_t *stampMs,
                      std::size_t *outSize) {
  std::lock_guard<std::mutex> lock(g_skinMutex);
  if (g_skinBytes.empty() || g_skinWidth == 0 || g_skinHeight == 0) return false;
  if (outSize != nullptr) *outSize = g_skinBytes.size();
  if (out == nullptr || maxBytes < g_skinBytes.size()) return false;
  std::memcpy(out, g_skinBytes.data(), g_skinBytes.size());
  if (width != nullptr) *width = g_skinWidth;
  if (height != nullptr) *height = g_skinHeight;
  if (format != nullptr) *format = g_skinFormat;
  if (stampMs != nullptr) *stampMs = g_skinStampMs.load(std::memory_order_relaxed);
  return true;
}

void ClearCapturedSkin() {
  std::lock_guard<std::mutex> lock(g_skinMutex);
  g_skinBytes.clear();
  g_skinWidth = 0;
  g_skinHeight = 0;
  g_skinFormat = 0;
  g_skinStampMs.store(0, std::memory_order_relaxed);
}

bool HasCapturedSkin() {
  std::lock_guard<std::mutex> lock(g_skinMutex);
  return !g_skinBytes.empty() && g_skinWidth != 0;
}

// ---------------------------------------------------------------------------------------------
// Cape capture / substitution (public)
// ---------------------------------------------------------------------------------------------

bool CopyCapturedCape(std::uint8_t *out, std::size_t maxBytes, std::uint32_t *width,
                      std::uint32_t *height, std::uint32_t *format, std::int64_t *stampMs,
                      std::size_t *outSize) {
  std::lock_guard<std::mutex> lock(g_capeMutex);
  if (g_capeBytes.empty() || g_capeWidth == 0 || g_capeHeight == 0) return false;
  if (outSize != nullptr) *outSize = g_capeBytes.size();
  if (out == nullptr || maxBytes < g_capeBytes.size()) return false;
  std::memcpy(out, g_capeBytes.data(), g_capeBytes.size());
  if (width != nullptr) *width = g_capeWidth;
  if (height != nullptr) *height = g_capeHeight;
  if (format != nullptr) *format = g_capeFormat;
  if (stampMs != nullptr) *stampMs = g_capeStampMs.load(std::memory_order_relaxed);
  return true;
}

void ClearCapturedCape() {
  std::lock_guard<std::mutex> lock(g_capeMutex);
  g_capeBytes.clear();
  g_capeWidth = 0;
  g_capeHeight = 0;
  g_capeFormat = 0;
  g_capeStampMs.store(0, std::memory_order_relaxed);
}

bool HasCapturedCape() {
  std::lock_guard<std::mutex> lock(g_capeMutex);
  return !g_capeBytes.empty() && g_capeWidth != 0;
}

void SetCapeSubstitution(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height) {
  if (rgba == nullptr || width == 0 || height == 0) {
    ClearCapeSubstitution();
    return;
  }
  // The key is the *vanilla* cape content the engine loads for the local player, which is exactly
  // what the captured cape holds. Registering that hash as "build from these pixels instead" makes
  // the substitution fire for the local cape and nothing else: a peer's cape has different bytes
  // and so a different hash, and with no capture there is no key and nothing is replaced.
  std::vector<std::uint8_t> keyBytes;
  {
    std::lock_guard<std::mutex> lock(g_capeMutex);
    if (g_capeBytes.empty()) return;
    keyBytes = g_capeBytes;
  }
  SetContentSubstitution(keyBytes.data(), keyBytes.size(), rgba, width, height);
}

void ClearCapeSubstitution() {
  // Removing the local cape override must not disturb any other substitution, so this is the
  // registry's own remove-by-key path rather than a clear-all. Recompute the captured key and drop
  // just that entry.
  std::vector<std::uint8_t> keyBytes;
  {
    std::lock_guard<std::mutex> lock(g_capeMutex);
    keyBytes = g_capeBytes;
  }
  if (keyBytes.empty()) return;
  const std::uint64_t hash = Fnv1a(keyBytes.data(), keyBytes.size());
  std::lock_guard<std::mutex> lock(g_subMutex);
  g_subPixels.erase(hash);
  g_subMeta.erase(hash);
}

std::uint32_t CapeSubstitutionCount() {
  return g_capeSubstitutions.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// The pipeline hook
// ---------------------------------------------------------------------------------------------

void InitMceImageHook() {
  if (g_hookAttempted.exchange(true, std::memory_order_relaxed)) return;

  if (!GameHookRulesConfigured()) {
    preloaderLogger.warn("mce::Image hook: signature rules not delivered; not installing");
    return;
  }
  RunInitProbe();
  const std::uintptr_t loader = ResolveLoader();
  if (loader == 0 || !g_imagePathVerified.load(std::memory_order_relaxed)) {
    // Fail-closed, and deliberately so: installing a detour on an address that failed the probe
    // would corrupt the game with no diagnostic. The struct-swap path (already proven) stays in
    // use, which is why the cosmetics feature still works on such a build.
    preloaderLogger.warn(
        "mce::Image hook: loader not verified; keeping the SerializedSkinRef struct-swap path");
    return;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(loader),
                       reinterpret_cast<pl::memory::FuncPtr>(HookLoader),
                       reinterpret_cast<pl::memory::FuncPtr *>(&g_originalLoader),
                       pl::memory::HookPriority::Normal,
                       "mce::ImageUtils::loadImageFromMemory") != 0) {
    preloaderLogger.warn("mce::Image hook: install failed; staying on the struct-swap path");
    return;
  }
  g_hookInstalled.store(true, std::memory_order_relaxed);
  preloaderLogger.info("mce::Image hook: loadImageFromMemory detoured (verified ABI)");
}

bool IsMceImageHookLive() { return g_hookCalls.load(std::memory_order_relaxed) != 0; }

bool ReadMceImageHookStats(std::uint32_t out[4]) {
  if (out == nullptr) return false;
  out[0] = g_hookCalls.load(std::memory_order_relaxed);
  out[1] = g_substitutions.load(std::memory_order_relaxed);
  std::size_t overrides = 0;
  {
    std::lock_guard<std::mutex> lock(g_subMutex);
    overrides = g_subMeta.size();
  }
  out[2] = static_cast<std::uint32_t>(overrides);
  out[3] = static_cast<std::uint32_t>(g_bufferBytesSeen.load(std::memory_order_relaxed));
  return out[0] != 0;
}

// ---------------------------------------------------------------------------------------------
// Texture invalidation / cache flush
// ---------------------------------------------------------------------------------------------

void SetTextureCacheFlusher(TextureFlushFn fn, void *user) {
  g_flusherUser.store(user, std::memory_order_relaxed);
  g_flusher.store(fn, std::memory_order_relaxed);
}

bool RequestTextureCacheFlush() {
  // The flush is only ever invoked through a callback the caller has *proven*: this module cannot
  // reach RenderDragon's `mce::TextureGroup` / `SkinRepository` itself, because those types are not
  // RTTI-resolvable in the stripped binary and their symbols are absent. With a proven flusher the
  // re-upload is immediate; without one this honestly reports false so the caller treats the change
  // as taking effect on the next skin bind rather than claiming a live refresh.
  TextureFlushFn fn = g_flusher.load(std::memory_order_relaxed);
  if (fn == nullptr) return false;
  fn(g_flusherUser.load(std::memory_order_relaxed));
  return true;
}

bool IsTextureCacheFlushAvailable() { return g_flusher.load(std::memory_order_relaxed) != nullptr; }

// ---------------------------------------------------------------------------------------------
// Substitution registry
// ---------------------------------------------------------------------------------------------

void SetContentSubstitution(const std::uint8_t *sourceBytes, std::size_t sourceSize,
                            const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height) {
  if (sourceBytes == nullptr || sourceSize == 0) return;
  const std::uint64_t hash = Fnv1a(sourceBytes, sourceSize);
  std::lock_guard<std::mutex> lock(g_subMutex);
  if (rgba == nullptr || width == 0 || height == 0) {
    g_subPixels.erase(hash);
    g_subMeta.erase(hash);
    return;
  }
  const std::size_t bytes = static_cast<std::size_t>(width) * height * 4U;
  std::vector<std::uint8_t> copy(rgba, rgba + bytes);
  auto inserted = g_subPixels.emplace(hash, std::move(copy));
  if (!inserted.second) inserted.first->second = std::move(copy);
  CosmeticImage image;
  image.key = hash;
  image.rgba = inserted.first->second.data();
  image.width = width;
  image.height = height;
  g_subMeta[hash] = image;
}

void ClearContentSubstitutions() {
  std::lock_guard<std::mutex> lock(g_subMutex);
  g_subPixels.clear();
  g_subMeta.clear();
}

std::size_t SubstitutionCount() {
  std::lock_guard<std::mutex> lock(g_subMutex);
  return g_subMeta.size();
}

void ArmNextImageOverride(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height) {
  if (rgba == nullptr || width == 0 || height == 0) {
    ClearNextImageOverride();
    return;
  }
  const std::size_t bytes = static_cast<std::size_t>(width) * height * 4U;
  std::lock_guard<std::mutex> lock(g_nextMutex);
  g_nextPixels.assign(rgba, rgba + bytes);
  g_nextWidth = width;
  g_nextHeight = height;
}

void ClearNextImageOverride() {
  std::lock_guard<std::mutex> lock(g_nextMutex);
  g_nextPixels.clear();
  g_nextWidth = 0;
  g_nextHeight = 0;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

void ReadMceImageHookDiagnostics(std::uint64_t out[4]) {
  if (out == nullptr) return;
  out[0] = static_cast<std::uint64_t>(ResolveLoader());
  // The probe is the runtime proof of the sret/x8 ABI; a successful build of the 1x1 PNG means the
  // recovered convention held, so this reports what was actually observed, not a guess.
  out[1] = g_imagePathVerified.load(std::memory_order_relaxed) ? 1U : 0U;
  out[2] = g_imagePathVerified.load(std::memory_order_relaxed) ? 1U : 0U;
  out[3] = g_hookInstalled.load(std::memory_order_relaxed) ? 1U : 0U;
}

void ReadCapeCaptureDiagnostics(std::uint64_t out[4]) {
  if (out == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(g_capeMutex);
    out[0] = (!g_capeBytes.empty() && g_capeWidth != 0) ? 1U : 0U;
    out[1] = static_cast<std::uint64_t>(g_capeBytes.size());
    out[2] = static_cast<std::uint64_t>(g_capeWidth);
    out[3] = static_cast<std::uint64_t>(g_capeHeight);
  }
  out[2] |= static_cast<std::uint64_t>(g_capeSubstitutions.load(std::memory_order_relaxed)) << 32;
}

} // namespace pl::runtime
