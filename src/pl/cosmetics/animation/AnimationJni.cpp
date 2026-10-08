/**
 * @file AnimationJni.cpp
 * @brief JNI surface for the native cape chain (Task 3).
 *
 * Exposes the native {@link pl::cosmetics::animation::CapeMotion} so the launcher's preview and the
 * native render sample the *same* curve instead of each carrying its own copy of the amplitudes.
 * Fail-closed: a bad argument count returns an empty array, which the caller treats as "no native
 * sample" and falls back to the Java curve.
 */

#include <jni.h>

#include <algorithm>

#include "pl/cosmetics/animation/AnimationSolver.hpp"

extern "C" {

/**
 * @brief Samples the per-segment cape lean/sway.
 *
 * Returns a `float[segments * 2]` flattened as `{lean1, sway1, lean2, sway2, ...}` (degrees), or an
 * empty array when `segments` is not positive.
 */
JNIEXPORT jfloatArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSampleCapeChain(
    JNIEnv *env, jclass, jdouble moveSpeed, jboolean jumping, jdouble verticalSpeed,
    jdouble distanceMoved, jdouble capeFlap, jdouble bodyYawDegrees, jint segments) {
  if (segments <= 0) {
    return env->NewFloatArray(0);
  }
  const int count = std::min(static_cast<int>(segments), 64);
  jfloatArray result = env->NewFloatArray(count * 2);
  if (result == nullptr) {
    return nullptr;
  }
  float values[128];
  for (int i = 0; i < count; ++i) {
    const int index = i + 1;
    values[i * 2 + 0] = static_cast<float>(
        pl::cosmetics::animation::CapeMotion::segmentLeanDegrees(
            index, count, moveSpeed, jumping != 0, verticalSpeed, distanceMoved, capeFlap));
    values[i * 2 + 1] = static_cast<float>(
        pl::cosmetics::animation::CapeMotion::segmentSwayDegrees(index, count, moveSpeed,
                                                                 distanceMoved, bodyYawDegrees));
  }
  env->SetFloatArrayRegion(result, 0, count * 2, values);
  return result;
}

} // extern "C"
