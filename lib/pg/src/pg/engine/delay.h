// Tempo-synced send delay. Runs at HALF the engine sample rate on one mono
// int16 buffer (66 KB, the size of 0.75 s of full-rate int16): 1.5 s of line
// at 44.1 kHz, 2.07 s at 32 kHz.
//
// Topology (D = delay time):
//     y[n]   = F(in[n] + fb^2 * y[n - 2D])        F = one-pole LP + one-pole HP ("tone")
//     odd echoes  (D, 3D, ...)  = y[n - D]        -> panned left by `width`
//     even echoes (2D, 4D, ...) = fb * y[n - 2D]  -> panned right by `width`
// Summed to mono this is exactly a plain feedback delay (echo k has gain
// fb^(k-1)) at any width, which matters because the device's codec is mono;
// panned apart it is a ping-pong, from a single buffer. The line
// has to hold 2D, so D may be up to 0.75 s at 44.1 kHz (a dotted 1/8 at the
// 60 BPM minimum just fits); longer requests are halved until they fit.
// Time changes cross-fade both taps over 25 ms instead of jumping.
#pragma once
#include "dsp_util.h"

namespace pg {
namespace engine_detail {

class Delay {
 public:
  static constexpr int kBufLen = 33100;      // samples at half rate: 2 * 16538 (0.75 s) + block margin
  static constexpr int kMaxBlock = 8;
  static constexpr float kScale = 16384.0f;  // +-1.0 in -> int16 with 6 dB headroom

  void init(float halfRate);
  void reset();

  // Desired delay in half-rate samples; halved until 2*D fits the buffer.
  // Returns the delay actually used (half-rate samples).
  int setTime(float halfRateSamples);
  // fb: per-echo gain 0..0.95. lp/hp: one-pole coefficients. width 0..1.
  void setParams(float fb, float lpCoef, float hpCoef, float width);

  // n <= kMaxBlock half-rate samples. ADDS the wet signal to outL/outR.
  void process(const float* in, float* outL, float* outR, int n) { run<false>(in, outL, outR, n); }
  // Mono device mode: ADDS only the mono sum (L+R)/2 = e1 + e2 to `out`
  // (no width law). Bit-for-bit the same line state as process().
  void processMono(const float* in, float* out, int n) { run<true>(in, out, nullptr, n); }

  // True once the whole buffer is silent and the loop filter has drained.
  bool idle() const { return zeroRun_ >= kBufLen; }
  int currentDelay() const { return d_; }
  int targetDelay() const { return fading_ ? dNew_ : d_; }

 private:
  void readTap(float* dst, int delay, int n) const;
  template <bool kMono> void run(const float* in, float* outL, float* outR, int n);

  int16_t buf_[kBufLen];
  int w_;
  int d_, dNew_, dPending_;
  bool fading_;
  float fade_, fadeInc_;
  float fb_, lpK_, hpK_, width_;
  float lp_, hp_;
  int zeroRun_;
};

}  // namespace engine_detail
}  // namespace pg
