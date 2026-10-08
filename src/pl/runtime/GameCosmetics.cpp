#include "pl/runtime/GameCosmetics.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/mce_image_hook.hpp"

namespace pl::runtime {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

// 'CHF1' little-endian, matching pl::cosmetics::CosmeticFrame.MAGIC on the Java side.
constexpr std::uint32_t kCosmeticFrameMagic = 0x43484631;

// The Skin cape entry and the texture binder are reached by RTTI name, exactly like the
// local-player feed's ClientInstance slot. The default slots are the ones the feed was designed
// around; a build that reorders them reports "unresolved" and stays on the resource-pack path.
// Overridable from the per-version signature rules, which is what makes this runtime-detected
// rather than an allowlist.
// Slots verified against the shipped 1.26.60.28 arm64-v8a binary:
//   LivePlayerRenderer vtable address point 0x15144a78, slot 17 -> 0xaeaefb4 (the render entry;
//   the same slot the player-render feed already uses), and
//   ClientNetworkHandler vtable address point 0x1520ef98, slot 40 -> 0xb80f518 (the
//   AddPlayerPacket handler, whose body references the "AddPlayerPacket: NaN position" diagnostic).
//
// A Skin object is not polymorphic (no RTTI name exists for it), so there is no reachable
// "Skin::getCapeImage" vtable; the renderer that holds the skin is the honest seam for the 2D
// cape, which is why the skin/cape slot is the renderer's render entry. The SerializedSkinRef
// cape accessors (getCapeImageData / getCapeImageDataCereal) exist but are plain methods on a
// non-virtual type, so they cannot be reached by slot.
constexpr std::size_t kDefaultSkinCapeSlot = 17;
constexpr std::size_t kDefaultPacketReadSlot = 40;
// No texture-binder slot is verified for this build, so the texture seam stays fail-closed (0):
// it is installed only when a per-version rule supplies a confirmed index.
constexpr std::size_t kDefaultTextureBindSlot = 0;

// Real RTTI type names, present as standalone typeinfo-name strings in the binary.
constexpr const char *kSkinTypeName = "18LivePlayerRenderer";
constexpr const char *kPacketHandlerTypeName = "20ClientNetworkHandler";
// The texture-atlas type (a real typeinfo-name string in this binary). Named for the
// rule-driven path only; with no verified slot the hook is not installed.
constexpr const char *kTextureTypeName = "12TextureAtlas";

std::atomic<std::uint32_t> g_skinCapeCalls{0};
std::atomic<std::uint32_t> g_textureCalls{0};
std::atomic<std::uint32_t> g_packetCalls{0};
std::atomic_bool g_skinHookInstalled{false};
std::atomic_bool g_textureHookInstalled{false};
std::atomic_bool g_packetHookInstalled{false};

// The registry. Guarded by one mutex: writes come from the launcher thread (JNI), reads from the
// game thread, and both are coarse (a set on cosmetic change, a lookup on a hook that may not be
// live) so a mutex costs nothing next to the network and GPU work around it.
std::mutex g_mutex;
std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> g_capePixels;
std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> g_texturePixels;
std::unordered_map<std::uint64_t, CosmeticImage> g_capeMeta;
std::unordered_map<std::uint64_t, CosmeticImage> g_textureMeta;
std::vector<std::uint8_t> g_geometry;
std::uint64_t g_geometryHash = 0;
// The per-frame transform buffer the launcher pushes (CosmeticFrame wire layout). Stored whole;
// the render hook parses it while drawing the local player.
std::vector<std::uint8_t> g_cosmeticFrame;

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

void StoreImage(std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> &pixels,
                std::unordered_map<std::uint64_t, CosmeticImage> &meta,
                std::uint64_t key, const std::uint8_t *rgba,
                std::uint32_t width, std::uint32_t height) {
  if (key == 0) return;
  if (rgba == nullptr || width == 0 || height == 0) {
    pixels.erase(key);
    meta.erase(key);
    return;
  }
  std::size_t bytes = static_cast<std::size_t>(width) * height * 4U;
  std::vector<std::uint8_t> copy(rgba, rgba + bytes);
  auto inserted = pixels.emplace(key, std::move(copy));
  if (!inserted.second) inserted.first->second = std::move(copy);
  CosmeticImage image;
  image.key = key;
  image.rgba = inserted.first->second.data();
  image.width = width;
  image.height = height;
  meta[key] = image;
}

const CosmeticImage *Find(const std::unordered_map<std::uint64_t, CosmeticImage> &meta,
                          std::uint64_t key) {
  auto it = meta.find(key);
  return it == meta.end() ? nullptr : &it->second;
}

// --- Hook detours ---------------------------------------------------------------------------
//
// Pure passthroughs. The real signatures of the Skin cape accessor and the texture binder are not
// recoverable (the game strips its symbols), so the detour forwards the full integer argument
// register set unchanged and dereferences nothing. That is what keeps an unknown ABI safe: a
// wrong signature cannot read freed memory or mis-handle an object, because no argument is
// touched. It only records that the hook fired.

using GenericFn = void (*)(void *, void *, void *, void *, void *, void *, void *, void *);

GenericFn g_origSkinCape = nullptr;
GenericFn g_origTextureBind = nullptr;

void HookSkinCape(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  g_skinCapeCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_origSkinCape) g_origSkinCape(a, b, c, d, e, f, g, h);
}

