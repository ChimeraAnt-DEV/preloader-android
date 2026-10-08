/**
 * @file CosmeticSyncJni.cpp
 * @brief JNI surface for the native cosmetic sync codec (Task 4).
 *
 * Lets the launcher use the native encoder/decoder for the LAN advertisement so the native module
 * and the Java protocol cannot drift — the same datagram the native module broadcasts is the one the
 * Java listener decodes. Fail-closed: a malformed datagram decodes to null rather than throwing.
 */

#include <jni.h>

#include <string>
#include <vector>

#include "pl/cosmetics/network/CosmeticSocketProtocol.hpp"

namespace {

std::string toStd(JNIEnv *env, jstring value) {
  if (value == nullptr) return {};
  const char *chars = env->GetStringUTFChars(value, nullptr);
  std::string result = chars != nullptr ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
  return result;
}

jstring toJava(JNIEnv *env, const std::string &value) {
  return env->NewStringUTF(value.c_str());
}

} // namespace

extern "C" {

/** @brief Encodes an advertisement datagram (byte-for-byte the Java CosmeticSyncProtocol). */
JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeEncodeCosmeticAdvert(
    JNIEnv *env, jclass, jstring peerId, jstring name, jstring capeId, jstring accessoryId,
    jstring petId) {
  const std::vector<std::uint8_t> packet = pl::cosmetics::network::CosmeticSocketProtocol::encodeAdvert(
      toStd(env, peerId), toStd(env, name), toStd(env, capeId), toStd(env, accessoryId),
      toStd(env, petId));
  jbyteArray result = env->NewByteArray(static_cast<jsize>(packet.size()));
  if (result != nullptr && !packet.empty()) {
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(packet.size()),
                            reinterpret_cast<const jbyte *>(packet.data()));
  }
  return result;
}

/**
 * @brief Decodes a datagram into `{type, peerId, name, capeId, accessoryId, petId}`, or null.
 */
JNIEXPORT jobjectArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeDecodeCosmeticAdvert(
    JNIEnv *env, jclass, jbyteArray data) {
  if (data == nullptr) return nullptr;
  const jsize length = env->GetArrayLength(data);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte *>(bytes.data()));

  auto advert = pl::cosmetics::network::CosmeticSocketProtocol::decode(bytes);
  if (!advert.has_value()) return nullptr;

  jclass stringClass = env->FindClass("java/lang/String");
  jobjectArray result = env->NewObjectArray(6, stringClass, nullptr);
  env->SetObjectArrayElement(result, 0, toJava(env, std::to_string(advert->type)));
  env->SetObjectArrayElement(result, 1, toJava(env, advert->peerId));
  env->SetObjectArrayElement(result, 2, toJava(env, advert->name));
  env->SetObjectArrayElement(result, 3, toJava(env, advert->capeId));
  env->SetObjectArrayElement(result, 4, toJava(env, advert->accessoryId));
  env->SetObjectArrayElement(result, 5, toJava(env, advert->petId));
  return result;
}

/** @brief The datagram magic, so the launcher can size its receive buffer / filter packets. */
JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeCosmeticSyncMagic(
    JNIEnv *env, jclass) {
  jbyteArray result = env->NewByteArray(2);
  const jbyte magic[2] = {static_cast<jbyte>(pl::cosmetics::network::kMagic[0]),
                          static_cast<jbyte>(pl::cosmetics::network::kMagic[1])};
  env->SetByteArrayRegion(result, 0, 2, magic);
  return result;
}

} // extern "C"
