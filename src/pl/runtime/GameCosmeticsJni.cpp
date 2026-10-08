#include <jni.h>

#include <cstdint>
#include <vector>

#include "pl/runtime/GameCosmetics.h"
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

} // extern "C"
