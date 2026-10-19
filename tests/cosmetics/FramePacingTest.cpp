/**
 * @file FramePacingTest.cpp
 * @brief Host test for the render-thread frame-pacing analyzer (item 11).
 *
 * The analyzer is pure, so its classification rules (a lock, a hitch, no reading before two frames,
 * and no lock claim when the refresh rate is unknown) are pinned without a device.
 */

#include <cstdio>
#include <cstdlib>

#include "pl/render/FramePacing.hpp"

using pl::render::FramePacingAnalyzer;
using pl::render::FramePacingReading;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

}  // namespace

int main() {
  // No reading before two frames have been seen.
  {
    FramePacingAnalyzer a;
    FramePacingReading r = a.onFrame(1000, 60);
    expect(r.sampleCount == 0, "the first frame produces no interval");
    expect(r.averageFrameUs == 0, "no average before an interval exists");
  }

  // A steady 60Hz stream reads as vsync-locked.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 40; ++i) {
      t += 16667; // ~60Hz
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.reading(60);
    expect(r.sampleCount == 39, "every interval after the first is counted");
    expect(r.vsyncLocked, "a steady 60Hz stream is vsync-locked");
    expect(!r.jittering, "a steady stream does not jitter");
    expect(r.averageFrameUs > 16000 && r.averageFrameUs < 17500, "the average is ~16.7ms");
  }

  // One long frame in the window is reported as jitter.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    t += 60000; // a 60ms hitch
    FramePacingReading r = a.onFrame(t, 60);
    expect(r.jittering, "a 60ms frame against a 16.7ms interval is jitter");
    expect(r.worstFrameUs == 60000, "the worst interval is retained");
  }

  // A hitch ages out of the window.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    a.onFrame(t, 60);
    t += 60000; // the hitch
    a.onFrame(t, 60);
    for (int i = 0; i < FramePacingAnalyzer::kWindow + 2; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.reading(60);
    expect(!r.jittering, "a hitch ages out of the window");
    expect(r.worstFrameUs < 30000, "the worst interval is now a normal one");
  }

  // An unknown refresh rate never claims a lock.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 16667;
      a.onFrame(t, 0);
    }
    FramePacingReading r = a.reading(0);
    expect(!r.vsyncLocked, "an unknown refresh rate cannot be locked");
    expect(!r.jittering, "a steady stream with no refresh rate does not jitter");
    expect(r.averageFrameUs > 0, "the average is still measured");
  }

  // An unknown refresh rate still flags a hitch, relative to the mean.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 16667;
      a.onFrame(t, 0);
    }
    t += 80000; // a long stall against a ~16.7ms mean
    FramePacingReading r = a.onFrame(t, 0);
    expect(r.jittering, "a hitch is flagged with no refresh rate, relative to the mean");
    expect(!r.vsyncLocked, "a lock is still not claimed with no refresh rate");
  }

  // A 30fps stream against a 60Hz display is not locked (the average is ~2x the interval).
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 33333;
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.reading(60);
    expect(!r.vsyncLocked, "30fps against 60Hz is not locked");
    expect(r.jittering, "30fps against 60Hz is late on every frame");
  }

  // Reset clears the window.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 10; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    a.reset();
    FramePacingReading r = a.reading(60);
    expect(r.sampleCount == 0, "reset clears the sample count");
    expect(r.averageFrameUs == 0, "reset clears the average");
  }

  // A sub-millisecond floor collapses several model draws in one frame to one sample.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    a.onFrame(t, 60, 3000);
    t += 500; // a second model draw inside the same frame: ignored
    FramePacingReading r = a.onFrame(t, 60, 3000);
    expect(r.sampleCount == 0, "an interval below the floor is not recorded");
    t += 16667;
    r = a.onFrame(t, 60, 3000);
    expect(r.sampleCount == 1, "the next real frame is recorded");
  }

  std::printf("FramePacingTest: %d checks passed\n", checks);
  return 0;
}
