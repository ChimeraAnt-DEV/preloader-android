/**
 * @file EngineAllocatorTest.cpp
 * @brief Host test for the engine-facing allocator's header and refusal rules.
 *
 * The engine frees an image's backing store through its own allocator, so the free path has to be
 * correct for a pointer this module produced and must refuse one it did not. The header carries the
 * alignment and base pointer the matching aligned delete needs; a magic word makes a foreign or
 * double free detectable at the pointer instead of at the heap.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "pl/runtime/EngineAllocator.hpp"

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
  expect(pl::runtime::EngineAllocImpl(0, 8) == nullptr, "a zero-size request returns nullptr");

  // Alignment is honoured for each supported value, and the payload is zeroed.
  for (std::size_t alignment : {std::size_t{1}, std::size_t{8}, std::size_t{16}, std::size_t{64}}) {
    void *ptr = pl::runtime::EngineAllocImpl(100, alignment);
    expect(ptr != nullptr, "an allocation succeeds");
    expect(reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0, "the payload is aligned");
    const auto *bytes = static_cast<const std::uint8_t *>(ptr);
    bool zeroed = true;
    for (int i = 0; i < 100; ++i) {
      if (bytes[i] != 0) zeroed = false;
    }
    expect(zeroed, "the payload is zeroed");
    expect(pl::runtime::EngineFreeImpl(ptr), "the pointer frees");
  }

  // A double free is refused, not performed twice.
  void *once = pl::runtime::EngineAllocImpl(32, 16);
  expect(pl::runtime::EngineFreeImpl(once), "the first free succeeds");
  expect(!pl::runtime::EngineFreeImpl(once), "the second free is refused");

  // A foreign pointer is refused. Allocate on the heap so the header check reads real memory.
  void *foreign = ::operator new(32);
  expect(!pl::runtime::EngineFreeImpl(foreign), "a foreign pointer is refused");
  ::operator delete(foreign);

  expect(!pl::runtime::EngineFreeImpl(nullptr), "nullptr is refused");

  // An unaligned request is clamped up to the pointer alignment.
  void *clamped = pl::runtime::EngineAllocImpl(8, 1);
  expect(clamped != nullptr &&
             reinterpret_cast<std::uintptr_t>(clamped) % alignof(void *) == 0,
         "an under-aligned request is clamped to alignof(void*)");
  expect(pl::runtime::EngineFreeImpl(clamped), "the clamped allocation frees");

  std::printf("EngineAllocatorTest: %d checks passed\n", checks);
  return 0;
}
