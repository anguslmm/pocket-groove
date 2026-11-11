// Tempo-synced send delay. Runs at HALF the engine sample rate on one mono
// int16 buffer of 16.5k samples (33 KB): 0.75 s of line at 44.1 kHz, 1.03 s
// at 32 kHz. Bandwidth: the 2x half-band resamplers pass up to ~0.2 * fs
// (8.8 kHz at 44.1 kHz, 6.4 kHz at 32 kHz), the tone filter sets the rest.
//
// The sound is a plain feedback delay, D = delay time, F = one-pole LP +
// one-pole HP ("tone") applied once per recirculation:
//     echo k = fb^(k-1) * F^k(in) at k*D            (mono sum, any width)
// Two ways of running it from the same buffer:
//   * PLAIN (line holds D): z[n] = F(in[n] + fb * z[n - D]); wet = z[n - D].
//     Mono device mode always runs this, so the full 0.75 s fits.
//   * PING-PONG (stereo only, line holds 2D): odd and even echoes kept apart,
//       a[n] = F(in[n] + fb * E[n]),  O[n] = a[n - D],  E[n] = fb * F2(a[n - 2D])
//     (F2 = a second instance of the tone filter on the 2D tap). O + E is
//     exactly the PLAIN signal; O pans one way, E the other, by `width`.
//     Used while 2D fits (D <= 0.375 s at 44.1 kHz); longer delays in stereo
//     run PLAIN, centred (same mono sum, no ping-pong for those times).
// Requests longer than the line are halved until they fit. Time and mode
// changes cross-fade old and new taps (and their feedback) over 25 ms.
#pragma once
#include "dsp_util.h"

namespace pg {
namespace engine_detail {

class Delay {
 public:
  static constexpr int kMaxBlock = 8;
  static constexpr int kBufLen = 16538 + 2 * kMaxBlock - 2;  // 0.75 s at 22.05 kHz + block margin
  static constexpr float kScale = 16384.0f;  // +-1.0 in -> int16 with 6 dB headroom

  void init(float halfRate);
  void reset();
  // Device mode: always PLAIN, no stereo output. Call before audio starts.
  void setMono(bool mono) { mono_ = mono; }

  // Desired delay in half-rate samples; halved until D fits the buffer.
  // Returns the delay actually used (half-rate samples).
  int setTime(float halfRateSamples);
  // fb: per-echo gain 0..0.95. lp/hp: one-pole coefficients (per pass). width 0..1.
  void setParams(float fb, float lpCoef, float hpCoef, float width);

  // n <= kMaxBlock half-rate samples. ADDS the wet signal to outL/outR.
  void process(const float* in, float* outL, float* outR, int n) { run<false>(in, outL, outR, n); }
  // Mono: ADDS the mono sum (L+R)/2 of the wet signal to `out`.
  void processMono(const float* in, float* out, int n) { run<true>(in, out, nullptr, n); }

  // True once the whole buffer is silent and the loop filters have drained.
  bool idle() const { return zeroRun_ >= kBufLen; }
  int currentDelay() const { return d_; }
  int targetDelay() const { return fading_ ? dNew_ : d_; }
  bool pingPong() const { return pp_; }

 private:
  void readTap(float* dst, int delay, int n) const;
  bool fitsPingPong(int d) const { return !mono_ && 2 * d <= kBufLen - 2 * kMaxBlock - 4; }
  template <bool kMono> void run(const float* in, float* outL, float* outR, int n);

  int16_t buf_[kBufLen];
  int w_;
  int d_, dNew_, dPending_;
  bool pp_, ppNew_, mono_;
  bool fading_;
  float fade_, fadeInc_;
  float fb_, lpK_, hpK_, width_;
  float lp_, hp_, lp2_, hp2_;
  int zeroRun_;
};

}  // namespace engine_detail
}  // namespace pg
