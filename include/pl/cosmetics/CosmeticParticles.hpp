#pragma once

/**
 * @file CosmeticParticles.hpp
 * @brief Pure particle/aura model for cosmetic VFX (host-testable).
 *
 * Cosmetics previously had no visual flourish: a cape, hat or pet was a static mesh with no aura,
 * trail or sparkle. This is the simulation half of a lightweight particle system — spawn, age,
 * gravity and fade — kept deliberately free of GLES and Android types so the motion is unit-tested
 * on the build host. {@code NativeCosmeticRenderer} only projects and draws the particles this
 * returns; it never owns the maths.
 *
 * <p><b>Fixed pool, no allocation.</b> The pool is a fixed array, so a burst allocates nothing and
 * the render thread never touches the heap. Spawning when the pool is full recycles the oldest
 * particle rather than dropping the burst, so an aura stays visible.
 *
 * <p><b>Deterministic.</b> A caller-supplied seed drives every random draw, so a test can assert an
 * exact trajectory and the in-game motion is reproducible frame to frame.
 */

#include <cstddef>
#include <cstdint>

#include "pl/Export.hpp"

namespace pl::cosmetics {

/** @brief One live particle. */
struct Particle {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float vx = 0.0F;
  float vy = 0.0F;
  float vz = 0.0F;
  float ageSeconds = 0.0F;
  float lifeSeconds = 1.0F;
  float size = 1.0F;
  /** 0 = dead; a live particle's remaining life fraction is {@code 1 - age/life}. */
  bool alive = false;

  /** @brief Remaining life as a 0..1 fraction (1 = just spawned, 0 = about to die). */
  [[nodiscard]] float lifeFraction() const {
    if (!alive || lifeSeconds <= 0.0F) return 0.0F;
    float f = 1.0F - ageSeconds / lifeSeconds;
    return f < 0.0F ? 0.0F : (f > 1.0F ? 1.0F : f);
  }
};

/**
 * @brief A fixed-pool particle system for cosmetic auras and trails.
 *
 * Emission is explicit: the caller calls {@link emit} at a point (a cape hem, a pet's feet) with a
 * burst count and the system ages/spawns on {@link step}. Nothing is emitted when the pool is
 * disabled, so a cosmetic without an aura costs nothing.
 */
class PL_EXPORT CosmeticParticles {
public:
  static constexpr std::size_t kMaxParticles = 256;

  CosmeticParticles() = default;

  /** @brief Enables/disables emission. Disabling lets live particles finish, then idles. */
  void setEnabled(bool enabled) { mEnabled = enabled; }
  [[nodiscard]] bool enabled() const { return mEnabled; }

  /** @brief Sets the seed for the deterministic random stream. */
  void setSeed(std::uint32_t seed) { mState = seed == 0 ? 1u : seed; }

  /**
   * @brief Spawns @p count particles at a world point.
   *
   * @param count    particles to spawn (clamped to the pool)
   * @param x,y,z    spawn origin in world blocks
   * @param spread   positional jitter radius
   * @param speed    upward drift speed, blocks/s
   * @param life     particle lifetime, seconds
   */
  void emit(int count, float x, float y, float z, float spread, float speed, float life);

  /** @brief Ages every live particle and integrates its velocity. */
  void step(float dtSeconds, float gravity);

  /** @brief The live particle array (fixed size; check {@link Particle::alive}). */
  [[nodiscard]] const Particle *particles() const { return mParticles; }
  [[nodiscard]] std::size_t capacity() const { return kMaxParticles; }

  /** @brief Number of live particles. */
  [[nodiscard]] std::size_t liveCount() const;

  /** @brief Kills every particle. */
  void clear();

private:
  float nextUnit();  // xorshift -> 0..1

  Particle mParticles[kMaxParticles]{};
  std::size_t mNext = 0;
  bool mEnabled = true;
  std::uint32_t mState = 1u;
};

} // namespace pl::cosmetics
