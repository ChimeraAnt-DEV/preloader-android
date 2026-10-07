#include <jni.h>

#include <cstdint>
#include <vector>

#include "pl/runtime/GameCosmetics.h"

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

} // extern "C"