void HookTextureBind(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  g_textureCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_origTextureBind) g_origTextureBind(a, b, c, d, e, f, g, h);
}

GenericFn g_origPacketRead = nullptr;

void HookPacketRead(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  g_packetCalls.fetch_add(1, std::memory_order_relaxed);
  if (g_origPacketRead) g_origPacketRead(a, b, c, d, e, f, g, h);
}

// Installs one passthrough hook at a resolved vtable slot. Returns true on success.
bool InstallSlotHook(const char *typeName, std::size_t slot, GenericFn detour,
                     GenericFn *original, std::atomic_bool &flag, const char *label) {
  if (flag.exchange(true, std::memory_order_relaxed)) return true;
  if (slot == 0) {
    preloaderLogger.warn(
        "Cosmetics {}: no verified vtable slot for this build; staying on the resource-pack path",
        label);
    return false;
  }
  const std::uintptr_t target =
      pl::memory::resolveVtableFunction(typeName, slot, kGameModule);
  if (!target) {
    preloaderLogger.warn(
        "Cosmetics {}: {} slot {} unresolved; staying on the resource-pack path",
        label, typeName, slot);
    return false;
  }
  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target),
                       reinterpret_cast<pl::memory::FuncPtr>(detour),
                       reinterpret_cast<pl::memory::FuncPtr *>(original),
                       pl::memory::HookPriority::Normal, label) != 0) {
    preloaderLogger.warn("Cosmetics {}: hook install failed", label);
    return false;
  }
  preloaderLogger.info("Cosmetics {}: hooked {} slot {}", label, typeName, slot);
  return true;
}

std::size_t SkinCapeSlotFromRules() {
  return ReadConfiguredOptifineSlot("skinCapeVtableIndex");
}

std::size_t TextureBindSlotFromRules() {
  return ReadConfiguredOptifineSlot("textureBindVtableIndex");
}

std::size_t PacketReadSlotFromRules() {
  return ReadConfiguredOptifineSlot("packetReadVtableIndex");
}

// The engine image loader, resolved once from its byte signature. The class exports no symbol, so a
// pattern match is the only handle; a build whose pattern is absent leaves this 0 and the
// cosmetics path stays on the resource pack.
std::atomic<std::uintptr_t> g_imageLoader{0};
std::atomic_bool g_imageLoaderResolved{false};

std::uintptr_t ResolveImageLoader() {
  if (g_imageLoaderResolved.load(std::memory_order_relaxed)) {
    return g_imageLoader.load(std::memory_order_relaxed);
  }
  g_imageLoaderResolved.store(true, std::memory_order_relaxed);
  if (!GameHookRulesConfigured()) return 0;
  auto signatures = LoadConfiguredGameHookSignatures();
  if (!signatures || signatures->imageLoaderSig.empty()) return 0;
  std::vector<std::string> patterns{signatures->imageLoaderSig};
  auto results = pl::memory::resolveSignatures(patterns, kGameModule);
  auto it = results.find(signatures->imageLoaderSig);
  std::uintptr_t address = it == results.end() ? 0 : it->second;
  g_imageLoader.store(address, std::memory_order_relaxed);
  if (address) {
    preloaderLogger.info("Cosmetics image loader: mce::ImageUtils::loadImageFromMemory resolved");
  } else {
    preloaderLogger.warn(
        "Cosmetics image loader: signature did not match; the pack path stays in use");
  }
  return address;
}

} // namespace

