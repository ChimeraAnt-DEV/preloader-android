/**
 * @file Utf8Test.cpp
 * @brief Host test for the modified-UTF-8 validation that guards `NewStringUTF`.
 *
 * The crash this fixes was `JNI DETECTED ERROR IN APPLICATION: illegal start byte` — a raw buffer
 * with an 0x87 byte went straight into `NewStringUTF`. These checks pin the exact rules: what is
 * accepted, what is rejected, and that the sanitizer always produces a valid buffer.
 */

#include <cassert>
#include <cstdio>
#include <string>

#include "pl/runtime/Utf8.hpp"

using pl::jni::IsValidModifiedUtf8;
using pl::jni::SanitizeToModifiedUtf8;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  // Plain ASCII and ordinary UTF-8 text pass through unchanged.
  expect(IsValidModifiedUtf8("hello/world.apk"), "plain ASCII is valid");
  expect(IsValidModifiedUtf8(""), "empty string is valid");
  expect(IsValidModifiedUtf8("caf\xc3\xa9"), "2-byte UTF-8 (e-acute) is valid");
  expect(IsValidModifiedUtf8("\xe2\x9c\x93"), "3-byte UTF-8 (check mark) is valid");
  expect(SanitizeToModifiedUtf8("caf\xc3\xa9") == "caf\xc3\xa9",
         "valid text is returned unchanged");

  // The 0x87 byte that aborted the process: a continuation byte with no lead byte.
  expect(!IsValidModifiedUtf8(std::string("path\x87name")),
         "lone 0x87 continuation byte is rejected");
  expect(!IsValidModifiedUtf8(std::string("\x80")), "lone 0x80 continuation byte is rejected");
  expect(!IsValidModifiedUtf8(std::string("\xbf")), "lone 0xbf continuation byte is rejected");

  // A high-bit byte embedded in a real path, the reported crash shape.
  const std::string crashedPath = std::string("/storage/emulated/0/\x87/minecraft.apk");
  expect(!IsValidModifiedUtf8(crashedPath), "high-bit byte in a path is rejected");
  const std::string repaired = SanitizeToModifiedUtf8(crashedPath);
  expect(IsValidModifiedUtf8(repaired), "repaired path is valid");
  expect(repaired == "/storage/emulated/0/?/minecraft.apk",
         "the stray byte becomes a visible placeholder");

  // Embedded NUL: NewStringUTF would truncate the C string, so it is dropped.
  expect(!IsValidModifiedUtf8(std::string("a\0b", 3)), "embedded NUL is rejected");
  expect(SanitizeToModifiedUtf8(std::string("a\0b", 3)) == "ab", "embedded NUL is dropped");

  // Truncated multi-byte sequences.
  expect(!IsValidModifiedUtf8(std::string("caf\xc3")), "2-byte lead with no continuation");
  expect(!IsValidModifiedUtf8(std::string("\xe2\x9c")), "3-byte sequence cut short");
  expect(!IsValidModifiedUtf8(std::string("\xe2" "A")), "3-byte lead followed by ASCII");

  // Overlong encodings.
  expect(!IsValidModifiedUtf8(std::string("\xc0\xaf")), "overlong 2-byte form (0xC0) rejected");
  expect(!IsValidModifiedUtf8(std::string("\xc1\xbf")), "overlong 2-byte form (0xC1) rejected");
  expect(!IsValidModifiedUtf8(std::string("\xe0\x80\xaf")), "overlong 3-byte form rejected");

  // 4-byte sequences are not part of modified UTF-8.
  expect(!IsValidModifiedUtf8(std::string("\xf0\x9f\x98\x80")), "4-byte emoji rejected");

  // Never-valid lead bytes.
  expect(!IsValidModifiedUtf8(std::string("\xf5\x80\x80\x80")), "0xF5 lead rejected");
  expect(!IsValidModifiedUtf8(std::string("\xff")), "0xFF lead rejected");
  expect(!IsValidModifiedUtf8(std::string("\xfe")), "0xFE lead rejected");

  // A surrogate pair encoded as two 3-byte sequences is what modified UTF-8 actually uses, and is
  // therefore valid (this is the emoji form NewStringUTF expects).
  expect(IsValidModifiedUtf8(std::string("\xed\xa0\xbd\xed\xb8\x80")),
         "surrogate pair encoded as two 3-byte sequences is valid");

  // Sanitizing arbitrary binary must always yield something NewStringUTF accepts.
  std::string binary;
  for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
  const std::string safe = SanitizeToModifiedUtf8(binary);
  expect(IsValidModifiedUtf8(safe), "sanitized arbitrary binary is always valid");

  // A long run of stray bytes does not grow unboundedly or lose the readable parts.
  const std::string noisy = std::string(8, '\x87') + "readable" + std::string(8, '\x87');
  const std::string noisySafe = SanitizeToModifiedUtf8(noisy);
  expect(IsValidModifiedUtf8(noisySafe), "noisy buffer is repaired");
  expect(noisySafe.find("readable") != std::string::npos,
         "readable text survives sanitizing");

  std::printf("Utf8Test: %d checks passed\n", checks);
  return 0;
}
