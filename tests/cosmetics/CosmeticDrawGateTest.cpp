/**
 * @file CosmeticDrawGateTest.cpp
 * @brief Host test for the per-render-frame cosmetic draw gate.
 *
 * Two on-device bugs are pinned here because neither is visible on a build machine:
 *
 *  - The debug cube could never draw: the guard returned before the renderer when nothing was
 *    equipped, so the diagnostic self-test was unreachable.
 *  - A frame-version equality check skipped every render call that arrived without a freshly
 *    published Java frame, so cosmetics flickered when the render rate exceeded the push rate.
 */

#include <cstdio>
#include <cstdlib>

#include "pl/cosmetics/CosmeticDrawGate.hpp"

using pl::cosmetics::DecideCosmeticDraw;
using pl::cosmetics::DrawDecision;
using pl::cosmetics::ShouldDrawFrameVersion;

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
  // Nothing equipped, cube off -> skip.
  expect(DecideCosmeticDraw(true, 0x0, false, true, 4096) == DrawDecision::SkipNothingEquipped,
         "nothing equipped skips");

  // Nothing equipped, cube ARMED -> draw. This is the self-test path that used to be unreachable.
  expect(DecideCosmeticDraw(true, 0x0, true, true, 0) == DrawDecision::Draw,
         "debug cube draws with nothing equipped and no geometry");

  // A cape equipped, geometry present -> draw.
  expect(DecideCosmeticDraw(true, 0x1, false, true, 4096) == DrawDecision::Draw,
         "equipped piece draws");

  // No live local player -> never draw (nobody to wear it).
  expect(DecideCosmeticDraw(true, 0x1, true, false, 4096) == DrawDecision::SkipNoLocalPlayer,
         "no local player skips even with the cube armed");

  // No published frame -> skip (the renderer has no transforms).
  expect(DecideCosmeticDraw(false, 0x1, true, true, 4096) == DrawDecision::SkipNoFrame,
         "no frame skips");

  // A piece equipped but no geometry published -> skip (nothing to draw).
  expect(DecideCosmeticDraw(true, 0x1, false, true, 0) == DrawDecision::SkipNoGeometry,
         "missing geometry skips");

  // The VFX flag (bit 3) is inside the low nibble on purpose: a VFX-only frame must reach the
  // renderer's particle path, so it is NOT "nothing equipped".
  expect(DecideCosmeticDraw(true, 0x8, false, true, 4096) == DrawDecision::Draw,
         "a vfx-only frame is not treated as nothing-equipped");

  // Version equality must NEVER skip: the renderer runs more often than Java publishes.
  expect(ShouldDrawFrameVersion(7, 7), "an unchanged published version still draws");
  expect(ShouldDrawFrameVersion(8, 7), "a new published version draws");
  expect(ShouldDrawFrameVersion(0, 0), "the zero version still draws");

  std::printf("CosmeticDrawGateTest: %d checks passed\n", checks);
  return 0;
}