void SetCapeOverride(std::uint64_t playerKey, const std::uint8_t *rgba,
                     std::uint32_t width, std::uint32_t height) {
  std::lock_guard<std::mutex> lock(g_mutex);
  StoreImage(g_capePixels, g_capeMeta, playerKey, rgba, width, height);
}

void ClearCapeOverrides() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_capePixels.clear();
  g_capeMeta.clear();
}

const CosmeticImage *FindCapeOverride(std::uint64_t playerKey) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Find(g_capeMeta, playerKey);
}

std::size_t CapeOverrideCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_capeMeta.size();
}

const CosmeticImage *FirstCapeOverride() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_capeMeta.empty()) return nullptr;
  return &g_capeMeta.begin()->second;
}

void SetTextureOverride(std::uint64_t textureId, const std::uint8_t *rgba,
                        std::uint32_t width, std::uint32_t height) {
  std::lock_guard<std::mutex> lock(g_mutex);
  StoreImage(g_texturePixels, g_textureMeta, textureId, rgba, width, height);
}

void ClearTextureOverrides() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_texturePixels.clear();
  g_textureMeta.clear();
}

const CosmeticImage *FindTextureOverride(std::uint64_t textureId) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Find(g_textureMeta, textureId);
}

std::size_t TextureOverrideCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_textureMeta.size();
}

void SetRenderGeometry(const std::uint8_t *data, std::size_t size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_geometry.assign(data, data + size);
  // FNV-1a, so a frame can cheaply tell the geometry changed without copying or comparing it.
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::uint8_t byte : g_geometry) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  g_geometryHash = hash;
}

void SetCosmeticFrame(const std::uint8_t *data, std::size_t size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (data == nullptr || size == 0) {
    g_cosmeticFrame.clear();
    return;
  }
  g_cosmeticFrame.assign(data, data + size);
}

std::size_t CosmeticFrameSize() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cosmeticFrame.size();
}

std::size_t CosmeticFrameBytes() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cosmeticFrame.size();
}

bool CosmeticFrameData(std::vector<std::uint8_t> &out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_cosmeticFrame.empty()) {
    out.clear();
    return false;
  }
  out = g_cosmeticFrame;
  return true;
}

CosmeticFrameHeader ReadCosmeticFrameHeader() {
  std::lock_guard<std::mutex> lock(g_mutex);
  CosmeticFrameHeader header;
  if (g_cosmeticFrame.size() < 8) return header;
  const std::uint8_t *p = g_cosmeticFrame.data();
  const std::uint32_t magic = static_cast<std::uint32_t>(p[0]) |
                              (static_cast<std::uint32_t>(p[1]) << 8) |
                              (static_cast<std::uint32_t>(p[2]) << 16) |
                              (static_cast<std::uint32_t>(p[3]) << 24);
  if (magic != kCosmeticFrameMagic) return header;
  header.valid = true;
  header.flags = static_cast<std::uint32_t>(p[4]) |
                 (static_cast<std::uint32_t>(p[5]) << 8) |
                 (static_cast<std::uint32_t>(p[6]) << 16) |
                 (static_cast<std::uint32_t>(p[7]) << 24);
  return header;
}

std::size_t RenderGeometrySize() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_geometry.size();
}

bool RenderGeometryData(std::vector<std::uint8_t> &out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_geometry.empty()) {
    out.clear();
    return false;
  }
  out = g_geometry;
  return true;
}

std::uint64_t RenderGeometryHash() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_geometryHash;
}

