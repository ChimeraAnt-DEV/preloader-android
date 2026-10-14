#pragma once

/**
 * @file Utf8.hpp
 * @brief Validation and repair of modified UTF-8 buffers.
 *
 * Pure C++ with no JNI types, so the rules that decide whether a buffer is safe to hand to the JVM
 * are host-unit-tested (see `tests/cosmetics/Utf8Test.cpp`). {@link pl::jni::NewString} wraps these
 * with the actual `NewStringUTF` call.
 */

#include <cstddef>
#include <string>
#include <string_view>

namespace pl::jni {

/** True for a byte that may continue a multi-byte sequence (0x80..0xBF). */
constexpr bool IsContinuation(unsigned char b) {
    return (b & 0xC0u) == 0x80u;
}

/**
 * Validates a buffer as modified UTF-8 (the encoding `JNIEnv::NewStringUTF` accepts).
 *
 * <p>Rejects: an embedded NUL, a lone continuation byte (the 0x87 case that aborted the process),
 * a lead byte with a missing or out-of-range continuation, an overlong two/three-byte form, the
 * never-valid lead bytes 0xC0/0xC1/0xF5..0xFF, and any 4-byte sequence (modified UTF-8 has none).
 * Surrogate halves encoded as three-byte sequences are accepted, because that is exactly how
 * modified UTF-8 represents them.
 */
inline bool IsValidModifiedUtf8(std::string_view text) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const std::size_t size = text.size();
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char b = bytes[i];
        if (b < 0x80u) {
            if (b == 0x00u) return false; // Modified UTF-8 has no embedded NUL.
            continue;
        }
        if (IsContinuation(b)) return false; // A continuation with no lead byte.
        if (b >= 0xC2u && b <= 0xDFu) {
            if (i + 1 >= size || !IsContinuation(bytes[i + 1])) return false;
            i += 1;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            if (i + 2 >= size) return false;
            if (!IsContinuation(bytes[i + 1]) || !IsContinuation(bytes[i + 2])) return false;
            // 0xE0 0x80..0x9F is an overlong encoding of a one-byte value.
            if (b == 0xE0u && bytes[i + 1] < 0xA0u) return false;
            i += 2;
        } else {
            // 0xC0, 0xC1 (overlong), 0xF0..0xF4 (4-byte, not in modified UTF-8) and 0xF5..0xFF.
            return false;
        }
    }
    return true;
}

/**
 * Rewrites a buffer into a valid modified UTF-8 string.
 *
 * <p>A well-formed buffer is returned unchanged (the common case allocates only the copy). A
 * malformed one has each offending byte replaced with '?' and every embedded NUL dropped, so the
 * result is always safe for `NewStringUTF`. Substitution, rather than truncation, keeps the
 * surrounding text readable — a path or a JSON payload stays diagnosable instead of being cut at
 * the first stray byte.
 */
inline std::string SanitizeToModifiedUtf8(std::string_view text) {
    if (IsValidModifiedUtf8(text)) return std::string(text);

    std::string out;
    out.reserve(text.size());
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const std::size_t size = text.size();
    for (std::size_t i = 0; i < size;) {
        const unsigned char b = bytes[i];
        if (b == 0x00u) {
            ++i; // Drop NUL: it would terminate the C string at NewStringUTF.
            continue;
        }
        if (b < 0x80u) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        std::size_t length = 0;
        if (b >= 0xC2u && b <= 0xDFu) {
            length = 2;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            length = 3;
        }
        bool ok = length != 0 && i + length <= size;
        if (ok) {
            for (std::size_t k = 1; k < length; ++k) {
                if (!IsContinuation(bytes[i + k])) { ok = false; break; }
            }
        }
        if (ok && b == 0xE0u && bytes[i + 1] < 0xA0u) ok = false; // Overlong.
        if (ok) {
            out.append(reinterpret_cast<const char *>(bytes + i), length);
            i += length;
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

} // namespace pl::jni
