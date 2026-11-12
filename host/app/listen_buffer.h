// Listen (docs/DESKTOP.md): the connected device's USB audio input, played
// through the app's output. The device's audio clock and the computer's
// output clock are independent (measured: the device runs about +61 ppm), so
// a plain FIFO would slowly fill up or run dry. DriftBuffer is the rate match
// between the two:
//
//   producer (the capture callback)   push(): interleaved stereo int16 into a
//                                     lock-free ring; a block that does not
//                                     fit is dropped (counted), never blocks.
//   consumer (the output callback)    mixInto(): reads the ring at a variable
//                                     rate 1 + u (4-point Hermite
//                                     interpolation) and ADDS the result to
//                                     the engine's output, saturating.
//
// u comes from a PI controller that holds the fill level (input frames
// waiting, measured at each output callback and low-passed over ~1 s) at a
// target of one output buffer + one capture buffer + 3 ms. u is limited to
// +-2000 ppm (3.5 cents: inaudible), far more than two crystals disagree by.
// Closed loop: natural frequency 0.1 rad/s, critically damped, so a 500 ppm
// clock difference costs a transient of about 2 ms of fill and is matched
// within a minute; tests/test_desktop_listen.cpp simulates both clocks
// (+-500 ppm, callback jitter, an hour) and checks there is no underrun,
// overflow or resync and the fill stays bounded.
//
// Start and recovery: after restart() (Listen switched on, device re-plugged)
// the consumer drops whatever is stale, waits until the target fill is there
// (priming), then fades in over 256 frames. If the input stops (unplug) the
// consumer counts one underrun, plays silence and primes again. If the fill
// ever runs far above target (the output stalled), it skips back to target
// (a resync) instead of carrying the latency forever.
//
// Threads: exactly one producer and one consumer; restart()/setTarget() from
// a third (main) thread go through atomics the consumer picks up. No locks,
// no allocation, no system calls on either side. Pure C++ (no SDL), so the
// unit test runs it against simulated clocks.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace desk {

class DriftBuffer {
 public:
  static constexpr uint32_t kCap = 1u << 14;          // frames: 371 ms at 44.1 kHz
  static constexpr double kMaxPpm = 2000.0;            // |u| limit
  static constexpr int kFadeFrames = 256;

  struct Stats {
    uint32_t underruns, overflows, resyncs, primes;
    float fillFrames;      // smoothed fill (input frames)
    float ppm;             // current rate correction u, in ppm (+ = reads faster: the input runs fast)
    float clockPpm;        // its integral part: the estimated clock difference
    bool primed;
    int target;
    uint64_t pushed;       // frames accepted from the producer
  };

  // ---- main thread ------------------------------------------------------------------
  // Target fill in input frames (one output buffer + one capture buffer + 3 ms).
  void setTarget(int frames) {
    if (frames < 16) frames = 16;
    if (frames > int(kCap / 4)) frames = int(kCap / 4);
    target_.store(frames, std::memory_order_relaxed);
  }
  static int targetFor(int outputFrames, int captureFrames, int rate) {
    return outputFrames + captureFrames + rate * 3 / 1000 + 4;
  }
  // Drop what is waiting and start over (prime, fade in). Picked up by the
  // consumer on its next call.
  void restart() { epoch_.fetch_add(1, std::memory_order_release); }
  // Largest |sample| the producer delivered since the last call (silence detection).
  int takePeak() { return peak_.exchange(0, std::memory_order_relaxed); }
  Stats stats() const {
    Stats s;
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overflows = overflows_.load(std::memory_order_relaxed);
    s.resyncs = resyncs_.load(std::memory_order_relaxed);
    s.primes = primes_.load(std::memory_order_relaxed);
    s.fillFrames = fillPub_.load(std::memory_order_relaxed);
    s.ppm = ppmPub_.load(std::memory_order_relaxed);
    s.clockPpm = clockPub_.load(std::memory_order_relaxed);
    s.primed = primedPub_.load(std::memory_order_relaxed);
    s.target = target_.load(std::memory_order_relaxed);
    s.pushed = pushed_.load(std::memory_order_relaxed);
    return s;
  }

