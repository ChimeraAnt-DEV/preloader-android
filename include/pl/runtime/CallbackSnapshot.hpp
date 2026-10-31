#pragma once

/**
 * @file CallbackSnapshot.hpp
 * @brief A lock-free, immutable snapshot of a callback list for the input hot path.
 *
 * The input bridge dispatches on every touch, key, mouse and text event, inside the game's own
 * input path. The previous shape copied two `std::vector`s under a mutex on every event, so each
 * event paid a lock and two heap allocations on the input-to-photon path.
 *
 * This store publishes an immutable list instead. A reader performs one atomic load and iterates
 * by const reference; a registration builds a new list and swaps it in. Registration is rare, so
 * its cost does not matter, and a reader never blocks a writer or observes a half-built list.
 *
 * `std::atomic<std::shared_ptr<T>>` is not available in the NDK's libc++, so the pre-C++20
 * `std::atomic_load` / `std::atomic_store` free functions are used on a plain `shared_ptr`. That
 * keeps the published list alive for as long as a reader holds it.
 */

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

// C++20 deprecates the `atomic_load`/`atomic_store` free functions in favour of
// `std::atomic<std::shared_ptr<T>>`, but that specialization is not implemented in the NDK's
// libc++ (it needs a trivially copyable type). The free functions are therefore the only way to
// publish a shared pointer atomically here, so their deprecation is suppressed rather than
// silenced by leaving the warning to every build.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace pl::runtime {

/**
 * @brief Publishes an immutable list of callbacks, read without a lock.
 *
 * @tparam T the callback type held in the list.
 */
template <typename T> class CallbackSnapshot {
public:
  using List = std::vector<T>;

  CallbackSnapshot() : mValue(std::make_shared<const List>()) {}

  CallbackSnapshot(const CallbackSnapshot &) = delete;
  CallbackSnapshot &operator=(const CallbackSnapshot &) = delete;

  /**
   * @brief Appends a callback, publishing a new list.
   *
   * The copy-then-store is safe because the list is immutable once published: no reader can be
   * mutating it, and the swap is a single atomic store of the new shared pointer.
   */
  void add(T callback) {
    auto next = std::make_shared<List>(*std::atomic_load(&mValue));
    next->push_back(std::move(callback));
    std::atomic_store(&mValue, std::shared_ptr<const List>(std::move(next)));
  }

  /** @brief Removes every callback. */
  void clear() { std::atomic_store(&mValue, std::make_shared<const List>()); }

  /** @brief The number of registered callbacks. */
  std::size_t size() const { return std::atomic_load(&mValue)->size(); }

  /**
   * @brief Runs `fn` for every callback, holding the published list alive for the whole loop.
   *
   * `fn` receives the callback by const reference. The shared pointer taken by the atomic load is
   * what keeps the list valid, so a concurrent registration cannot free it mid-iteration.
   */
  template <typename Fn> void forEach(Fn &&fn) const {
    const std::shared_ptr<const List> snapshot = std::atomic_load(&mValue);
    for (const T &callback : *snapshot) {
      fn(callback);
    }
  }

private:
  std::shared_ptr<const List> mValue;
};

} // namespace pl::runtime

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
