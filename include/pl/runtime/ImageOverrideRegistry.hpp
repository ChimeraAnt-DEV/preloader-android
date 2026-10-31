#pragma once

/**
 * @file ImageOverrideRegistry.hpp
 * @brief Lock-free, lifetime-safe stores for the image-substitution seam.
 *
 * The `mce::Image` loader detour runs on whatever thread the engine builds images on. Two things
 * there must hold:
 *
 *  - a lookup must not copy a pixel buffer, and must not return a pointer into a buffer another
 *    thread can free while the caller still reads it; and
 *  - a registration (rare, from the launcher thread) must not block the lookup.
 *
 * Both are met by publishing an immutable snapshot behind an atomically-loaded `shared_ptr`. A
 * reader takes one atomic load and gets a `shared_ptr<const OverrideImage>` that keeps the pixels
 * alive for as long as the reader holds it -- including across the engine call the pixels are fed
 * to. A writer builds a new snapshot and swaps it in.
 *
 * The content-addressed registry keeps its entries in a **sorted flat vector** keyed by hash, so a
 * lookup is a binary search with no allocation (an earlier shape used a hash map plus a second map
 * for metadata, which is two lookups and two rehash opportunities on the hot path).
 *
 * `std::atomic<std::shared_ptr<T>>` is not implemented in the NDK's libc++, so the C++20-deprecated
 * `atomic_load`/`atomic_store` free functions are used; their deprecation is suppressed here once.
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace pl::runtime {

/** One override image: the pixels and the size they were registered at. */
struct OverrideImage {
  /** Raw RGBA rows, top-left origin, `width * height * 4` bytes. */
  std::vector<std::uint8_t> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

/**
 * @brief A content-addressed registry: "when the engine loads bytes hashing to H, use these".
 *
 * Reads are lock-free and allocation-free; a returned image stays alive as long as the caller holds
 * the shared pointer, so the pixels cannot be freed mid-use.
 */
class ImageOverrideRegistry {
public:
  using ImagePtr = std::shared_ptr<const OverrideImage>;

  ImageOverrideRegistry() : mEntries(std::make_shared<const std::vector<Entry>>()) {}

  ImageOverrideRegistry(const ImageOverrideRegistry &) = delete;
  ImageOverrideRegistry &operator=(const ImageOverrideRegistry &) = delete;

  /**
   * @brief Adds or replaces the override for a key.
   *
   * A zero-size image removes the entry, matching the "clear on empty" contract callers expect.
   */
  void set(std::uint64_t key, std::vector<std::uint8_t> pixels, std::uint32_t width,
           std::uint32_t height) {
    if (pixels.empty() || width == 0 || height == 0) {
      remove(key);
      return;
    }
    auto image = std::make_shared<OverrideImage>();
    image->pixels = std::move(pixels);
    image->width = width;
    image->height = height;

    auto next = std::make_shared<std::vector<Entry>>(*std::atomic_load(&mEntries));
    const auto it = std::lower_bound(
        next->begin(), next->end(), key,
        [](const Entry &entry, std::uint64_t value) { return entry.key < value; });
    if (it != next->end() && it->key == key) {
      it->image = std::move(image);
    } else {
      next->insert(it, Entry{key, std::move(image)});
    }
    std::atomic_store(&mEntries, std::shared_ptr<const std::vector<Entry>>(std::move(next)));
  }

  /** @brief Removes the override for a key, leaving every other entry untouched. */
  void remove(std::uint64_t key) {
    auto next = std::make_shared<std::vector<Entry>>(*std::atomic_load(&mEntries));
    const auto it = std::lower_bound(
        next->begin(), next->end(), key,
        [](const Entry &entry, std::uint64_t value) { return entry.key < value; });
    if (it == next->end() || it->key != key) {
      return;
    }
    next->erase(it);
    std::atomic_store(&mEntries, std::shared_ptr<const std::vector<Entry>>(std::move(next)));
  }

