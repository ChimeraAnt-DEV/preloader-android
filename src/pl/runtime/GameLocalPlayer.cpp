#include "pl/runtime/GameLocalPlayer.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Vtable.hpp"

namespace pl::runtime {
namespace {

// The local player's entity origin (a Vec3) and view rotation (yaw, pitch, in
// degrees) on the Actor/LocalPlayer object. Both offsets are shared by every build
// the feed targets (1.26.33 / 1.26.50 / 1.26.51) and were confirmed against the
// 1.26.50.4 and 1.26.60.28 binaries.
constexpr std::size_t kActorPositionOffset = 0x230;
constexpr std::size_t kActorRotationOffset = 0x238;

// ClientInstance's vtable slot whose call returns the local player. Reached by name
// through the existing RTTI resolver, so no per-build code address is needed.
constexpr std::size_t kGetLocalPlayerSlot = 31;

constexpr const char *kGameModule = "libminecraftpe.so";

// A snapshot is considered live for this long after the last game call. The hook runs
// every frame while a world is loaded, so during play the stamp is always fresh; after
// teardown it goes stale within this window and the readers stop reporting a position
// instead of handing the caller a stale one.
constexpr long long kSnapshotFreshMs = 2000;

void *(*g_origGetLocalPlayer)(void *) = nullptr;
std::atomic_bool g_hookInstalled{false};

// Byte offset of the local player's health field, configured per game version from the
// signature rules. Zero means "not configured", and the health read then reports no data
// rather than reading an unverified offset -- a guessed field could sit at a plausible
// value and make a highlight fire on its own.
std::atomic<std::size_t> g_healthOffset{0};

// Seqlock: the game thread writes the snapshot, the Java UI thread reads it, and a torn
// cross-field read (a new x with an old y) must not be observable.
std::atomic<std::uint32_t> g_seq{0};
float g_position[3] = {0.0f, 0.0f, 0.0f};
float g_rotation[2] = {0.0f, 0.0f};
float g_health = 0.0f;
bool g_healthValid = false;
std::atomic<long long> g_stampMs{0};

// A health reading outside this range is not a real health value; the field was
// misconfigured, so the sample is treated as "no data".
constexpr float kMaxPlausibleHealth = 20.0f;

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

bool FinitePosition(const float p[3]) {
  // A garbage read shows up as NaN/inf; a real Bedrock position is well within the
  // world border, so an absurd magnitude is treated as "no data" too.
  constexpr float kWorldLimit = 3.0e7f;
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(p[i]) || std::fabs(p[i]) > kWorldLimit) {
      return false;
    }
  }
  return true;
}

void PublishSnapshot(const void *localPlayer) {
  const auto *position = reinterpret_cast<const float *>(
      reinterpret_cast<std::uintptr_t>(localPlayer) + kActorPositionOffset);
  const auto *rotation = reinterpret_cast<const float *>(
      reinterpret_cast<std::uintptr_t>(localPlayer) + kActorRotationOffset);

  float nextPosition[3] = {position[0], position[1], position[2]};
  float nextRotation[2] = {rotation[0], rotation[1]};
  if (!FinitePosition(nextPosition)) {
    return;
  }

  const std::size_t healthOffset = g_healthOffset.load(std::memory_order_relaxed);
  bool nextHealthValid = false;
  float nextHealth = 0.0f;
  if (healthOffset != 0) {
    const auto *health = reinterpret_cast<const float *>(
        reinterpret_cast<std::uintptr_t>(localPlayer) + healthOffset);
    const float value = *health;
    if (std::isfinite(value) && value >= 0.0f && value <= kMaxPlausibleHealth) {
      nextHealth = value;
      nextHealthValid = true;
    }
  }

  g_seq.fetch_add(1, std::memory_order_acq_rel); // odd: write in progress
  std::memcpy(g_position, nextPosition, sizeof(g_position));
  std::memcpy(g_rotation, nextRotation, sizeof(g_rotation));
  g_health = nextHealth;
  g_healthValid = nextHealthValid;
  g_stampMs.store(MonotonicMs(), std::memory_order_relaxed);
  g_seq.fetch_add(1, std::memory_order_release); // even: write complete
}

