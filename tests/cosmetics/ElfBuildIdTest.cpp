/**
 * @file ElfBuildIdTest.cpp
 * @brief Host test for the ELF build-id reader.
 *
 * The build id is what makes a cached byte-pattern scan safe to reuse: a different build must not
 * answer from this build's offsets. The reader is exercised against a hand-built ELF (ELF64 and
 * ELF32 little-endian), a wrong magic, a truncated image and a note segment without a build id.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pl/memory/ElfBuildId.hpp"

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void Put16(std::vector<std::uint8_t> &b, std::size_t at, std::uint16_t v) {
  b[at] = static_cast<std::uint8_t>(v & 0xFF);
  b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void Put32(std::vector<std::uint8_t> &b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

void Put64(std::vector<std::uint8_t> &b, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// Builds an ELF64 little-endian image with one PT_NOTE segment holding a GNU build id of `id`.
std::vector<std::uint8_t> MakeElf64(const std::vector<std::uint8_t> &id) {
  const std::size_t ehSize = 64;
  const std::size_t phSize = 56;
  const std::size_t noteOffset = ehSize + phSize;
  const std::uint32_t nameSize = 4;  // "GNU\0"
  const std::uint32_t namePadded = 4;
  const std::uint32_t descSize = static_cast<std::uint32_t>(id.size());
  const std::uint32_t descPadded = (descSize + 3U) & ~3U;
  const std::size_t noteSize = 12 + namePadded + descPadded;
  std::vector<std::uint8_t> image(noteOffset + noteSize, 0);

  image[0] = 0x7F;
  image[1] = 'E';
  image[2] = 'L';
  image[3] = 'F';
  image[4] = 2;  // ELFCLASS64
  image[5] = 1;  // ELFDATA2LSB
  Put64(image, 32, ehSize);  // e_phoff
  Put16(image, 54, static_cast<std::uint16_t>(phSize));  // e_phentsize
  Put16(image, 56, 1);                                   // e_phnum

  Put32(image, ehSize + 0, 4);  // PT_NOTE
  Put64(image, ehSize + 8, noteOffset);
  Put64(image, ehSize + 32, noteSize);

  Put32(image, noteOffset, nameSize);
  Put32(image, noteOffset + 4, descSize);
  Put32(image, noteOffset + 8, 3);  // NT_GNU_BUILD_ID
  std::memcpy(image.data() + noteOffset + 12, "GNU\0", 4);
  std::memcpy(image.data() + noteOffset + 12 + namePadded, id.data(), id.size());
  return image;
}

// Builds an ELF32 little-endian image with one PT_NOTE segment holding a GNU build id of `id`.
std::vector<std::uint8_t> MakeElf32(const std::vector<std::uint8_t> &id) {
  const std::size_t ehSize = 52;
  const std::size_t phSize = 32;
  const std::size_t noteOffset = ehSize + phSize;
  const std::uint32_t namePadded = 4;
  const std::uint32_t descSize = static_cast<std::uint32_t>(id.size());
  const std::uint32_t descPadded = (descSize + 3U) & ~3U;
  const std::size_t noteSize = 12 + namePadded + descPadded;
  std::vector<std::uint8_t> image(noteOffset + noteSize, 0);

  image[0] = 0x7F;
  image[1] = 'E';
  image[2] = 'L';
  image[3] = 'F';
  image[4] = 1;  // ELFCLASS32
  image[5] = 1;  // ELFDATA2LSB
  Put32(image, 28, static_cast<std::uint32_t>(ehSize));  // e_phoff
  Put16(image, 42, static_cast<std::uint16_t>(phSize));  // e_phentsize
  Put16(image, 44, 1);                                   // e_phnum

  Put32(image, ehSize + 0, 4);  // PT_NOTE
  Put32(image, ehSize + 4, static_cast<std::uint32_t>(noteOffset));
  Put32(image, ehSize + 16, static_cast<std::uint32_t>(noteSize));

  Put32(image, noteOffset, 4);
  Put32(image, noteOffset + 4, descSize);
  Put32(image, noteOffset + 8, 3);
  std::memcpy(image.data() + noteOffset + 12, "GNU\0", 4);
  std::memcpy(image.data() + noteOffset + 12 + namePadded, id.data(), id.size());
  return image;
}

} // namespace

int main() {
  // A standard 20-byte build id.
  const std::vector<std::uint8_t> id20 = {0x9f, 0x2a, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                          0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                                          0x01, 0x02};
  std::string out;
  expect(pl::memory::ReadElfBuildId(MakeElf64(id20).data(), MakeElf64(id20).size(), out),
         "an ELF64 build id is read");
  expect(out == "9f2a00112233445566778899aabbccddeeff0102", "the ELF64 id is lower-case hex");

  const auto elf32 = MakeElf32(id20);
  std::string out32;
  expect(pl::memory::ReadElfBuildId(elf32.data(), elf32.size(), out32),
         "an ELF32 build id is read");
  expect(out32 == out, "the ELF32 id matches the ELF64 one");

  // A short id (the linker pads to 4 bytes, but the desc size is what counts).
  const std::vector<std::uint8_t> id4 = {0xde, 0xad, 0xbe, 0xef};
  const auto elf4 = MakeElf64(id4);
  std::string out4;
  expect(pl::memory::ReadElfBuildId(elf4.data(), elf4.size(), out4), "a 4-byte id is read");
  expect(out4 == "deadbeef", "the short id round-trips");

  // Failures.
  std::string ignored;
  expect(!pl::memory::ReadElfBuildId(nullptr, 0, ignored), "a null image is refused");
  std::vector<std::uint8_t> notElf(64, 0);
  expect(!pl::memory::ReadElfBuildId(notElf.data(), notElf.size(), ignored),
         "a wrong magic is refused");
  const auto elf64 = MakeElf64(id20);
  expect(!pl::memory::ReadElfBuildId(elf64.data(), 8, ignored), "a truncated image is refused");
  expect(!pl::memory::ReadElfBuildId(elf64.data(), 64, ignored),
         "an image with only the ELF header is refused");

  // A note segment that carries no build id leaves the output empty and returns false.
  auto noId = MakeElf64(id20);
  Put32(noId, 64 + 56 + 8, 1);  // change the note type to something other than NT_GNU_BUILD_ID
  expect(!pl::memory::ReadElfBuildId(noId.data(), noId.size(), ignored),
         "a note without a build id is refused");
  expect(ignored.empty(), "the output stays empty on failure");

  std::printf("ElfBuildIdTest: %d checks passed\n", checks);
  return 0;
}