void InitCosmeticsHooks(std::size_t skinCapeVtableIndex, std::size_t textureBindVtableIndex,
                        std::size_t packetReadVtableIndex) {
  // Only install from a validated rule set, the same precondition the decision and render hooks
  // take: an unverified vtable index on an unstripped RTTI name resolves to a real but unrelated
  // function, and hooking it detours whatever that slot happens to be.
  if (!GameHookRulesConfigured()) {
    preloaderLogger.warn(
        "Cosmetics hooks: signature rules were not delivered; not installing cosmetics hooks");
    return;
  }
  std::size_t skinSlot = skinCapeVtableIndex != 0 ? skinCapeVtableIndex : SkinCapeSlotFromRules();
  if (skinSlot == 0) skinSlot = kDefaultSkinCapeSlot;
  std::size_t textureSlot =
      textureBindVtableIndex != 0 ? textureBindVtableIndex : TextureBindSlotFromRules();
  if (textureSlot == 0) textureSlot = kDefaultTextureBindSlot;
  std::size_t packetSlot =
      packetReadVtableIndex != 0 ? packetReadVtableIndex : PacketReadSlotFromRules();
  if (packetSlot == 0) packetSlot = kDefaultPacketReadSlot;

  InstallSlotHook(kSkinTypeName, skinSlot, HookSkinCape, &g_origSkinCape,
                  g_skinHookInstalled, "skin/cape");
  InstallSlotHook(kTextureTypeName, textureSlot, HookTextureBind, &g_origTextureBind,
                  g_textureHookInstalled, "texture-bind");
  InstallSlotHook(kPacketHandlerTypeName, packetSlot, HookPacketRead, &g_origPacketRead,
                  g_packetHookInstalled, "player-join-packet");

  // The engine image pipeline. This is the seam that fixes the memory-corruption/black-texture
  // failure: it verifies the recovered loader ABI (sret in x8, flag at +0x10) with a live probe and
  // only then detours the loader, so a custom cape is injected as a buffer the engine itself
  // allocated and owns. It is gated on the same signature rules as everything above and stays
  // fail-closed to the already-proven `SwapCapeImage` struct-swap path.
  InitMceImageHook();
}

bool IsPacketHookLive() {
  return g_packetCalls.load(std::memory_order_relaxed) != 0;
}

std::uintptr_t ImageLoaderAddress() {
  return ResolveImageLoader();
}

bool BuildCapeImageFromPng(const std::uint8_t *png, std::size_t size, void *outImage) {
  // Delegates to the corrected loader call in `mce_image_hook.cpp`. The earlier implementation here
  // declared the loader as an ordinary C function, which mapped its arguments to the wrong registers
  // (the real convention is `x0 = out`, `x1 = format`, ... with the return aggregate delivered
  // through `x8`) and never supplied `x8` at all -- the source of the memory corruption / black
  // texture. The hook module declares the return type so the compiler emits the correct ABI and is
  // the single place the loader is ever called.
  return BuildImageFromPng(png, size, outImage);
}

bool SwapCapeImage(void *skinRef, const void *image) {
  // The real pixel substitution: the cape the renderer samples is the mce::Image member of
  // SerializedSkinRef at +0xa8. The whole struct is copied rather than a pointer inside it poked,
  // because mce::Image's internal fields are not recoverable from the stripped binary — so no
  // assumption is made about where the buffer pointer sits.
  if (skinRef == nullptr || image == nullptr) return false;
  auto *destination = static_cast<std::uint8_t *>(skinRef) + skinlayout::kCapeImageData;
  std::memcpy(destination, image, skinlayout::kImageSize);
  return true;
}

bool IsSkinCapeHookLive() {
  return g_skinCapeCalls.load(std::memory_order_relaxed) != 0;
}

bool IsTextureHookLive() {
  return g_textureCalls.load(std::memory_order_relaxed) != 0;
}

bool ReadCosmeticsStats(std::uint32_t out[4]) {
  if (out == nullptr) return false;
  out[0] = g_skinCapeCalls.load(std::memory_order_relaxed);
  out[1] = g_textureCalls.load(std::memory_order_relaxed);
  out[2] = static_cast<std::uint32_t>(CapeOverrideCount());
  out[3] = static_cast<std::uint32_t>(TextureOverrideCount());
  return out[0] != 0 || out[1] != 0;
}

} // namespace pl::runtime
