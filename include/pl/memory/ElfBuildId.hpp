#pragma once

/**
 * @file ElfBuildId.hpp
 * @brief Reads the GNU build id out of an ELF image.
 *
 * A byte-pattern signature is only valid for the exact binary it was derived from, so a scan result
 * is only reusable for the same build. The GNU build id (the `.note.gnu.build-id` note) is the
 * stable identity of that binary: it changes on every relink and is present in every Android `.so`
 * the linker produces. Keying a cached scan by it is what makes "scan once, reuse across launches"
 * safe, because a different build cannot answer from a different build's offsets.
 *
 * The parse is a pure function over a byte buffer, so it is host-testable with a synthetic ELF.
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace pl::memory {

/** The hex length of a build id plus its NUL; the id is normally 20 bytes (40 hex characters). */
constexpr std::size_t kBuildIdHexMax = 65;

/**
 * @brief Reads the GNU build id from an ELF image.
 *
 * Supports ELF64 little-endian (every arm64 Android library) and ELF32 little-endian. The buffer
 * only has to contain the ELF header, the program header table and the note it points at, so a
 * caller can pass just the first page of a mapping.
 *
 * @param image the ELF bytes
 * @param size  how many bytes of @p image are valid
 * @param out   receives the lower-case hex id on success
 * @return true when a build id was found and written
 */
bool ReadElfBuildId(const std::uint8_t *image, std::size_t size, std::string &out);

} // namespace pl::memory
