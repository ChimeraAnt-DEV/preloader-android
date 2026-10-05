#include <jni.h>

#include <cstdint>
#include <string>

#include "pl/runtime/OptifineMode.h"

namespace {

std::string ToStdString(JNIEnv *env, jstring value) {
  if (value == nullptr) return {};
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) return {};
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

jobjectArray ToStateArray(JNIEnv *env, const pl::runtime::OptifineState &state) {
  const jclass stringClass = env->FindClass("java/lang/String");
  if (stringClass == nullptr) return nullptr;

  const int count = static_cast<int>(pl::runtime::OptifineItem::Count);
  jobjectArray result = env->NewObjectArray(count * 6, stringClass, nullptr);
  if (result == nullptr) return nullptr;

  int slot = 0;
  auto put = [&](const std::string &text) {
    jstring value = env->NewStringUTF(text.c_str());
    if (value != nullptr) {
      env->SetObjectArrayElement(result, slot, value);
      env->DeleteLocalRef(value);
    }
    ++slot;
  };

  for (int i = 0; i < count; ++i) {
    const pl::runtime::OptifineItemState &item = state.items[i];
    put(std::string(pl::runtime::OptifineItemId(static_cast<pl::runtime::OptifineItem>(i))));
    put(item.enabled ? "1" : "0");
    put(item.tier2 ? "1" : "0");
    put(std::to_string(item.status));
    put(item.detail);
    put(item.needsRestart ? "1" : "0");
  }
  return result;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeConfigureOptifineMode(
    JNIEnv *env, jclass clazz, jstring blob) {
  (void)clazz;
  pl::runtime::ConfigureOptifineMode(ToStdString(env, blob));
}

JNIEXPORT jobjectArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadOptifineState(
    JNIEnv *env, jclass clazz) {
  (void)clazz;
  // The read is guarded, but a miss still reaches here as a null return and the caller's
  // for-each would NPE. Return an empty array (not null) so the Settings UI renders with
  // defaults even if the native state has not been populated yet.
  jobjectArray state = ToStateArray(env, pl::runtime::ReadOptifineState());
  if (state != nullptr) {
    return state;
  }
  const jclass stringClass = env->FindClass("java/lang/String");
  if (stringClass == nullptr) {
    return nullptr;
  }
  return env->NewObjectArray(0, stringClass, nullptr);
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsOptifineModeActive(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::IsOptifineModeActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetOptifineRefreshTarget(
    JNIEnv *env, jclass clazz, jint hz) {
  (void)env;
  (void)clazz;
  pl::runtime::SetOptifineRefreshRateTarget(static_cast<int>(hz));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeConfigureRenderDistance(
    JNIEnv *env, jclass clazz, jint minDistance, jint maxDistance, jint fpsThreshold) {
  (void)env;
  (void)clazz;
  pl::runtime::ConfigureOptifineRenderDistance(static_cast<int>(minDistance),
                                               static_cast<int>(maxDistance),
                                               static_cast<int>(fpsThreshold));
}

JNIEXPORT jlong JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOptifineTickCount(JNIEnv *env,
                                                                           jclass clazz) {
  (void)env;
  (void)clazz;
  return static_cast<jlong>(pl::runtime::OptifineTickCount());
}

JNIEXPORT jlong JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOptifineHudUpdateCount(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return static_cast<jlong>(pl::runtime::OptifineHudUpdateCount());
}

} // extern "C"
