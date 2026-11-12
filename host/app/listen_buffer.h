// Listen (docs/DESKTOP.md): the connected device's USB audio input, played
// through the app's output. The device's audio clock and the computer's
// output clock are independent (measured: the device runs about +61 ppm), so
// a plain FIFO would slowly fill up or run dry. DriftBuffer is the rate match
// between the two:
//
//   producer (the capture callback)   push(): interleaved stereo int16 at the
//                                     capture rate (capture_convert.h has
//                                     already turned whatever SDL delivered
//                                     into that) into a lock-free ring; a
//                                     block that does not fit is dropped
//                                     (counted), never blocks.
//   consumer (the output callback)    mixInto(): reads the ring at a variable
//                                     step ratio * (1 + u), ratio = capture
//                                     rate / output rate (1 when SDL already
//                                     converted; 48000/44100 if the input came
//                                     at 48 kHz), 4-point Hermite
//                                     interpolation, and ADDS the result to
//                                     the engine's output, saturating
//                                     (clipped samples are counted).
//
// u comes from a PI controller that holds the fill level at a setpoint. u is
// limited to +-2000 ppm (3.5 cents), far more than two crystals disagree by.
// The fill it sees is time-consistent: at the first of the output's
// back-to-back callbacks, (input frames so far + capture rate x time since the
// input's latest burst began) - read position, i.e. both sides' bursts
// smoothed out by their timestamps (steady_clock, or the simulation's time),
// low-passed twice. Sampling the raw fill at each callback instead aliases two
// burst patterns into a slow wander that the loop follows as a wobble of the
// rate. Closed loop: critically damped, natural frequency 0.1 rad/s for the
// first 20 s (a 500 ppm clock difference is caught within a few ms of fill),
// then narrowing to 0.02 rad/s by 60 s, so what is left of the measurement
// noise moves the rate by a few ppm (a 1 kHz tone through it: THD+N below
// -70 dB in test_desktop_listen).
//
// The target is MEASURED, not taken from the callback sizes SDL reports. On
// macOS with Homebrew's SDL2 (sdl2-compat on SDL3) the obtained `samples` is
// only our own request echoed back; the real deliveries come from AudioQueue
// buffers in bursts (several capture callbacks back to back, several output
// callbacks back to back, the output also resampled to the Mac's 48 kHz).
// The rate match of 2026-11-12 sized its buffer from the reported 256 + 256
// frames, far below the real swing of the fill, so it ran dry tens of times a
// second: silence, re-prime, fade in, over and over (the "very distorted"
// Listen; in simulation, 2583 underruns a minute with 1024-frame bursts on
// both sides). Now the consumer watches the pattern itself: the most input
// the output consumes between two arrivals (a run) and the largest output
// burst. The target is at least
//     longest run + largest output burst + 3 ms
// (runs over 100 ms are gaps or stalls, not a pattern, and are not learnt).
// It only grows during a session (restart() starts over); a target that grows
// while playing is reached by reading up to 1000 ppm slower, as feed-forward
// that glides in and out over 0.5 s, so neither a pitch step nor the clock
// estimate sees it.
//
// Start and recovery: after restart() (Listen switched on, device re-plugged)
// the consumer drops whatever is stale, watches the delivery pattern for at
// least 0.5 s and 8 arrivals, waits until the target fill is there
// (priming), then fades in over 256 frames. If the input stops (unplug) the
// consumer counts one underrun, plays silence and primes again. If the fill
// ever runs far above target (the output stalled), it skips back to target
// (a resync) instead of carrying the latency forever.
//
// Threads: exactly one producer and one consumer; restart()/setTarget()/
// setRates() from a third (main) thread go through atomics the consumer
// picks up. No locks, no allocation, no system calls on either side. Pure
// C++ (no SDL), so the unit test runs it against simulated clocks.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace desk {

class DriftBuffer {
 public:
  static constexpr uint32_t kCap = 1u << 15;          // frames: 743 ms at 44.1 kHz
  static constexpr int kMaxTarget = int(kCap / 4);     // 186 ms at 44.1 kHz
  static constexpr double kMaxPpm = 2000.0;            // |u| limit
  static constexpr int kFadeFrames = 256;
  static constexpr double kObserveSeconds = 0.5;       // pattern watched before the first prime
  static constexpr uint32_t kObserveArrivals = 8;
  static constexpr double kPatternMaxSeconds = 0.1;    // longer = a gap or a stall, not learnt