// Reads the latest snapshot, or false when none is live. The dereference of the local
// player happens in the hook (on the game thread, while the game itself just returned
// the pointer); this side only ever touches the copy, so a session ending can never
// leave the UI thread reading freed memory.
bool ReadSnapshot(float outPosition[3], float outRotation[2]) {
  std::uint32_t s1 = 0;
  std::uint32_t s2 = 0;
  float position[3];
  float rotation[2];
  long long stamp = 0;
  do {
    s1 = g_seq.load(std::memory_order_acquire);
    if ((s1 & 1U) != 0U) {
      return false;
    }
    std::memcpy(position, g_position, sizeof(position));
    std::memcpy(rotation, g_rotation, sizeof(rotation));
    stamp = g_stampMs.load(std::memory_order_relaxed);
    s2 = g_seq.load(std::memory_order_acquire);
  } while (s1 != s2);

  if (stamp == 0 || MonotonicMs() - stamp > kSnapshotFreshMs) {
    return false;
  }
  std::memcpy(outPosition, position, sizeof(position));
  std::memcpy(outRotation, rotation, sizeof(rotation));
  return true;
}

// Reads the health half of the snapshot. Kept separate from ReadSnapshot so the
// position/rotation callers pay no cost for a field they never use, and so a snapshot
// taken before a health offset was configured reports no health (never a stale value).
bool ReadHealthSnapshot(float *outHealth) {
  std::uint32_t s1 = 0;
  std::uint32_t s2 = 0;
  float health = 0.0f;
  bool valid = false;
  long long stamp = 0;
  do {
    s1 = g_seq.load(std::memory_order_acquire);
    if ((s1 & 1U) != 0U) {
      return false;
    }
    health = g_health;
    valid = g_healthValid;
    stamp = g_stampMs.load(std::memory_order_relaxed);
    s2 = g_seq.load(std::memory_order_acquire);
  } while (s1 != s2);

  if (!valid || stamp == 0 || MonotonicMs() - stamp > kSnapshotFreshMs) {
    return false;
  }
  *outHealth = health;
  return true;
}

// The one hook: the client's own local-player accessor becomes the place we latch the
// live local player and snapshot its position/rotation, once per call.
void *HookGetLocalPlayer(void *self) {
  void *localPlayer = g_origGetLocalPlayer ? g_origGetLocalPlayer(self) : nullptr;
  if (localPlayer != nullptr) {
    PublishSnapshot(localPlayer);
  }
  return localPlayer;
}

} // namespace

void InitLocalPlayerSource() {
  if (g_hookInstalled.exchange(true, std::memory_order_relaxed)) {
    return;
  }

  const std::uintptr_t slot =
      pl::memory::resolveVtableFunction("14ClientInstance", kGetLocalPlayerSlot,
                                        kGameModule);
  if (!slot) {
    preloaderLogger.warn(
        "Local-player feed: ClientInstance local-player slot unresolved; "
        "in-world voice nametag icons stay off");
    return;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(slot),
                       reinterpret_cast<pl::memory::FuncPtr>(HookGetLocalPlayer),
                       reinterpret_cast<pl::memory::FuncPtr *>(
                           &g_origGetLocalPlayer)) != 0) {
    preloaderLogger.warn("Local-player feed: hook install failed; icons stay off");
  }
}

bool IsLocalPlayerAvailable() {
  float position[3];
  float rotation[2];
  return ReadSnapshot(position, rotation);
}

bool ReadLocalPlayerPosition(float out[3]) {
  float rotation[2];
  return ReadSnapshot(out, rotation);
}

bool ReadLocalPlayerRotation(float out[2]) {
  float position[3];
  return ReadSnapshot(position, out);
}

bool ReadLocalPlayerHealth(float out[1]) {
  return ReadHealthSnapshot(out);
}

void SetLocalPlayerHealthOffset(std::size_t offset) {
  g_healthOffset.store(offset, std::memory_order_relaxed);
}

} // namespace pl::runtime
