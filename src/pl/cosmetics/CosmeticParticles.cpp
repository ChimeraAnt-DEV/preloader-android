/**
 * @file CosmeticParticles.cpp
 * @brief Pure particle/aura model for cosmetic VFX.
 */

#include "pl/cosmetics/CosmeticParticles.hpp"

#include <cmath>

namespace pl::cosmetics {

float CosmeticParticles::nextUnit() {
  // xorshift32: cheap, deterministic, and good enough for a sparkle jitter.
  mState ^= mState << 13;
  mState ^= mState >> 17;
  mState ^= mState << 5;
  return static_cast<float>(mState & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

void CosmeticParticles::emit(int count, float x, float y, float z, float spread, float speed,
                             float life) {
  if (!mEnabled || count <= 0) return;
  if (life <= 0.0F) life = 1.0F;
  for (int i = 0; i < count; ++i) {
    Particle &p = mParticles[mNext];
    mNext = (mNext + 1) % kMaxParticles;
    // Uniform jitter in [-spread, spread] on x/z, a small lift on y.
    p.x = x + (nextUnit() * 2.0F - 1.0F) * spread;
    p.y = y + nextUnit() * spread * 0.5F;
    p.z = z + (nextUnit() * 2.0F - 1.0F) * spread;
    p.vx = (nextUnit() * 2.0F - 1.0F) * speed * 0.25F;
    p.vy = speed * (0.5F + nextUnit());
    p.vz = (nextUnit() * 2.0F - 1.0F) * speed * 0.25F;
    p.ageSeconds = 0.0F;
    p.lifeSeconds = life * (0.7F + nextUnit() * 0.6F);
    p.size = 0.6F + nextUnit() * 0.8F;
    p.alive = true;
  }
}

void CosmeticParticles::step(float dtSeconds, float gravity) {
  if (dtSeconds <= 0.0F) return;
  // Clamp so a stall cannot fling particles or bank a huge step.
  if (dtSeconds > 0.1F) dtSeconds = 0.1F;
  for (Particle &p : mParticles) {
    if (!p.alive) continue;
    p.ageSeconds += dtSeconds;
    if (p.ageSeconds >= p.lifeSeconds) {
      p.alive = false;
      continue;
    }
    p.vy -= gravity * dtSeconds;
    p.x += p.vx * dtSeconds;
    p.y += p.vy * dtSeconds;
    p.z += p.vz * dtSeconds;
  }
}

std::size_t CosmeticParticles::liveCount() const {
  std::size_t count = 0;
  for (const Particle &p : mParticles) {
    if (p.alive) ++count;
  }
  return count;
}

void CosmeticParticles::clear() {
  for (Particle &p : mParticles) p.alive = false;
  mNext = 0;
}

} // namespace pl::cosmetics
