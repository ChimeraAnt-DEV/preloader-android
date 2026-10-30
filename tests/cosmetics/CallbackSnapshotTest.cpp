/**
 * @file CallbackSnapshotTest.cpp
 * @brief Host test for the input bridge's lock-free callback list.
 *
 * The input bridge dispatches on every touch, key, mouse and text event, so the list it iterates
 * must be read without a lock and without copying. The property that matters is that a snapshot
 * taken by a reader stays valid while it iterates, even if a writer publishes a new list mid-loop.
 * That is what the shared pointer the atomic load returns guarantees, and it is what this test
 * exercises directly.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "pl/runtime/CallbackSnapshot.hpp"

using pl::runtime::CallbackSnapshot;

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
  CallbackSnapshot<int> store;

  expect(store.size() == 0, "a fresh store is empty");

  store.add(1);
  store.add(2);
  store.add(3);
  expect(store.size() == 3, "three registrations are counted");

  // Every callback is visited, in registration order, by const reference.
  std::vector<int> seen;
  store.forEach([&](const int &value) { seen.push_back(value); });
  expect(seen.size() == 3, "forEach visits every callback");
  expect(seen[0] == 1 && seen[1] == 2 && seen[2] == 3, "registration order is preserved");

  // clear() empties the store.
  store.clear();
  expect(store.size() == 0, "clear() empties the store");
  int visitedAfterClear = 0;
  store.forEach([&](const int &) { ++visitedAfterClear; });
  expect(visitedAfterClear == 0, "nothing is visited after clear()");

  // The reader's snapshot survives a writer publishing a new list mid-iteration. This is the
  // property that makes the lock-free read safe: the shared pointer taken at the start of forEach
  // keeps the list alive, so a registration inside the loop cannot free what is being iterated.
  CallbackSnapshot<int> racing;
  racing.add(10);
  racing.add(20);
  std::vector<int> iterated;
  racing.forEach([&](const int &value) {
    iterated.push_back(value);
    if (value == 10) {
      // A registration during iteration must not invalidate the running loop.
      racing.add(30);
    }
  });
  expect(iterated.size() == 2, "a registration mid-iteration does not disturb the running loop");
  expect(iterated[0] == 10 && iterated[1] == 20, "the snapshot iterates its own values");
  expect(racing.size() == 3, "the registration is visible to the next reader");

  // A reader on another thread sees either the old or the new list, never a torn one.
  CallbackSnapshot<int> shared;
  shared.add(1);
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread writer([&] {
    for (int i = 0; i < 2000; ++i) {
      shared.add(i);
    }
    stop.store(true);
  });
  while (!stop.load()) {
    int local = 0;
    shared.forEach([&](const int &) { ++local; });
    if (local <= 0) {
      bad.fetch_add(1);
    }
  }
  writer.join();
  expect(bad.load() == 0, "a concurrent reader always observes at least one callback");

  std::printf("CallbackSnapshotTest: %d checks passed\n", checks);
  return 0;
}
