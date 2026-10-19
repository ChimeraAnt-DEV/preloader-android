#include <jni.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "pl/cosmetics/NativeRenderHook.hpp"
#include "pl/render/GlesProbe.hpp"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/GameLocalPlayer.h"
#include "pl/runtime/GamePlayerRender.h"
#include "pl/runtime/ModuleMap.hpp"
#include "pl/runtime/mce_image_hook.hpp"

// The JNI surface for the native cosmetics registry. Java fills the cape/texture overrides and the
// render geometry here; the hooks read the same tables. Every entry is a thin copy in or a stat
// out, so nothing here blocks or allocates beyond the one deliberate pixel copy.

namespace {

// Copies a Java byte[] into a native buffer, or an empty vector when the array is null. The
// length is clamped to the declared size so a short array cannot over-read.
std::vector<std::uint8_t> CopyBytes(JNIEnv *env, jbyteArray array, std::size_t expected) {
  std::vector<std::uint8_t> out;
  if (array == nullptr) return out;
  const jsize length = env->GetArrayLength(array);
  const jsize usable = static_cast<jsize>(
      expected == 0 ? length : (length < expected ? length : expected));
  out.resize(expected == 0 ? static_cast<std::size_t>(length) : expected, 0);
  if (usable > 0) {
    env->GetByteArrayRegion(array, 0, usable, reinterpret_cast<jbyte *>(out.data()));
  }
  return out;
}

/** The raw IEEE-754 bits of a float, so a float can travel inside an int[] across JNI. */
std::uint32_t FloatBits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetCapeOverride(
    JNIEnv *env, jclass, jlong playerKey, jbyteArray rgba, jint width, jint height) {
  const std::size_t expected = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4U;
  std::vector<std::uint8_t> pixels = CopyBytes(env, rgba, expected);
  pl::runtime::SetCapeOverride(static_cast<std::uint64_t>(playerKey), pixels.data(),
                               static_cast<std::uint32_t>(width),
                               static_cast<std::uint32_t>(height));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearCapeOverrides(JNIEnv *, jclass) {
  pl::runtime::ClearCapeOverrides();
}

JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeCapeOverrideCount(JNIEnv *, jclass) {
  return static_cast<jint>(pl::runtime::CapeOverrideCount());
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetTextureOverride(
    JNIEnv *env, jclass, jlong textureId, jbyteArray rgba, jint width, jint height) {
  const std::size_t expected = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4U;
  std::vector<std::uint8_t> pixels = CopyBytes(env, rgba, expected);
  pl::runtime::SetTextureOverride(static_cast<std::uint64_t>(textureId), pixels.data(),
                                  static_cast<std::uint32_t>(width),
                                  static_cast<std::uint32_t>(height));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearTextureOverrides(JNIEnv *, jclass) {
  pl::runtime::ClearTextureOverrides();
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeRemoveCapeOverride(JNIEnv *, jclass,
                                                                            jlong playerKey) {
  pl::runtime::RemoveCapeOverride(static_cast<std::uint64_t>(playerKey));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeRemoveTextureOverride(JNIEnv *, jclass,
                                                                               jlong textureId) {
  pl::runtime::RemoveTextureOverride(static_cast<std::uint64_t>(textureId));
}

JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeTextureOverrideCount(JNIEnv *, jclass) {
  return static_cast<jint>(pl::runtime::TextureOverrideCount());
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetRenderGeometry(
    JNIEnv *env, jclass, jbyteArray data) {
  std::vector<std::uint8_t> bytes = CopyBytes(env, data, 0);
  pl::runtime::SetRenderGeometry(bytes.data(), bytes.size());
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativePushCosmeticFrame(
    JNIEnv *env, jclass, jbyteArray frame) {
  std::vector<std::uint8_t> bytes = CopyBytes(env, frame, 0);
  pl::runtime::SetCosmeticFrame(bytes.data(), bytes.size());
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSwapCapeImage(
    JNIEnv *env, jclass, jlong skinRefAddress, jbyteArray imageBytes) {
  if (skinRefAddress == 0 || imageBytes == nullptr) return JNI_FALSE;
  const jsize length = env->GetArrayLength(imageBytes);
  // The image struct must be at least the verified size; copy exactly that many bytes.
  constexpr jsize kImageSize = 0x30;
  if (length < kImageSize) return JNI_FALSE;
  std::vector<std::uint8_t> image(static_cast<std::size_t>(kImageSize), 0);
  env->GetByteArrayRegion(imageBytes, 0, kImageSize,
                          reinterpret_cast<jbyte *>(image.data()));
  const bool ok = pl::runtime::SwapCapeImage(
      reinterpret_cast<void *>(static_cast<std::uintptr_t>(skinRefAddress)),
      image.data());
  return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeImageLoaderAddress(JNIEnv *, jclass) {
  return static_cast<jlong>(pl::runtime::ImageLoaderAddress());
}

/**
 * Builds a valid engine `mce::Image` from PNG bytes and returns it as a 0x30-byte array, or null
 * when the image loader is unresolved or the engine rejected the bytes.
 */
JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeBuildCapeImage(
    JNIEnv *env, jclass, jbyteArray pngBytes) {
  if (pngBytes == nullptr) return nullptr;
  const jsize length = env->GetArrayLength(pngBytes);
  if (length <= 0) return nullptr;
  std::vector<std::uint8_t> png(static_cast<std::size_t>(length));
  env->GetByteArrayRegion(pngBytes, 0, length, reinterpret_cast<jbyte *>(png.data()));

  // mce::Image is exactly 0x30 bytes (verified from the SerializedSkinRef accessors).
  constexpr std::size_t kImageSize = 0x30;
  std::vector<std::uint8_t> image(kImageSize, 0);
  if (!pl::runtime::BuildCapeImageFromPng(png.data(), png.size(), image.data())) {
    return nullptr;
  }
  jbyteArray out = env->NewByteArray(static_cast<jsize>(kImageSize));
  if (out == nullptr) return nullptr;
  env->SetByteArrayRegion(out, 0, static_cast<jsize>(kImageSize),
                          reinterpret_cast<const jbyte *>(image.data()));
  return out;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsCosmeticsHookLive(JNIEnv *, jclass) {
  return (pl::runtime::IsSkinCapeHookLive() || pl::runtime::IsTextureHookLive())
             ? JNI_TRUE : JNI_FALSE;
}

// The per-piece local atlases. Java hands each piece its own texture so the cape, hat and pet are
// not all drawn with the cape's artwork.
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetPieceAtlas(
    JNIEnv *env, jclass, jint piece, jbyteArray rgba, jint width, jint height) {
  if (piece < 0 || piece > 2) return;
  const auto kind = static_cast<pl::runtime::CosmeticPiece>(piece);
  if (rgba == nullptr || width <= 0 || height <= 0) {
    pl::runtime::SetPieceAtlas(kind, nullptr, 0, 0);
    return;
  }
  const std::size_t expected = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4U;
  std::vector<std::uint8_t> pixels = CopyBytes(env, rgba, expected);
  pl::runtime::SetPieceAtlas(kind, pixels.data(), static_cast<std::uint32_t>(width),
                             static_cast<std::uint32_t>(height));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearPieceAtlases(JNIEnv *, jclass) {
  pl::runtime::SetPieceAtlas(pl::runtime::CosmeticPiece::Cape, nullptr, 0, 0);
  pl::runtime::SetPieceAtlas(pl::runtime::CosmeticPiece::Hat, nullptr, 0, 0);
  pl::runtime::SetPieceAtlas(pl::runtime::CosmeticPiece::Pet, nullptr, 0, 0);
}

JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadCosmeticsStats(JNIEnv *env, jclass) {
  std::uint32_t stats[4] = {0, 0, 0, 0};
  const bool live = pl::runtime::ReadCosmeticsStats(stats);
  jintArray out = env->NewIntArray(4);
  if (out == nullptr) return nullptr;
  jint values[4] = {
      static_cast<jint>(stats[0]), static_cast<jint>(stats[1]),
      static_cast<jint>(stats[2]), static_cast<jint>(stats[3])};
  if (!live) {
    // No hook fired: report -1 calls so the caller can tell "unavailable" from "zero calls".
    values[0] = -1;
  }
  env->SetIntArrayRegion(out, 0, 4, values);
  return out;
}

// Item 3: the skin-path seam observation. Returns {stage, packetHookInstalled, packetCalls,
// textureCalls, skinCaptured}. The stage is the honest classification: the payload fields are not
// readable in this build, so the report names the stage reached, not a skin id it never read.
JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadSkinSeam(JNIEnv *env, jclass) {
  std::uint32_t seam[5] = {0, 0, 0, 0, 0};
  if (!pl::runtime::ReadSkinSeam(seam)) return nullptr;
  jint values[5];
  for (int i = 0; i < 5; ++i) values[i] = static_cast<jint>(seam[i]);
  jintArray out = env->NewIntArray(5);
  if (out == nullptr) return nullptr;
  env->SetIntArrayRegion(out, 0, 5, values);
  return out;
}

// The one-line explanation for a stage, so the wording lives in one place (the native rule) and the
// launcher does not paraphrase it.
JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeDescribeSkinSeam(JNIEnv *env, jclass,
                                                                         jint stage) {
  const char *text = pl::runtime::DescribeSkinSeamStage(static_cast<std::uint32_t>(stage));
  return env->NewStringUTF(text == nullptr ? "" : text);
}

// ---------------------------------------------------------------------------------------------
// The mce::Image pipeline surface
//
// These bind the *implemented* image-buffer substitution (see mce_image_hook.cpp) that a launcher
// drives from Java. Each entry is a thin copy in or a stat out, and every call stays fail-closed:
// without the engine loader resolved, the calls are no-ops that report "not installed" rather than
// claiming a substitution that never ran. This is what lets the launcher hand the game a custom
// cape *at the moment the engine uploads a texture* — the one seam where a launcher-built pixel
// buffer is safe, because the engine still owns the resulting image's lifetime.

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetContentSubstitution(
    JNIEnv *env, jclass, jbyteArray sourceBytes, jbyteArray rgba, jint width, jint height) {
  const std::size_t expected = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4U;
  if (sourceBytes == nullptr) return;
  const jsize sourceLength = env->GetArrayLength(sourceBytes);
  if (sourceLength <= 0) return;
  std::vector<std::uint8_t> source(static_cast<std::size_t>(sourceLength));
  env->GetByteArrayRegion(sourceBytes, 0, sourceLength, reinterpret_cast<jbyte *>(source.data()));
  if (rgba == nullptr || width <= 0 || height <= 0 || expected == 0) {
    pl::runtime::SetContentSubstitution(source.data(), source.size(), nullptr, 0, 0);
    return;
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(expected), 0);
  const jsize usable = static_cast<jsize>(expected) < env->GetArrayLength(rgba)
                           ? static_cast<jsize>(expected)
                           : env->GetArrayLength(rgba);
  env->GetByteArrayRegion(rgba, 0, usable, reinterpret_cast<jbyte *>(pixels.data()));
  pl::runtime::SetContentSubstitution(source.data(), source.size(), pixels.data(),
                                      static_cast<std::uint32_t>(width),
                                      static_cast<std::uint32_t>(height));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearContentSubstitutions(JNIEnv *, jclass) {
  pl::runtime::ClearContentSubstitutions();
}

JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSubstitutionCount(JNIEnv *, jclass) {
  return static_cast<jint>(pl::runtime::SubstitutionCount());
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeArmNextImageOverride(
    JNIEnv *env, jclass, jbyteArray rgba, jint width, jint height) {
  const std::size_t expected = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4U;
  if (rgba == nullptr || width <= 0 || height <= 0 || expected == 0) {
    pl::runtime::ClearNextImageOverride();
    return;
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(expected), 0);
  const jsize usable = static_cast<jsize>(expected) < env->GetArrayLength(rgba)
                           ? static_cast<jsize>(expected)
                           : env->GetArrayLength(rgba);
  env->GetByteArrayRegion(rgba, 0, usable, reinterpret_cast<jbyte *>(pixels.data()));
  pl::runtime::ArmNextImageOverride(pixels.data(), static_cast<std::uint32_t>(width),
                                    static_cast<std::uint32_t>(height));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearNextImageOverride(JNIEnv *, jclass) {
  pl::runtime::ClearNextImageOverride();
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsMceImageHookLive(JNIEnv *, jclass) {
  return pl::runtime::IsMceImageHookLive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsImagePathVerified(JNIEnv *, jclass) {
  return pl::runtime::IsImagePathVerified() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadMceImageHookStats(JNIEnv *env, jclass) {
  std::uint32_t stats[4] = {0, 0, 0, 0};
  const bool live = pl::runtime::ReadMceImageHookStats(stats);
  jintArray out = env->NewIntArray(4);
  if (out == nullptr) return nullptr;
  jint values[4] = {
      static_cast<jint>(stats[0]), static_cast<jint>(stats[1]),
      static_cast<jint>(stats[2]), static_cast<jint>(stats[3])};
  if (!live) values[0] = -1;
  env->SetIntArrayRegion(out, 0, 4, values);
  return out;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsTextureCacheFlushAvailable(JNIEnv *,
                                                                                       jclass) {
  return pl::runtime::IsTextureCacheFlushAvailable() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeRequestTextureCacheFlush(JNIEnv *, jclass) {
  return pl::runtime::RequestTextureCacheFlush() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadRenderDiagnostics(JNIEnv *env, jclass) {
  // 17 fields. The first 9 keep their original meaning so an older report reader still works; the
  // additions (indices 9..16) are the rules/install/camera/library-mapping/triangle state the
  // Stage-1/4/5/6 fixes added. -1 means "unknown / not available".
  constexpr int kFields = 17;
  jint values[kFields];
  for (int i = 0; i < kFields; ++i) values[i] = -1;

  // Slot used and how it was resolved.
  std::size_t slot = 0;
  bool byString = false;
  if (pl::runtime::LastResolvedRenderSlot(slot, byString)) {
    values[0] = static_cast<jint>(slot);
    values[1] = byString ? 1 : 0;
  } else {
    values[1] = -1;
  }

  // GL backend and whether a context is current.
  values[2] = static_cast<jint>(pl::render::GlesBackend());
  values[3] = pl::render::IsEglContextCurrent() ? 1 : 0;

  // Hook installed / frames seen, from the player-render feed.
  std::uint32_t stats[4] = {0, 0, 0, 0};
  const bool live = pl::runtime::ReadPlayerRenderStats(stats);
  values[4] = pl::cosmetics::IsNativeCosmeticRenderHookLive() ? 1 : 0;
  values[5] = live ? static_cast<jint>(stats[0]) : -1;

  values[6] = pl::render::LastGlesError();

  std::size_t geometryBytes = pl::runtime::RenderGeometrySize();
  values[7] = geometryBytes > 0 ? static_cast<jint>(geometryBytes) : -1;

  values[8] = pl::runtime::PieceAtlasMask();

  // Index 9 used to carry the GLES rasteriser's last draw decision. That rasteriser is retired (the
  // cape reaches the game through the engine texture swap), so the field reads "unknown".
  values[9] = -1;

  // Whether the local-player feed produced a position this frame (the cosmetics' local scope).
  float probePosition[3] = {0.0F, 0.0F, 0.0F};
  values[10] = pl::runtime::ReadLocalPlayerPosition(probePosition) ? 1 : 0;

  // The rules revision, how many extra rules matched, and whether the effective rule carries the
  // render slot. These are the fields whose loss silently disabled the hook.
  values[11] = static_cast<jint>(pl::runtime::GameHookRulesRevision());
  values[12] = pl::runtime::GameHookRulesMergeConflicts();
  values[13] = pl::runtime::GameHookRulesHaveRenderIndex() ? 1 : 0;
  values[14] = pl::runtime::GameHookRulesLoadAttempted() ? 1 : 0;

  // How many times `libpreloader.so` is mapped into this process. A second copy has its own empty
  // registry, so Java would publish to one and the hook would draw from another — the count is the
  // cheapest proof there is only one.
  values[15] = pl::runtime::CountModuleMappings("libpreloader.so");

  // Index 16 used to carry the GLES rasteriser's triangle count; retired with the rasteriser.
  values[16] = -1;

  jintArray out = env->NewIntArray(kFields);
  if (out == nullptr) return nullptr;
  env->SetIntArrayRegion(out, 0, kFields, values);
  return out;
}

// Item 18: the exact gate that refused the render hook (0 = live, 1 = version gate, 2 = slot
// unresolved, 3 = not executable, 4 = hook() failed), so the diagnostics report names the failure.
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeRenderHookFailureReason(JNIEnv *, jclass) {
  return static_cast<jint>(pl::runtime::RenderHookFailureReason());
}

// Item 11: the display refresh rate the launcher resolved, so the frame-pacing reading can decide
// whether the render thread is vsync-locked. 0 means unknown.
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetDisplayRefreshHz(JNIEnv *, jclass,
                                                                             jint hz) {
  pl::runtime::SetDisplayRefreshHz(static_cast<int>(hz));
}

// Item 11: the render-thread frame-pacing reading, as {valid, averageUs, worstUs, flags} where
// flags bit0 = jittering, bit1 = vsync-locked. A short array (length 1) means "no reading yet".
JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadFramePacing(JNIEnv *env, jclass) {
  long long averageUs = 0;
  long long worstUs = 0;
  bool jittering = false;
  bool vsyncLocked = false;
  const bool valid =
      pl::runtime::ReadFramePacing(averageUs, worstUs, jittering, vsyncLocked);
  jint values[4];
  values[0] = valid ? 1 : 0;
  values[1] = valid ? static_cast<jint>(averageUs) : -1;
  values[2] = valid ? static_cast<jint>(worstUs) : -1;
  values[3] = (jittering ? 1 : 0) | (vsyncLocked ? 2 : 0);
  jintArray out = env->NewIntArray(4);
  if (out == nullptr) return nullptr;
  env->SetIntArrayRegion(out, 0, 4, values);
  return out;
}

// The local-player feed: {live, xBits, yBits, zBits, yawBits, pitchBits}. Floats are returned as
// their raw int bits so a single int[] crosses JNI; the caller decodes with Float.intBitsToFloat.
JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadLocalPlayerFeed(JNIEnv *env, jclass) {
  float position[3] = {0.0F, 0.0F, 0.0F};
  float rotation[2] = {0.0F, 0.0F};
  const bool live = pl::runtime::ReadLocalPlayerPosition(position);
  pl::runtime::ReadLocalPlayerRotation(rotation);

  jint values[6];
  values[0] = live ? 1 : 0;
  auto bits = [](float value) { return static_cast<jint>(FloatBits(value)); };
  values[1] = bits(position[0]);
  values[2] = bits(position[1]);
  values[3] = bits(position[2]);
  values[4] = bits(rotation[0]);
  values[5] = bits(rotation[1]);

  jintArray out = env->NewIntArray(6);
  if (out == nullptr) return nullptr;
  env->SetIntArrayRegion(out, 0, 6, values);
  return out;
}

// The camera anchor source for the last frame: 1 = the engine view-projection resolved, 0 = the
// first-person fallback was used. A pass/fail device test must never silently accept the fallback.
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeCameraAnchorSource(JNIEnv *, jclass) {
  return pl::cosmetics::LastCameraAnchorFromEngine() ? 1 : 0;
}


// Returns the captured base-skin texture as {rgba bytes, width, height, format} in one byte[]:
// a 16-byte little-endian header (width, height, format, stampMs) followed by the payload. Null
// when no base-skin-shaped upload has been seen. The launcher decodes it (PNG or raw RGBA) and
// renders it as the player's skin.
JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadCapturedSkin(JNIEnv *env, jclass) {
  // One locked pass: the copy returns the payload size, so no separate (racy) size query is needed
  // and the read allocates exactly the payload.
  constexpr std::size_t kMaxPayload = 512U * 512U * 4U;
  std::vector<std::uint8_t> buffer(kMaxPayload, 0);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::int64_t stampMs = 0;
  std::size_t payloadSize = 0;
  if (!pl::runtime::CopyCapturedSkin(buffer.data(), buffer.size(), &width, &height, &format,
                                     &stampMs, &payloadSize)
      || payloadSize == 0 || payloadSize > kMaxPayload) {
    return nullptr;
  }

  constexpr std::size_t kHeader = 16;
  std::vector<std::uint8_t> out(kHeader + payloadSize, 0);
  auto put32 = [&out](std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
  };
  put32(0, width);
  put32(4, height);
  put32(8, format);
  put32(12, static_cast<std::uint32_t>(stampMs & 0xFFFFFFFFu));
  std::memcpy(out.data() + kHeader, buffer.data(), payloadSize);

  jbyteArray result = env->NewByteArray(static_cast<jsize>(out.size()));
  if (result == nullptr) return nullptr;
  env->SetByteArrayRegion(result, 0, static_cast<jsize>(out.size()),
                          reinterpret_cast<const jbyte *>(out.data()));
  return result;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeHasCapturedSkin(JNIEnv *, jclass) {
  return pl::runtime::HasCapturedSkin() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearCapturedSkin(JNIEnv *, jclass) {
  pl::runtime::ClearCapturedSkin();
}

// The cape route. The captured cape is the local player's vanilla cape content; registering a custom
// cape against it makes the loader substitute the local cape's pixels for the custom ones, which is
// what makes a cape visible in third person on a Vulkan build (no GL draw is involved).
JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadCapturedCape(JNIEnv *env, jclass) {
  constexpr std::size_t kMaxPayload = 1024U * 1024U;
  std::vector<std::uint8_t> buffer(kMaxPayload, 0);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::int64_t stampMs = 0;
  std::size_t payloadSize = 0;
  if (!pl::runtime::CopyCapturedCape(buffer.data(), buffer.size(), &width, &height, &format,
                                     &stampMs, &payloadSize)
      || payloadSize == 0 || payloadSize > kMaxPayload) {
    return nullptr;
  }

  constexpr std::size_t kHeader = 16;
  std::vector<std::uint8_t> out(kHeader + payloadSize, 0);
  auto put32 = [&out](std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
  };
  put32(0, width);
  put32(4, height);
  put32(8, format);
  put32(12, static_cast<std::uint32_t>(stampMs & 0xFFFFFFFFu));
  std::memcpy(out.data() + kHeader, buffer.data(), payloadSize);

  jbyteArray result = env->NewByteArray(static_cast<jsize>(out.size()));
  if (result == nullptr) return nullptr;
  env->SetByteArrayRegion(result, 0, static_cast<jsize>(out.size()),
                          reinterpret_cast<const jbyte *>(out.data()));
  return result;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeHasCapturedCape(JNIEnv *, jclass) {
  return pl::runtime::HasCapturedCape() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearCapturedCape(JNIEnv *, jclass) {
  pl::runtime::ClearCapturedCape();
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetCapeSubstitution(
    JNIEnv *env, jclass, jbyteArray rgba, jint width, jint height) {
  if (rgba == nullptr || width <= 0 || height <= 0) {
    pl::runtime::ClearCapeSubstitution();
    return;
  }
  const jsize length = env->GetArrayLength(rgba);
  const std::size_t expected =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  if (length <= 0 || static_cast<std::size_t>(length) < expected) return;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(length), 0);
  env->GetByteArrayRegion(rgba, 0, length, reinterpret_cast<jbyte *>(pixels.data()));
  pl::runtime::SetCapeSubstitution(pixels.data(), static_cast<std::uint32_t>(width),
                                   static_cast<std::uint32_t>(height));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearCapeSubstitution(JNIEnv *, jclass) {
  pl::runtime::ClearCapeSubstitution();
}

JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeCapeSubstitutionCount(JNIEnv *, jclass) {
  return static_cast<jint>(pl::runtime::CapeSubstitutionCount());
}

JNIEXPORT jlongArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadCapeCaptureDiagnostics(JNIEnv *env,
                                                                                    jclass) {
  std::uint64_t out[4] = {0, 0, 0, 0};
  pl::runtime::ReadCapeCaptureDiagnostics(out);
  jlongArray result = env->NewLongArray(4);
  if (result == nullptr) return nullptr;
  jlong values[4] = {static_cast<jlong>(out[0]), static_cast<jlong>(out[1]),
                     static_cast<jlong>(out[2]), static_cast<jlong>(out[3])};
  env->SetLongArrayRegion(result, 0, 4, values);
  return result;
}

} // extern "C"
