/**
 * @file SnapshotChannelTest.cpp
 * @brief Host test for the lock-free snapshot channel that carries game data off the render thread.
 *
 * The render thread publishes a snapshot and a consumer on another thread reads a consistent copy —
 * never a half-written payload, never a pointer into data the writer can rewrite. These checks pin
 * the seqlock's observable contract (latest-wins, size-bounded, torn reads rejected) with a real
 * reader thread hammering the writer, which is the shape that would otherwise surface as a torn read
 * only on a device.
 */

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <thread>
#include <vector>

#include "pl/ipc/SnapshotChannel.hpp"

using pl::ipc::SnapshotChannel;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
  std::vector<std::uint8_t> out;
  for (int v : values) out.push_back(static_cast<std::uint8_t>(v));
  return out;
}

} // namespace

int main() {
  // A fresh channel has no snapshot.
  {
    SnapshotChannel channel(16);
    expect(!channel.hasSnapshot(), "fresh channel has no snapshot");
    std::vector<std::uint8_t> out;
    expect(!channel.read(out), "read on an empty channel fails");
    expect(channel.capacity() == 16, "capacity reported");
  }

  // Publish then read returns exactly what was published.
  {
    SnapshotChannel channel(16);
    const std::vector<std::uint8_t> payload = bytes({1, 2, 3, 4});
    expect(channel.publish(std::span<const std::uint8_t>(payload)) == SnapshotChannel::PublishResult::Ok,
           "publish ok");
    expect(channel.hasSnapshot(), "snapshot present after publish");
    std::vector<std::uint8_t> out;
    expect(channel.read(out), "read succeeds");
    expect(out == payload, "read returns the published bytes");
    expect(channel.lastLength() == 4, "last length reported");
  }

  // A later publish replaces the earlier one (latest wins), and the buffer is not grown.
  {
    SnapshotChannel channel(16);
    const std::vector<std::uint8_t> first = bytes({9, 9, 9});
    const std::vector<std::uint8_t> second = bytes({5, 5});
    channel.publish(std::span<const std::uint8_t>(first));
    channel.publish(std::span<const std::uint8_t>(second));
    std::vector<std::uint8_t> out;
    expect(channel.read(out), "read after republish");
    expect(out == second, "the latest payload wins");
    expect(out.size() == 2, "the shorter payload does not keep the longer length");
  }

  // An over-large payload is refused and leaves the previous snapshot intact (no truncated publish).
  {
    SnapshotChannel channel(4);
    const std::vector<std::uint8_t> good = bytes({1, 2});
    channel.publish(std::span<const std::uint8_t>(good));
    const std::vector<std::uint8_t> tooBig = bytes({1, 2, 3, 4, 5});
    expect(channel.publish(std::span<const std::uint8_t>(tooBig)) ==
               SnapshotChannel::PublishResult::TooLarge,
           "over-capacity publish refused");
    std::vector<std::uint8_t> out;
    expect(channel.read(out), "read after a refused publish");
    expect(out == good, "a refused publish leaves the previous snapshot intact");
  }

  // An empty publish is valid and reads back empty.
  {
    SnapshotChannel channel(4);
    expect(channel.publish({}) == SnapshotChannel::PublishResult::Ok, "empty publish ok");
    std::vector<std::uint8_t> out;
    expect(channel.read(out), "read after empty publish");
    expect(out.empty(), "empty payload reads back empty");
  }

  // The concurrency contract: a reader hammering the channel while the writer republishes must only
  // ever observe a whole payload from the set the writer writes — never a torn mix.
  {
    SnapshotChannel channel(8);
    const std::vector<std::uint8_t> a = bytes({1, 1, 1, 1, 1, 1, 1, 1});
    const std::vector<std::uint8_t> b = bytes({2, 2, 2, 2, 2, 2, 2, 2});
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};

    std::thread writer([&]() {
      for (int i = 0; i < 200000 && !stop.load(); ++i) {
        channel.publish(std::span<const std::uint8_t>(i % 2 == 0 ? a : b));
      }
      stop.store(true);
    });

    std::thread reader([&]() {
      std::vector<std::uint8_t> out;
      while (!stop.load()) {
        if (!channel.read(out)) continue;
        reads.fetch_add(1);
        for (std::size_t i = 1; i < out.size(); ++i) {
          if (out[i] != out[0]) { torn.fetch_add(1); break; }
        }
      }
    });

    writer.join();
    reader.join();
    expect(torn.load() == 0, "a reader never observes a torn payload");
    expect(reads.load() > 0, "the reader observed at least one snapshot");
  }

  std::printf("SnapshotChannelTest: %d checks passed\n", checks);
  return 0;
}
