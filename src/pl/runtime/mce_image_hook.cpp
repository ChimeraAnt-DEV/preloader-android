#include "pl/runtime/mce_image_hook.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "pl/Logger.hpp"
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

} // namespace pl::runtime
