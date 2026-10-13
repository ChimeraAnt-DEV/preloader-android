/**
 * @file CosmeticParticlesTest.cpp
 * @brief Host test for the cosmetic VFX particle pool.
 *
 * The pool is pure (no GLES/Android), so spawn, ageing, gravity and recycling are all pinned here
 * rather than only visible on a device. The renderer only projects what this returns.
 */

#include "pl/cosmetics/CosmeticParticles.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using pl::cosmetics::CosmeticParticles;

namespace {

int gChecks = 0;

void check(bool condition, const char *message) {
  ++gChecks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    assert(condition);
  }
}

} // namespace

int main() {
  CosmeticParticles particles;
  particles.setSeed(12345u);

  // Nothing is live before an emit.
  check(particles.liveCount() == 0, "pool starts empty");

  particles.emit(5, 0.0F, 1.0F, 0.0F, 0.4F, 1.0F, 1.0F);
  check(particles.liveCount() == 5, "five particles spawned");

  // They must be near the spawn point and moving upward.
  const auto *pool = particles.particles();
  int near = 0;
  for (std::size_t i = 0; i < particles.capacity(); ++i) {
    if (!pool[i].alive) continue;
    if (std::fabs(pool[i].x) <= 0.5F && std::fabs(pool[i].y - 1.0F) <= 0.5F) ++near;
  }
  check(near == 5, "spawned near the origin");

  // Ageing past the lifetime kills them.
  for (int i = 0; i < 200; ++i) particles.step(0.05F, 2.0F);
  check(particles.liveCount() == 0, "all particles die after their lifetime");

  // A disabled pool emits nothing.
  particles.setEnabled(false);
  particles.emit(3, 0.0F, 0.0F, 0.0F, 0.1F, 1.0F, 1.0F);
  check(particles.liveCount() == 0, "disabled pool emits nothing");
  particles.setEnabled(true);

  // A full pool recycles rather than growing past the cap.
  particles.clear();
  particles.emit(static_cast<int>(CosmeticParticles::kMaxParticles) + 40, 0.0F, 0.0F, 0.0F, 0.1F,
                 1.0F, 5.0F);
  check(particles.liveCount() <= CosmeticParticles::kMaxParticles, "pool never exceeds its cap");

  // Gravity pulls a particle down over time.
  particles.clear();
  particles.emit(1, 0.0F, 10.0F, 0.0F, 0.0F, 0.0F, 5.0F);
  float startY = 0.0F;
  for (std::size_t i = 0; i < particles.capacity(); ++i) {
    if (particles.particles()[i].alive) startY = particles.particles()[i].y;
  }
  for (int i = 0; i < 20; ++i) particles.step(0.05F, 2.0F);
  float endY = startY;
  for (std::size_t i = 0; i < particles.capacity(); ++i) {
    if (particles.particles()[i].alive) endY = particles.particles()[i].y;
  }
  check(endY < startY, "gravity pulls particles down");

  std::printf("CosmeticParticlesTest: %d checks passed\n", gChecks);
  return 0;
}
