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
#include "dsp_util.h"

namespace pg {
namespace engine_detail {

class Reverb {
 public:
  static constexpr int kPoolLen = 13800;     // int16 samples -> 27.6 KB
  static constexpr int kMaxBlock = 8;
  static constexpr int kNumAp = 6;
  static constexpr int kNumLines = 8;
  static constexpr float kScale = 16384.0f;

  void init(float halfRate);
  void reset();
  // rt60 in seconds (mid-band), dampCoef = one-pole coefficient of the in-loop low-pass.
  void setParams(float rt60, float dampCoef);
  // n <= kMaxBlock half-rate samples of mono input; ADDS wet stereo to outL/outR.
  void process(const float* in, float* outL, float* outR, int n);
  bool idle() const { return zeroRun_ >= idleAfter_; }
  int totalDelaySamples() const { return used_; }

 private:
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
