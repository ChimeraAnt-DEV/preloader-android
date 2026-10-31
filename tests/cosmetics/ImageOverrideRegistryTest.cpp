/**
 * @file ImageOverrideRegistryTest.cpp
 * @brief Host test for the image-substitution stores' lifetime and lookup rules.
 *
 * The loader detour runs on the engine's image thread and the registrations arrive from the launcher
 * thread, so two properties matter and neither is visible on a device until it corrupts a texture:
 *
 *  - a lookup must return something that owns its pixels, so a concurrent registration cannot free
 *    the bytes the engine is about to read; and
 *  - the take-once slot must hand the pixels over whole, never clear the buffer and then read it
 *    through a raw pointer into the emptied storage.
 *
 * Both are exercised directly here, including from two threads.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "pl/runtime/ImageOverrideRegistry.hpp"

using pl::runtime::CapturedImageSlot;
using pl::runtime::ImageOverrideRegistry;
using pl::runtime::NextImageOverride;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

std::vector<std::uint8_t> pixels(std::size_t count, std::uint8_t value) {
  return std::vector<std::uint8_t>(count, value);
}

} // namespace

int main() {
  // --- Content-addressed registry ---
  ImageOverrideRegistry registry;
  expect(registry.size() == 0, "a fresh registry is empty");
  expect(!registry.find(7), "an unknown key misses");

  registry.set(7, pixels(16, 0xAB), 2, 2);
  expect(registry.size() == 1, "a set is counted");
  auto hit = registry.find(7);
  expect(hit != nullptr, "the key hits after a set");
  expect(hit->width == 2 && hit->height == 2, "the dimensions round-trip");
  expect(hit->pixels.size() == 16 && hit->pixels[0] == 0xAB, "the pixels round-trip");

  // Replacing a key keeps one entry and swaps the pixels.
  registry.set(7, pixels(4, 0xCD), 1, 1);
  expect(registry.size() == 1, "replacing a key does not add an entry");
  auto replaced = registry.find(7);
  expect(replaced->pixels.size() == 4 && replaced->pixels[0] == 0xCD, "the replacement wins");

  // Sorted order: several keys all resolve, and lookup is exact.
  registry.set(3, pixels(4, 1), 1, 1);
  registry.set(9, pixels(4, 2), 1, 1);
  registry.set(5, pixels(4, 3), 1, 1);
  expect(registry.size() == 4, "four distinct keys");
  expect(registry.find(3)->pixels[0] == 1, "key 3 resolves");
  expect(registry.find(5)->pixels[0] == 3, "key 5 resolves");
  expect(registry.find(9)->pixels[0] == 2, "key 9 resolves");
  expect(!registry.find(4), "a key between two present ones misses");

  // A zero-size image removes just that key.
  registry.set(3, {}, 0, 0);
  expect(registry.size() == 3, "a zero-size set removes the entry");
  expect(!registry.find(3), "the removed key misses");
  expect(registry.find(5) != nullptr, "the neighbours survive a remove");

  registry.remove(9);
  expect(!registry.find(9) && registry.size() == 2, "remove drops one entry");

  registry.clear();
  expect(registry.size() == 0, "clear empties the registry");

  // A pointer taken before a replace keeps the old pixels alive: this is the property that makes
  // the read safe while a registration lands on another thread.
  ImageOverrideRegistry keepAlive;
  keepAlive.set(1, pixels(8, 0x11), 2, 1);
  auto held = keepAlive.find(1);
  keepAlive.set(1, pixels(4, 0x22), 1, 1);
  keepAlive.clear();
  expect(held->pixels.size() == 8 && held->pixels[0] == 0x11,
         "a held image survives a replace and a clear");

  // --- Take-once slot ---
  NextImageOverride next;
  expect(!next.armed(), "a fresh slot is not armed");
  expect(!next.take(), "taking an empty slot yields nothing");
  next.arm(pixels(12, 0x5A), 3, 1);
  expect(next.armed(), "arming the slot reports armed");
  auto taken = next.take();
  expect(taken != nullptr, "the armed override is taken");
  expect(taken->pixels.size() == 12 && taken->pixels[0] == 0x5A, "the taken pixels round-trip");
  expect(!next.armed(), "the slot is empty after a take");
  expect(!next.take(), "a second take yields nothing");

  next.arm(pixels(4, 1), 1, 1);
  next.clear();
  expect(!next.armed(), "clear empties the slot");

  // --- Captured image slot ---
  CapturedImageSlot capture;
  expect(!capture.has() && !capture.get(), "a fresh capture slot is empty");
  capture.set(pixels(20, 0x77), 5, 1, 4, 4242);
  expect(capture.has(), "the capture reports present");
  auto captured = capture.get();
  expect(captured->width == 5 && captured->height == 1 && captured->format == 4,
         "the capture dimensions and format round-trip");
  expect(captured->stampMs == 4242, "the capture timestamp round-trips");
  capture.clear();
  expect(!capture.has(), "clear empties the capture");
  expect(captured != nullptr && captured->bytes.size() == 20,
         "a held capture survives a clear");

  // A reader on one thread must never observe a torn or freed image while another replaces it.
  ImageOverrideRegistry shared;
  shared.set(1, pixels(64, 1), 4, 4);
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::thread writer([&] {
    for (int i = 0; i < 5000; ++i) {
      shared.set(1, pixels(64, static_cast<std::uint8_t>(i)), 4, 4);
    }
    stop.store(true);
  });
  while (!stop.load()) {
    auto image = shared.find(1);
    if (!image || image->pixels.size() != 64) {
      torn.fetch_add(1);
    }
  }
  writer.join();
  expect(torn.load() == 0, "a concurrent reader never sees a torn or missing image");

  std::printf("ImageOverrideRegistryTest: %d checks passed\n", checks);
  return 0;
}
