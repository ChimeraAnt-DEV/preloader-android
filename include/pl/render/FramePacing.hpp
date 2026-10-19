#pragma once

/**
 * @file FramePacing.hpp
 * @brief Frame-time analysis for the render thread (item 11).
 *
 * <p>The render thread's priority is boosted (see `OptifineRenderPriority.cpp`), but priority alone
 * does not tell the client whether frames are actually arriving evenly. The post-render callback
 * fires once per rendered frame with a real monotonic timestamp, so the client can measure the
 * frame interval and classify it.
 *
 * <p><b>Honest scope.</b> This measures and reports; it does not reschedule the engine's frame
 * submission. Actively pacing against the display's vsync needs the engine's own swap/present call,
 * which the stripped binary does not expose (the same limit every native seam here documents). The
 * value delivered is a real reading the diagnostics can show and the launcher can act on, rather
 * than a claim that pacing changed.
 *
 * <p>Pure and allocation-free so it is host-tested and can run on the render thread without cost.
 */

#include <cstddef>
#include <cstdint>

namespace pl::render {

/** @brief One frame-time reading, classified against the display interval. */
struct FramePacingReading {
  /** The smoothed frame interval in microseconds, or 0 before two frames have been seen. */
  long long averageFrameUs = 0;
  /** The worst frame interval in the recent window, in microseconds. */
  long long worstFrameUs = 0;
  /** Frames measured so far. */
  std::size_t sampleCount = 0;
  /** A frame arrived more than one interval late (a hitch). */
  bool jittering = false;
  /** The interval matches the display period within tolerance (vsync-locked). */
  bool vsyncLocked = false;
};

/**
 * @brief Tracks frame intervals from the render tick and classifies them.
 *
 * <p>A fixed-size ring of the last {@link kWindow} intervals is kept, so a hitch ages out and the
 * reading reflects the recent steady state rather than the whole session. No allocation: the ring
 * is a member array.
 */
class FramePacingAnalyzer {
public:
  /** Intervals retained for the worst-case and jitter decision. */
  static constexpr std::size_t kWindow = 60;
  /**
   * A frame is "late" when it exceeds the expected interval by this factor. 1.5 tolerates the
   * normal variance around vsync without calling a single dropped frame a pacing problem.
   */
  static constexpr int kLateFactor = 3; // numerator
  static constexpr int kLateDivisor = 2; // denominator: 3/2 = 1.5x
  /** The lock tolerance: within this fraction of the display interval counts as locked. */
  static constexpr int kLockTolerancePercent = 12;

  /**
   * @brief Feeds one rendered frame.
   *
   * @param nowUs    a monotonic microsecond timestamp.
   * @param displayRefreshHz the display's refresh rate, or 0 when unknown (locking is then not
   *        claimed, because there is no interval to compare against).
   * @param minIntervalUs intervals shorter than this are ignored rather than recorded. The render
   *        entry is called once per rendered player model, so several calls can fall inside one
   *        frame; a floor (a few milliseconds) collapses them to one sample so the window measures
   *        real frames instead of model draws. 0 records every interval.
   * @return the updated reading.
   */
  FramePacingReading onFrame(long long nowUs, int displayRefreshHz, long long minIntervalUs = 0) {
    if (lastUs_ != 0 && nowUs > lastUs_) {
      const long long interval = nowUs - lastUs_;
      if (interval >= minIntervalUs) {
        ring_[next_] = interval;
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
        ++sampleCount_;
      }
    }
    lastUs_ = nowUs;
    return reading(displayRefreshHz);
  }

  /** @return the current reading without feeding a frame. */
  FramePacingReading reading(int displayRefreshHz) const {
    FramePacingReading out;
    out.sampleCount = sampleCount_;
    if (count_ == 0) return out;

    long long sum = 0;
    long long worst = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      sum += ring_[i];
      if (ring_[i] > worst) worst = ring_[i];
    }
    out.averageFrameUs = sum / static_cast<long long>(count_);
    out.worstFrameUs = worst;

    if (displayRefreshHz > 0 && out.averageFrameUs > 0) {
      const long long expectedUs = 1000000LL / displayRefreshHz;
      // Late: a frame exceeded 1.5x the expected interval.
      out.jittering = worst * kLateDivisor > expectedUs * kLateFactor;
      // Locked: the average is within the tolerance band of the expected interval.
      const long long low = expectedUs * (100 - kLockTolerancePercent) / 100;
      const long long high = expectedUs * (100 + kLockTolerancePercent) / 100;
      out.vsyncLocked = out.averageFrameUs >= low && out.averageFrameUs <= high;
    } else {
      // No refresh rate: classify jitter relative to the average (a frame more than 1.5x the mean
      // is a hitch), which needs no absolute interval. A lock cannot be claimed without one.
      out.jittering = out.averageFrameUs > 0 &&
                      out.worstFrameUs * kLateDivisor > out.averageFrameUs * kLateFactor;
      out.vsyncLocked = false;
    }
    return out;
  }

  /** Clears the window, e.g. when a session starts. */
  void reset() {
    next_ = 0;
    count_ = 0;
    lastUs_ = 0;
    sampleCount_ = 0;
  }

private:
  long long ring_[kWindow] = {};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
  long long lastUs_ = 0;
  std::size_t sampleCount_ = 0;
};

} // namespace pl::render