  struct Stats {
    uint32_t underruns, overflows, resyncs, primes;
    float fillFrames;      // smoothed fill (input frames)
    float ppm;             // current rate correction u, in ppm (+ = reads faster: the input runs fast)
    float clockPpm;        // its integral part: the estimated clock difference
    bool primed;
    int target;            // the target in force (input frames): max(base, measured)
    int baseTarget;        // what setTarget() asked
    int maxArrival;        // largest input arrival between two output callbacks (input frames)
    int maxRun;            // most input the output consumed between two arrivals (input frames)
    int maxCall;           // largest output burst: back-to-back callbacks (output frames)
    double ratio;          // nominal step: capture rate / output rate
    uint64_t pushed;       // frames accepted from the producer
    uint64_t clipped;      // output samples the mix saturated
  };

  // ---- main thread ------------------------------------------------------------------
  // Base target fill in input frames (one output buffer + one capture buffer + 3 ms).
  void setTarget(int frames) {
    if (frames < 16) frames = 16;
    if (frames > kMaxTarget) frames = kMaxTarget;
    target_.store(frames, std::memory_order_relaxed);
  }
  static int targetFor(int outputFrames, int captureFrames, int rate) {
    return outputFrames + captureFrames + rate * 3 / 1000 + 4;
  }
  // The capture and output rates (Hz), taken up at the next restart().
  void setRates(double inRate, double outRate) {
    ratioReq_.store(inRate > 0 && outRate > 0 ? inRate / outRate : 1.0, std::memory_order_relaxed);
  }
  // Drop what is waiting and start over (watch, prime, fade in). Picked up by
  // the consumer on its next call.
  void restart() { epoch_.fetch_add(1, std::memory_order_release); }
  // Largest |sample| the producer delivered since the last call (silence detection).
  int takePeak() { return peak_.exchange(0, std::memory_order_relaxed); }
  // Largest |device sample| as mixed in, and largest |output sample| after the
  // mix, since the last call.
  void takeMixPeaks(int* device, int* mixed) {
    *device = devPeak_.exchange(0, std::memory_order_relaxed);
    *mixed = mixPeak_.exchange(0, std::memory_order_relaxed);
  }
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
    s.target = targetPub_.load(std::memory_order_relaxed);
    s.baseTarget = target_.load(std::memory_order_relaxed);
    s.maxArrival = maxInPub_.load(std::memory_order_relaxed);
    s.maxRun = maxRunPub_.load(std::memory_order_relaxed);
    s.maxCall = maxCallPub_.load(std::memory_order_relaxed);
    s.ratio = ratioPub_.load(std::memory_order_relaxed);
    s.pushed = pushed_.load(std::memory_order_relaxed);
    s.clipped = clipped_.load(std::memory_order_relaxed);
    return s;
  }

  // The stored frame at absolute input index i (tests: what the producer wrote).
  void peekFrame(uint64_t i, int16_t* lr) const {
    lr[0] = buf_[size_t(i % kCap) * 2];
    lr[1] = buf_[size_t(i % kCap) * 2 + 1];
  }
  // Input frames waiting right now (any thread; a snapshot).
  uint32_t waiting() const {
    return uint32_t(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire));
  }

  // ---- producer -------------------------------------------------------------------
  // now = seconds on any steady clock (the same one as mixInto's); < 0 = steady_clock.
  void push(const int16_t* lr, uint32_t frames, double now = -1.0) {
    if (!frames) return;
    if (now < 0.0) now = clockSeconds();
    if (!pushedOnce_ || now - lastPushT_ > kBurstGap) burstT_ = now;   // a new burst starts
    pushedOnce_ = true;
    lastPushT_ = now;
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
    // (burst start time, input frames so far) for the consumer's fill estimate (a seqlock)
    const uint32_t q = seq_.load(std::memory_order_relaxed);
    seq_.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    arrT_.store(burstT_, std::memory_order_relaxed);
    arrW_.store(w + frames, std::memory_order_relaxed);
    seq_.store(q + 2, std::memory_order_release);
  }

  // ---- consumer -------------------------------------------------------------------
  // Adds `frames` resampled frames to out (interleaved stereo int16, saturating).
  // rate = the output's sample rate (the controller's time base); now as for push().
  void mixInto(int16_t* out, int frames, int rate, double now = -1.0) {
    if (now < 0.0) now = clockSeconds();
    const bool burstStart = !calledOnce_ || now - lastCallT_ > kBurstGap;   // first of back-to-back calls
    calledOnce_ = true;
    lastCallT_ = now;
    const uint32_t ep = epoch_.load(std::memory_order_acquire);
    const uint64_t w = w_.load(std::memory_order_acquire);
    if (ep != seenEpoch_) {   // restart: drop stale input, watch and prime again
      seenEpoch_ = ep;
      r_.store(w, std::memory_order_release);
      primed_ = false;
      integ_ = 0.0;
      u_ = 0.0;
      frac_ = 0.0;
      locked_ = 0.0;
      ratio_ = ratioReq_.load(std::memory_order_relaxed);
      lastW_ = w;
      run_ = 0.0;
      maxIn_ = maxRun_ = 0.0;
      maxCall_ = outBurst_ = 0;
      arrivals_ = 0;
      observed_ = 0;
      sinceAvg_ = 0.0;
      sinceN_ = 0;
      leadSet_ = false;
    }
    const double inRate = double(rate) * ratio_;

    // The delivery pattern, as this side sees it.
    const double patternMax = kPatternMaxSeconds * inRate;
    const bool arrived = w != lastW_;
    if (arrived) {
      const double dw = double(w - lastW_);
      if (dw <= patternMax && dw > maxIn_) maxIn_ = dw;
      if (run_ <= patternMax && run_ > maxRun_) maxRun_ = run_;
      run_ = 0.0;
      lastW_ = w;
      ++arrivals_;
    }
    run_ += double(frames) * ratio_;
    outBurst_ = burstStart ? frames : outBurst_ + frames;
    if (outBurst_ <= int(kPatternMaxSeconds * rate) && outBurst_ > maxCall_) maxCall_ = outBurst_;
    observed_ += uint64_t(frames);
    int target = target_.load(std::memory_order_relaxed);
    const double measured = maxRun_ + std::ceil(double(maxCall_) * ratio_) + inRate * 0.003 + 4.0;
    if (measured > double(target)) target = int(measured);
    if (target > kMaxTarget) target = kMaxTarget;

    // At the first of the output's back-to-back callbacks: the producer's
    // latest burst (start time, frames so far) and the time since it began.
    double sinceNow = -1.0;
    uint64_t wA = 0;
    if (burstStart) {
      double tA;
      if (arrival(&tA, &wA)) {
        sinceNow = now - tA;
        if (sinceNow < 0.0) sinceNow = 0.0;
        if (sinceNow > kPatternMaxSeconds) sinceNow = kPatternMaxSeconds;
        sinceAvg_ += (sinceNow - sinceAvg_) * (sinceN_ < 50 ? 1.0 / double(++sinceN_) : 0.02);
      }
    }

    uint64_t r = r_.load(std::memory_order_relaxed);
    if (!primed_) {
      if (w - r > kCap / 2) {   // nothing reads while priming: keep the ring from filling up
        r = w - kCap / 2;
        r_.store(r, std::memory_order_release);
      }
      const bool watched = observed_ >= uint64_t(kObserveSeconds * rate) && arrivals_ >= kObserveArrivals;
      if (!watched || w - r < uint64_t(target) + 3) {
        publish(0.0f, target);
        return;   // still watching / filling
      }
      // Start at exactly the target fill (anything above is older than needed).
      r = w - uint64_t(target);
      frac_ = 0.0;
      primed_ = true;
      fade_ = 0;
      fill_ = setpoint_ = double(target);
      err1_ = err2_ = sinceSample_ = 0.0;
      if (!leadSet_) lead_ = inRate * sinceAvg_;   // once per restart: a re-prime after a gap keeps it
      uff_ = 0.0;
      leadSet_ = true;
      // integ_ stays: after a dropout the clocks have not changed
      primes_.fetch_add(1, std::memory_order_relaxed);
    } else if (double(w - r) - frac_ > double(4 * target) + 2.0 * double(frames) * ratio_) {
      // The output stalled (or the input burst): back to the target fill.
      r = w - uint64_t(target);
      frac_ = 0.0;
      fill_ = setpoint_ = double(target);
      err1_ = err2_ = 0.0;
      resyncs_.fetch_add(1, std::memory_order_relaxed);
    }
    // PI controller on the smoothed fill (seconds of output as the time base).
    // Its bandwidth narrows once locked: wn = 0.1 rad/s for the first 20 s
    // (a 500 ppm clock difference is caught within a few ms of fill), then
    // down to 0.02 rad/s by 60 s, so the noise of the fill measurement (the
    // callbacks' bursts, sampled at the output's callbacks) no longer moves
    // the rate: what is left of it is a few ppm, a frequency modulation far
    // below hearing (THD+N of a 1 kHz tone < -60 dB in the tests).
    const double dt = double(frames) / double(rate);
    locked_ += dt;
    const double wn = locked_ < 20.0 ? 0.1 : locked_ > 60.0 ? 0.02 : 0.1 - 0.08 * (locked_ - 20.0) / 40.0;
    // A target that grew while playing (a burst bigger than any seen so far)
    // is approached at 1000 ppm, read slower by exactly that (feed-forward),
    // so the controller, and its estimate of the clocks, never see it.
    // The feed-forward itself changes smoothly (from 0 to 1000 ppm and back
    // over 0.5 s): a pitch glide of at most 1.7 cents, never a step.
    constexpr double kRamp = 1000e-6, kSlew = kRamp / 0.5;
    {
      const double remaining = double(target) - setpoint_;   // frames still to add (>= 0: targets only grow)
      // as fast as possible, but slow enough to stop on the target with the slew limit
      double want = remaining > 0.0 ? -std::sqrt(2.0 * kSlew * remaining / inRate) : 0.0;
      if (want < -kRamp) want = -kRamp;
      const double maxDelta = kSlew * dt;
      if (want > uff_ + maxDelta) want = uff_ + maxDelta;
      if (want < uff_ - maxDelta) want = uff_ - maxDelta;
      uff_ = want;
      setpoint_ += -uff_ * inRate * dt;
      if (setpoint_ > double(target)) setpoint_ = double(target);
    }
    const double uff = uff_;
    // The error (fill - setpoint), low-passed twice, corner 10 x wn (1 s, then 5 s).
    // The fill is taken with both sides' bursts smoothed out by their
    // timestamps: at the first of the output's back-to-back callbacks, the
    // input as if it flowed continuously since its last burst began
    // (frames so far + rate x time since, at most 100 ms) minus the read
    // position. Sampling the raw fill instead aliases the two delivery
    // patterns into a slow wander (a 1024-frame burst slipping past the
    // output's callbacks at the clocks' 61 ppm took 95 s per cycle), which
    // the loop would follow as a slow wobble of the rate.
    // `lead` (set at the prime) is how far that runs ahead of the real fill
    // on average, so the setpoint stays in real-fill terms.
    sinceSample_ += dt;
    if (sinceNow >= 0.0) {
      {
        const double fillNow = double(wA) + inRate * sinceNow - (double(r) + frac_) - lead_;
        const double a = sinceSample_ / (0.1 / wn + sinceSample_);
        sinceSample_ = 0.0;
        err1_ += (fillNow - setpoint_ - err1_) * a;
        err2_ += (err1_ - err2_) * a;
      }
    }
    fill_ = setpoint_ + err2_;
    const double err = err2_ / inRate;   // seconds of input in excess
    // s^2 + Kp s + Ki, zeta = 1: Kp = 2 wn, Ki = wn^2; the integral is kept in rate units, so
    // the gains can change without a jump in u
    constexpr double kLim = kMaxPpm * 1e-6;
    integ_ += wn * wn * err * dt;
    if (integ_ > kLim) integ_ = kLim;
    if (integ_ < -kLim) integ_ = -kLim;
    u_ = 2.0 * wn * err + integ_ + uff;
    if (u_ > kLim) u_ = kLim;
    if (u_ < -kLim) u_ = -kLim;
    const double step = ratio_ * (1.0 + u_);

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
    int devPk = 0, mixPk = 0;
    uint32_t clipped = 0;
    for (int i = 0; i < n; ++i) {
      const uint32_t i0 = uint32_t(r % kCap);
      const int16_t* xm1 = buf_ + size_t((i0 + kCap - 1) % kCap) * 2;
      const int16_t* x0 = buf_ + size_t(i0) * 2;
      const int16_t* x1 = buf_ + size_t((i0 + 1) % kCap) * 2;
      const int16_t* x2 = buf_ + size_t((i0 + 2) % kCap) * 2;
      const float t = float(frac_);
      const float g = fade_ < kFadeFrames ? float(fade_++) / float(kFadeFrames) : 1.0f;
      for (int c = 0; c < 2; ++c) {
        const int y = int(std::lrint(hermite(float(xm1[c]), float(x0[c]), float(x1[c]), float(x2[c]), t) * g));
        const int ay = y < 0 ? -y : y;
        if (ay > devPk) devPk = ay;
        int v = int(out[2 * i + c]) + y;
        if (v > 32767) { v = 32767; ++clipped; }
        if (v < -32768) { v = -32768; ++clipped; }
        const int av = v < 0 ? -v : v;
        if (av > mixPk) mixPk = av;
        out[2 * i + c] = int16_t(v);
      }
      frac_ += step;
      const double whole = std::floor(frac_);
      frac_ -= whole;
      r += uint64_t(whole);
    }
    r_.store(r, std::memory_order_release);
    if (clipped) clipped_.fetch_add(clipped, std::memory_order_relaxed);
    if (devPk > devPeak_.load(std::memory_order_relaxed)) devPeak_.store(devPk, std::memory_order_relaxed);
    if (mixPk > mixPeak_.load(std::memory_order_relaxed)) mixPeak_.store(mixPk, std::memory_order_relaxed);
    if (under) {
      underruns_.fetch_add(1, std::memory_order_relaxed);
      primed_ = false;   // silence for the rest, then prime and fade in again
    }
    publish(float(fill_), target);
  }

  static double clockSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  static float hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }

 private:
  static constexpr double kBurstGap = 0.001;   // callbacks closer than this are one burst
  bool arrival(double* t, uint64_t* w) const {   // the producer's latest (burst start, frames), consistent
    for (int i = 0; i < 4; ++i) {
      const uint32_t q0 = seq_.load(std::memory_order_acquire);
      if (q0 & 1) continue;
      *t = arrT_.load(std::memory_order_relaxed);
      *w = arrW_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == q0) return q0 != 0;
    }
    return false;
  }
  void publish(float fill, int target) {
    fillPub_.store(primed_ ? fill : 0.0f, std::memory_order_relaxed);
    ppmPub_.store(float(u_ * 1e6), std::memory_order_relaxed);
    clockPub_.store(float(integ_ * 1e6), std::memory_order_relaxed);
    primedPub_.store(primed_, std::memory_order_relaxed);
    targetPub_.store(target, std::memory_order_relaxed);
    maxInPub_.store(int(maxIn_), std::memory_order_relaxed);
    maxRunPub_.store(int(maxRun_), std::memory_order_relaxed);
    maxCallPub_.store(maxCall_, std::memory_order_relaxed);
    ratioPub_.store(ratio_, std::memory_order_relaxed);
  }

  int16_t buf_[kCap * 2] = {};
  std::atomic<uint64_t> w_{0}, r_{0}, pushed_{0}, clipped_{0};
  std::atomic<int> target_{1024}, peak_{0}, devPeak_{0}, mixPeak_{0};
  std::atomic<uint32_t> epoch_{0};
  std::atomic<uint32_t> underruns_{0}, overflows_{0}, resyncs_{0}, primes_{0};
  std::atomic<float> fillPub_{0.0f}, ppmPub_{0.0f}, clockPub_{0.0f};
  std::atomic<bool> primedPub_{false};
  std::atomic<int> targetPub_{0}, maxInPub_{0}, maxRunPub_{0}, maxCallPub_{0};
  std::atomic<double> ratioReq_{1.0}, ratioPub_{1.0};
  std::atomic<uint32_t> seq_{0};
  std::atomic<double> arrT_{0.0};
  std::atomic<uint64_t> arrW_{0};
  // producer-only state
  bool pushedOnce_ = false;
  double lastPushT_ = 0.0, burstT_ = 0.0;
  // consumer-only state
  uint32_t seenEpoch_ = 0xFFFFFFFFu;
  bool primed_ = false;
  int fade_ = 0;
  double frac_ = 0.0, err1_ = 0.0, err2_ = 0.0, fill_ = 0.0, integ_ = 0.0, u_ = 0.0, ratio_ = 1.0, locked_ = 0.0, setpoint_ = 0.0, sinceSample_ = 0.0;
  uint64_t lastW_ = 0, observed_ = 0;
  double run_ = 0.0, maxIn_ = 0.0, maxRun_ = 0.0;
  int maxCall_ = 0, outBurst_ = 0, sinceN_ = 0;
  double sinceAvg_ = 0.0, lead_ = 0.0, uff_ = 0.0;
  bool leadSet_ = false;
  bool calledOnce_ = false;
  double lastCallT_ = 0.0;
  uint32_t arrivals_ = 0;
};

}  // namespace desk
