#pragma once

/**
 * @file SafeJni.hpp
 * @brief Byte-safe helpers for turning native text into JVM strings.
 *
 * The JVM's `JNIEnv::NewStringUTF` expects <em>modified UTF-8</em>: no embedded NUL, no 4-byte
 * sequences, no overlong encodings, and every multi-byte lead byte followed by the right number of
 * continuation bytes. A buffer that violates any of those rules is not rejected gracefully — the
 * ART verifier aborts the process with
 * `JNI DETECTED ERROR IN APPLICATION: illegal start byte ...` and the game dies. The crash the
 * launcher hit came from feeding a raw buffer straight into `NewStringUTF`: an 0x87 byte (a
 * continuation byte with no lead) arrived in a path/JSON string and the check fired.
 *
 * Every native string built from game, file-system or network data must therefore go through one of
 * these helpers rather than calling `NewStringUTF` directly. They are fail-closed:
 * {@link NewString} always returns a valid string or nullptr, never an abort.
 *
 * The encoding rules live in {@link Utf8.hpp} (pure, host-tested); this header adds the JNI calls.
 */

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "pl/runtime/Utf8.hpp"

namespace pl::jni {

/**
 * Creates a JVM string from native bytes, validating the encoding first.
 *
 * <p>This is the drop-in replacement for `env->NewStringUTF(text.c_str())`. It never aborts the
 * process: a malformed buffer is sanitized before it reaches the JVM. Returns nullptr only when
 * the JNI call itself fails (e.g. an exhausted local reference table).
 */
inline jstring NewString(JNIEnv *env, std::string_view text) {
    if (env == nullptr) return nullptr;
    const std::string safe = SanitizeToModifiedUtf8(text);
    return env->NewStringUTF(safe.c_str());
}

/** Convenience overload for a NUL-terminated C string. */
inline jstring NewString(JNIEnv *env, const char *text) {
    if (text == nullptr) return nullptr;
    return NewString(env, std::string_view(text));
}

/**
 * Binary-safe path: builds the string from a `byte[]` through `new String(bytes, "UTF-8")`.
 *
 * <p>Use this when the payload is genuinely arbitrary bytes (a file name read from a filesystem
 * whose encoding is unknown, or a binary blob) and substitution would lose information. It costs
 * two extra JNI calls, so {@link NewString} remains the default for ordinary text.
 */
inline jstring NewStringFromBytes(JNIEnv *env, const std::uint8_t *bytes, std::size_t size) {
    if (env == nullptr) return nullptr;
    if (bytes == nullptr || size == 0) return env->NewStringUTF("");

    jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
    if (array == nullptr) return nullptr;
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                            reinterpret_cast<const jbyte *>(bytes));

    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) {
        env->DeleteLocalRef(array);
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    jstring result = nullptr;
    if (ctor != nullptr && charset != nullptr) {
        result = static_cast<jstring>(env->NewObject(stringClass, ctor, array, charset));
    }
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(stringClass);
    env->DeleteLocalRef(array);
    return result;
}

} // namespace pl::jni
