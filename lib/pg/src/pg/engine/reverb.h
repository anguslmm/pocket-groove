// Send reverb: 6 series input allpasses (diffusion) into an 8-line feedback
// delay network with a Hadamard matrix, per-line damping, and 8 output taps
// per tap set spread inside the lines (so the output is dense within ~15 ms
// instead of waiting for the first recirculation). The two tap sets are used
// as mid and side (L = M + 0.75 S, R = M - 0.75 S) so the mono sum the device
// plays is exactly the mid set: no level loss, no comb colouring.
//
// Runs at HALF the engine sample rate, on int16 storage: 13.5k samples =
// 27 KB for 0.61 s of total delay (a float full-rate design would get 0.16 s
// from the same RAM, and modal density is what keeps a small reverb from
// sounding metallic). The FDN lines quantise with first-order error feedback
// down to 1 LSB (smooth decay) and truncate toward zero below that; the
// allpasses always truncate toward zero. Everything decays to exact zero, at
// which point idle() lets the engine skip the whole effect.
#pragma once
#include "../config.h"
#include "dsp_util.h"

namespace pg {
namespace engine_detail {

// The reverb's line lengths (Reverb::init lays them out in this order).
namespace reverb_len {
constexpr int kMaxBlock = 8;
constexpr int kNumAp = 6;
constexpr int kNumLines = 8;
// Lengths in samples at 22.05 kHz (the 44.1 kHz engine rate). Every rate scales
// them by halfRate / 22,050 (each rounded up to a prime), so the room is the same
// size in seconds at 32, 44.1 and 48 kHz; above PG_MAX_SAMPLE_RATE (the pool's
// size) they stop growing.
constexpr int kApLen[kNumAp] = {37, 53, 89, 139, 211, 307};
constexpr int kLineLen[kNumLines] = {887, 1039, 1213, 1409, 1627, 1877, 2141, 2423};
constexpr bool isPrime(int n) {
  if (n < 4) return n > 1;
  if ((n & 1) == 0) return false;
  for (int d = 3; d * d <= n; d += 2)
    if (n % d == 0) return false;
  return true;
}
constexpr int nextPrime(int n) {
  while (!isPrime(n)) ++n;
  return n;
}
constexpr float kMinScale = 0.25f;
constexpr float kMaxScale = 0.5f * float(kMaxSampleRate) / 22050.0f;   // >= 1
constexpr float scaleFor(float halfRate) {
  const float s = halfRate / 22050.0f;
  return s > kMaxScale ? kMaxScale : (s < kMinScale ? kMinScale : s);
}
constexpr int apLen(int i, float scale) {
  const int len = nextPrime(int(float(kApLen[i]) * scale + 0.5f));
  return len < kMaxBlock + 3 ? nextPrime(kMaxBlock + 3) : len;
}
constexpr int lineLen(int i, float scale) { return nextPrime(int(float(kLineLen[i]) * scale + 0.5f)); }
// What every line together needs at `scale`.
constexpr int poolFor(float scale) {
  int n = 0;
  for (int i = 0; i < kNumAp; ++i) n += apLen(i, scale);
  for (int i = 0; i < kNumLines; ++i) n += lineLen(i, scale);
  return n;
}
}  // namespace reverb_len

class Reverb {
 public:
  static constexpr int kMaxBlock = 8;
  static constexpr int kNumAp = 6;
  static constexpr int kNumLines = 8;
  static constexpr float kScale = 16384.0f;
  // int16 samples: exactly what the prime-rounded lengths need at PG_MAX_SAMPLE_RATE
  // (13,452 = 26.9 KB at 44.1 kHz, the Cardputer's; 14,640 = 29.3 KB at 48 kHz).
  static constexpr int kPoolLen = reverb_len::poolFor(reverb_len::kMaxScale);

  void init(float halfRate);
  void reset();
  // rt60 in seconds (mid-band), dampCoef = one-pole coefficient of the in-loop low-pass.
  void setParams(float rt60, float dampCoef);
  // n <= kMaxBlock half-rate samples of mono input; ADDS wet stereo to outL/outR.
  void process(const float* in, float* outL, float* outR, int n) { run<false>(in, outL, outR, n); }
  // Mono device mode: ADDS only the mid tap set (= (L+R)/2 of process()) to
  // `out`; the side taps are never read. Same tank state as process().
  void processMono(const float* in, float* out, int n) { run<true>(in, out, nullptr, n); }
  bool idle() const { return zeroRun_ >= idleAfter_; }
  int totalDelaySamples() const { return used_; }

 private:
  template <bool kMono> void run(const float* in, float* outL, float* outR, int n);
  struct Line { int16_t* buf; int len; int pos; };
  int16_t pool_[kPoolLen];
  Line ap_[kNumAp];
  Line dl_[kNumLines];
  int tapL_[kNumLines], tapR_[kNumLines];
  float g_[kNumLines], lp_[kNumLines], err_[kNumLines];
  float damp_, hp_, hpK_, outGain_, halfRate_;
  int used_, zeroRun_, idleAfter_;
};

}  // namespace engine_detail
}  // namespace pg