  // Input frames waiting right now (any thread; a snapshot).
  uint32_t waiting() const {
    return uint32_t(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire));
  }

  // ---- producer -------------------------------------------------------------------
  void push(const int16_t* lr, uint32_t frames) {
    if (!frames) return;
    int pk = 0;
    for (uint32_t i = 0; i < frames * 2; ++i) {
      const int a = lr[i] < 0 ? -int(lr[i]) : int(lr[i]);
      if (a > pk) pk = a;
    }
    if (pk > peak_.load(std::memory_order_relaxed)) peak_.store(pk, std::memory_order_relaxed);
    const uint64_t w = w_.load(std::memory_order_relaxed);
    const uint64_t r = r_.load(std::memory_order_acquire);
    // 4 frames behind the read position stay intact: the interpolator looks one back.
    if (w + frames + 4 - r > kCap) {
      overflows_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t at = uint32_t(w % kCap);
    const uint32_t first = frames < kCap - at ? frames : kCap - at;
    std::memcpy(buf_ + size_t(at) * 2, lr, size_t(first) * 4);
    if (first < frames) std::memcpy(buf_, lr + size_t(first) * 2, size_t(frames - first) * 4);
    w_.store(w + frames, std::memory_order_release);
    pushed_.fetch_add(frames, std::memory_order_relaxed);
  }

  // ---- consumer -------------------------------------------------------------------
  // Adds `frames` resampled frames to out (interleaved stereo int16, saturating).
  // rate = the output's sample rate (for the controller's time base).
  void mixInto(int16_t* out, int frames, int rate) {
    const uint32_t ep = epoch_.load(std::memory_order_acquire);
    const uint64_t w = w_.load(std::memory_order_acquire);
    if (ep != seenEpoch_) {   // restart: drop stale input, prime again
      seenEpoch_ = ep;
      r_.store(w, std::memory_order_release);
      primed_ = false;
      integ_ = 0.0;
      u_ = 0.0;
      frac_ = 0.0;
    }
    const int target = target_.load(std::memory_order_relaxed);
    uint64_t r = r_.load(std::memory_order_relaxed);
    const double avail = double(w - r) - frac_;
    if (!primed_) {
      if (w - r < uint64_t(target) + 3) {
        publish(float(avail), target);
        return;   // still filling
      }
      // Start at exactly the target fill (anything above is older than needed).
      r = w - uint64_t(target);
      frac_ = 0.0;
      primed_ = true;
      fade_ = 0;
      fill_ = double(target);
      // integ_ stays: after a dropout the clocks have not changed
      primes_.fetch_add(1, std::memory_order_relaxed);
    } else if (avail > double(4 * target + 2 * frames)) {
      // The output stalled (or the input burst): back to the target fill.
      r = w - uint64_t(target);
      frac_ = 0.0;
      fill_ = double(target);
      resyncs_.fetch_add(1, std::memory_order_relaxed);
    }
    // PI controller on the smoothed fill (seconds of output as the time base).
    const double dt = double(frames) / double(rate);
    const double fillNow = double(w - r) - frac_;
    const double a = dt / (1.0 + dt);   // ~1 s low-pass: removes the callbacks' sawtooth and jitter
    fill_ += (fillNow - fill_) * a;
    const double err = (fill_ - double(target)) / double(rate);   // seconds of input in excess
    // s^2 + Kp s + Ki: wn = 0.1 rad/s, zeta = 1
    constexpr double kKp = 0.2, kKi = 0.01, kLim = kMaxPpm * 1e-6;
    integ_ += err * dt;
    if (integ_ * kKi > kLim) integ_ = kLim / kKi;
    if (integ_ * kKi < -kLim) integ_ = -kLim / kKi;
    u_ = kKp * err + kKi * integ_;
    if (u_ > kLim) u_ = kLim;
    if (u_ < -kLim) u_ = -kLim;
    const double step = 1.0 + u_;

    // Enough input for this whole call? (the last frame reads index floor(pos) + 2)
    const double need = frac_ + double(frames - 1) * step + 3.0;
    int n = frames;
    bool under = false;
    if (double(w - r) < need) {
      under = true;
      const double have = double(w - r) - 3.0 - frac_;
      n = have <= 0.0 ? 0 : int(have / step);   // what can be played, then silence
      if (n > frames) n = frames;
    }
    for (int i = 0; i < n; ++i) {
      const uint32_t i0 = uint32_t(r % kCap);
      const int16_t* xm1 = buf_ + size_t((i0 + kCap - 1) % kCap) * 2;
      const int16_t* x0 = buf_ + size_t(i0) * 2;
      const int16_t* x1 = buf_ + size_t((i0 + 1) % kCap) * 2;
      const int16_t* x2 = buf_ + size_t((i0 + 2) % kCap) * 2;
      const float t = float(frac_);
      const float g = fade_ < kFadeFrames ? float(fade_++) / float(kFadeFrames) : 1.0f;
      for (int c = 0; c < 2; ++c) {
        const float y = hermite(float(xm1[c]), float(x0[c]), float(x1[c]), float(x2[c]), t) * g;
        const int v = int(out[2 * i + c]) + int(std::lrint(y));
        out[2 * i + c] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
      }
      frac_ += step;
      const double whole = std::floor(frac_);
      frac_ -= whole;
      r += uint64_t(whole);
    }
    r_.store(r, std::memory_order_release);
    if (under) {
      underruns_.fetch_add(1, std::memory_order_relaxed);
      primed_ = false;   // silence for the rest, then prime and fade in again
    }
    publish(float(fill_), target);
  }

  static float hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }

 private:
  void publish(float fill, int target) {
    fillPub_.store(primed_ ? fill : 0.0f, std::memory_order_relaxed);
    ppmPub_.store(float(u_ * 1e6), std::memory_order_relaxed);
    clockPub_.store(float(integ_ * 0.01 * 1e6), std::memory_order_relaxed);
    primedPub_.store(primed_, std::memory_order_relaxed);
    (void)target;
  }

  int16_t buf_[kCap * 2] = {};
  std::atomic<uint64_t> w_{0}, r_{0}, pushed_{0};
  std::atomic<int> target_{1024}, peak_{0};
  std::atomic<uint32_t> epoch_{0};
  std::atomic<uint32_t> underruns_{0}, overflows_{0}, resyncs_{0}, primes_{0};
  std::atomic<float> fillPub_{0.0f}, ppmPub_{0.0f}, clockPub_{0.0f};
  std::atomic<bool> primedPub_{false};
  // consumer-only state
  uint32_t seenEpoch_ = 0xFFFFFFFFu;
  bool primed_ = false;
  int fade_ = 0;
  double frac_ = 0.0, fill_ = 0.0, integ_ = 0.0, u_ = 0.0;
};

}  // namespace desk
