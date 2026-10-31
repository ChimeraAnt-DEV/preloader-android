#pragma once

/**
 * @file EngineAllocator.hpp
 * @brief A tracked allocator whose free path matches the engine's, with no side table.
 *
 * The engine frees an `mce::Image`'s backing store through its own allocator when a skin unloads, so
 * a buffer handed to it must come from a matching allocator. Tracking which pointers we produced
 * used to be a mutex plus an `unordered_map` insert on every allocation and an erase on every free,
 * which put a lock and a map operation on the image-build path.
 *
 * The alignment (and the base pointer the matching aligned `operator delete` needs) are instead
 * written into a small header placed immediately before the returned payload. A free reads the
 * header, so it needs no lock and no lookup, and a magic word makes a foreign or double free
 * detectable at the pointer itself.
 *
 * Pure C++ (no Android types), so the rules are host-testable.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace pl::runtime {

/** Marks a header this module wrote. A mismatch means the pointer was not ours. */
constexpr std::uint64_t kEngineAllocMagic = 0x43484D52414C4C4FULL; // "CHMRALLO"

/** The header stored immediately before every payload `EngineAllocImpl` returns. */
struct EngineAllocHeader {
  std::uint64_t magic;
  void *base;
  std::size_t alignment;
};

/**
 * @brief Allocates `bytes` zeroed bytes aligned to `alignment`, with a header before the payload.
 *
 * @param bytes     requested size; 0 returns nullptr
 * @param alignment requested alignment; clamped to at least `alignof(void*)` and rounded up to a
 *                  power of two
 * @return a zeroed pointer, or nullptr on failure
 */
inline void *EngineAllocImpl(std::size_t bytes, std::size_t alignment) noexcept {
  if (bytes == 0) return nullptr;
  if (alignment < alignof(void *)) alignment = alignof(void *);
  std::size_t align = 1;
  while (align < alignment && align < (std::size_t{1} << 20)) align <<= 1;

  // Over-allocate so the payload can be placed at the first `align`-aligned address at or after
  // `base + sizeof(header)` while still leaving room for the header itself.
  const std::size_t total = bytes + align + sizeof(EngineAllocHeader);
  void *base = nullptr;
  try {
    base = ::operator new(total, std::align_val_t(align));
  } catch (...) {
    return nullptr;
  }
  if (base == nullptr) return nullptr;

  const auto raw = reinterpret_cast<std::uintptr_t>(base);
  std::uintptr_t payload = raw + sizeof(EngineAllocHeader);
  payload = (payload + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
  auto *header = reinterpret_cast<EngineAllocHeader *>(payload - sizeof(EngineAllocHeader));
  header->magic = kEngineAllocMagic;
  header->base = base;
  header->alignment = align;

  void *result = reinterpret_cast<void *>(payload);
  std::memset(result, 0, bytes);
  return result;
}

/**
 * @brief Frees a pointer from `EngineAllocImpl`.
 *
 * @return true when the pointer was ours and was freed; false when it was foreign or already freed
 *         (a double free), which the caller should report. A refused free never touches the heap.
 */
inline bool EngineFreeImpl(void *ptr) noexcept {
  if (ptr == nullptr) return false;
  auto *header = reinterpret_cast<EngineAllocHeader *>(reinterpret_cast<std::uintptr_t>(ptr) -
                                                       sizeof(EngineAllocHeader));
  if (header->magic != kEngineAllocMagic) {
    return false;
  }
  const std::size_t align = header->alignment;
  void *base = header->base;
  // Clear the magic first, so a double free of the same pointer is refused rather than freeing the
  // same block twice.
  header->magic = 0;
  ::operator delete(base, std::align_val_t(align));
  return true;
}

} // namespace pl::runtime
