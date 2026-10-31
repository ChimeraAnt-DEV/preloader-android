#include "pl/memory/ElfBuildId.hpp"

#include <cstring>

namespace pl::memory {
namespace {

constexpr std::uint32_t kElfMagic = 0x464C457FU;      // 0x7F 'E' 'L' 'F'
constexpr std::uint8_t kElfClass32 = 1;
constexpr std::uint8_t kElfClass64 = 2;
constexpr std::uint8_t kElfDataLsb = 1;
constexpr std::uint32_t kPtNote = 4;
constexpr std::uint32_t kNoteGnuBuildId = 3;
constexpr char kGnuNoteName[] = "GNU";

std::uint16_t Read16(const std::uint8_t *p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t Read32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t Read64(const std::uint8_t *p) {
  return static_cast<std::uint64_t>(Read32(p)) | (static_cast<std::uint64_t>(Read32(p + 4)) << 32);
}

// Walks the note records in a PT_NOTE segment looking for the GNU build id.
bool ScanNoteSegment(const std::uint8_t *image, std::size_t size, std::uint64_t offset,
                     std::uint64_t length, std::string &out) {
  if (offset > size) return false;
  if (length > size - offset) length = size - offset;

  std::uint64_t cursor = 0;
  while (cursor + 12 <= length) {
    const std::uint8_t *note = image + offset + cursor;
    const std::uint32_t nameSize = Read32(note);
    const std::uint32_t descSize = Read32(note + 4);
    const std::uint32_t type = Read32(note + 8);

    // Both fields are 4-byte aligned in the file, so the record's total size is the sum rounded up.
    const std::uint64_t namePadded = (static_cast<std::uint64_t>(nameSize) + 3U) & ~3ULL;
    const std::uint64_t descPadded = (static_cast<std::uint64_t>(descSize) + 3U) & ~3ULL;
    const std::uint64_t recordSize = 12ULL + namePadded + descPadded;
    if (recordSize == 0 || cursor + recordSize > length) break;

    const char *name = reinterpret_cast<const char *>(note + 12);
    const bool gnuName = nameSize >= 4 && std::strncmp(name, kGnuNoteName, 3) == 0;
    if (type == kNoteGnuBuildId && gnuName && descSize > 0 && descSize <= 64) {
      const std::uint8_t *desc = note + 12 + namePadded;
      static const char kHex[] = "0123456789abcdef";
      out.clear();
      out.reserve(descSize * 2);
      for (std::uint32_t i = 0; i < descSize; ++i) {
        out.push_back(kHex[(desc[i] >> 4) & 0xF]);
        out.push_back(kHex[desc[i] & 0xF]);
      }
      return true;
    }
    cursor += recordSize;
  }
  return false;
}

} // namespace

bool ReadElfBuildId(const std::uint8_t *image, std::size_t size, std::string &out) {
  out.clear();
  if (image == nullptr || size < 52) return false;
  if (Read32(image) != kElfMagic) return false;
  const std::uint8_t elfClass = image[4];
  if (image[5] != kElfDataLsb) return false;

  std::uint64_t phoff = 0;
  std::uint16_t phentsize = 0;
  std::uint16_t phnum = 0;
  if (elfClass == kElfClass64) {
    if (size < 64) return false;
    phoff = Read64(image + 32);
    phentsize = Read16(image + 54);
    phnum = Read16(image + 56);
  } else if (elfClass == kElfClass32) {
    phoff = Read32(image + 28);
    phentsize = Read16(image + 42);
    phnum = Read16(image + 44);
  } else {
    return false;
  }
  if (phentsize == 0 || phnum == 0) return false;
  if (phoff > size) return false;

  for (std::uint16_t i = 0; i < phnum; ++i) {
    const std::uint64_t entryOffset =
        phoff + static_cast<std::uint64_t>(i) * phentsize;
    if (entryOffset + phentsize > size) break;
    const std::uint8_t *entry = image + entryOffset;
    const std::uint32_t type = Read32(entry);
    if (type != kPtNote) continue;

    std::uint64_t noteOffset = 0;
    std::uint64_t noteLength = 0;
    if (elfClass == kElfClass64) {
      if (phentsize < 56) continue;
      noteOffset = Read64(entry + 8);
      noteLength = Read64(entry + 32);
    } else {
      if (phentsize < 32) continue;
      noteOffset = Read32(entry + 4);
      noteLength = Read32(entry + 16);
    }
    if (ScanNoteSegment(image, size, noteOffset, noteLength, out)) return true;
  }
  return false;
}

} // namespace pl::memory