  /** @brief Removes every entry. */
  void clear() {
    std::atomic_store(&mEntries, std::make_shared<const std::vector<Entry>>());
  }

  /** @brief The number of registered overrides. */
  std::size_t size() const { return std::atomic_load(&mEntries)->size(); }

  /**
   * @brief The override for a key, or null when none.
   *
   * The returned pointer owns the pixels, so the caller may keep it across an engine call that
   * happens after this lookup returns.
   */
  ImagePtr find(std::uint64_t key) const {
    const auto snapshot = std::atomic_load(&mEntries);
    const auto it = std::lower_bound(
        snapshot->begin(), snapshot->end(), key,
        [](const Entry &entry, std::uint64_t value) { return entry.key < value; });
    if (it == snapshot->end() || it->key != key) {
      return nullptr;
    }
    return it->image;
  }

private:
  struct Entry {
    std::uint64_t key = 0;
    ImagePtr image;
  };

  std::shared_ptr<const std::vector<Entry>> mEntries;
};

/**
 * @brief A single-slot, take-once override for "the very next image the engine builds is ours".
 *
 * `take()` swaps the slot out atomically and hands the caller the shared pointer, so the pixels are
 * moved out rather than cleared and then read through a raw pointer into the emptied buffer. A
 * writer that arms the slot concurrently with a take either wins or loses whole; it can never leave
 * the taker holding a dangling pointer.
 */
class NextImageOverride {
public:
  using ImagePtr = std::shared_ptr<const OverrideImage>;

  /** @brief Arms the slot; a zero-size image clears it. */
  void arm(std::vector<std::uint8_t> pixels, std::uint32_t width, std::uint32_t height) {
    if (pixels.empty() || width == 0 || height == 0) {
      clear();
      return;
    }
    auto image = std::make_shared<OverrideImage>();
    image->pixels = std::move(pixels);
    image->width = width;
    image->height = height;
    std::atomic_store(&mSlot, ImagePtr(std::move(image)));
  }

  /** @brief Takes the armed override, leaving the slot empty. */
  ImagePtr take() {
    const ImagePtr taken = std::atomic_exchange(&mSlot, ImagePtr{});
    return taken;
  }

  /** @brief Whether an override is currently armed. */
  bool armed() const { return std::atomic_load(&mSlot) != nullptr; }

  /** @brief Clears the slot. */
  void clear() { std::atomic_store(&mSlot, ImagePtr{}); }

private:
  ImagePtr mSlot;
};

/**
 * @brief A single captured image (the player's base skin or cape) behind an atomic snapshot.
 *
 * The capture is written from the loader detour and read from the launcher thread. Publishing an
 * immutable snapshot means the read copies without taking the writer's lock, so a capture that
 * lands during a read cannot block it and cannot free the bytes it is reading.
 */
struct CapturedImage {
  std::vector<std::uint8_t> bytes;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::int64_t stampMs = 0;
};

class CapturedImageSlot {
public:
  using ImagePtr = std::shared_ptr<const CapturedImage>;

  /** @brief Replaces the captured image. */
  void set(std::vector<std::uint8_t> bytes, std::uint32_t width, std::uint32_t height,
           std::uint32_t format, std::int64_t stampMs) {
    if (bytes.empty() || width == 0 || height == 0) {
      clear();
      return;
    }
    auto image = std::make_shared<CapturedImage>();
    image->bytes = std::move(bytes);
    image->width = width;
    image->height = height;
    image->format = format;
    image->stampMs = stampMs;
    std::atomic_store(&mSlot, ImagePtr(std::move(image)));
  }

  /** @brief The captured image, or null when nothing has been captured. */
  ImagePtr get() const { return std::atomic_load(&mSlot); }

  /** @brief Whether an image is currently held. */
  bool has() const { return std::atomic_load(&mSlot) != nullptr; }

  /** @brief Clears the slot. */
  void clear() { std::atomic_store(&mSlot, ImagePtr{}); }

private:
  ImagePtr mSlot;
};

} // namespace pl::runtime

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
