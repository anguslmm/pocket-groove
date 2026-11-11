// Master bus: DC/rumble high-pass -> DJ filter -> drive -> volume -> soft limiter -> int16.
//
//   * Rumble filter: always on, 2nd-order Butterworth high-pass at 20 Hz
//     (TPT state-variable form). Removes DC (from asymmetric voices, or an
//     offset left by a drive stage) and sub-sonic rumble that would only eat
//     limiter headroom: -0.2 dB at 45 Hz (kick fundamental), -12 dB/oct below.
//
// GAIN STAGING (the reasoning the defaults are built on)
//   * Voices are specified to peak around -9..-6 dBFS (0.35..0.5) each.
//   * Track level 100 = unity, taper (v/100)^3: 127 = +6.2 dB, 64 = -11.6 dB.
//     Pan is constant-power balance normalised to unity at centre.
//   * Master volume 100 = unity (same taper), drive 0 = clean.
//   * With the default levels (kick +1 dB, bass -0.5 dB, the rest -1..-4.5 dB)
//     a busy bar has the kick at ~0.55, clap/snare ~0.4, hats ~0.25, lead ~0.3,
//     bass ~0.45 between kicks and ~0.15 under them (ducker). Uncorrelated
//     parts add in power, so typical bus peaks are 0.7..0.9 with occasional
//     coincident peaks of 1.1..1.4; RMS lands around -15..-13 dBFS, which for
//     kick+bass-heavy material is roughly -13..-11 LUFS.
//   * The limiter is a static soft clipper: bit-transparent below 0.7071
//     (-3 dBFS), a quadratic knee from there, full scale reached at an input
//     of 1.29 (+2.2 dBFS), hard ceiling after that. So the default mix is
//     only touched on the loudest coincident hits (1-3 dB of soft clipping on
//     the kick+clap downbeats: "loud but not crushed"), and no parameter
//     setting can make the int16 output wrap.
//   * Mono device: the firmware plays (L+R)/2. Centred tracks are unaffected;
//     a hard-panned track is -3 dB in mono (constant-power pan). The delay's
//     ping-pong and the reverb's mid/side output sum to mono with no loss or
//     colouring by construction (tested in test_engine_fx).
//   * Drive: u = clamp(pre*x, +-1.5); y = post*(u - c*u^3). pre runs 1..8
//     (0..+18 dB), post = 1/sqrt(pre) (about half the added level is given
//     back, so drive gets louder and denser but does not run away), c fades in
//     over the first eighth of the knob so drive 0 is exactly linear.
#pragma once
#include "dsp_util.h"

namespace pg {
namespace engine_detail {

class MasterBus {
 public:
  void init(float sampleRate, int quantum);
  void reset();
  // Raw param values, called once per quantum (cheap when nothing moves).
  void setParams(int filter, int reso, int drive, int volume);
  // In-place on the float bus (frames = quantum), then writes interleaved int16.
  // Returns nothing; peaks (post-limiter, 0..1) are accumulated into peakL/peakR.
  void process(float* l, float* r, int16_t* out, int frames, float* peakL, float* peakR) {
    run<false>(l, r, out, frames, peakL, peakR);
  }
  // Mono device mode: one channel through the whole chain; the same int16
  // goes to both output slots, and *peakR is set to *peakL.
  void processMono(float* m, int16_t* out, int frames, float* peakL, float* peakR) {
    run<true>(m, m, out, frames, peakL, peakR);
    *peakR = *peakL;
  }

  bool filterActive() const { return mode_ != kBypass || mix_ > 0.0f; }
  static constexpr float kKnee = 0.70710678f;
  uint32_t faults() const { return faults_; }
  void setRumbleBypass(bool b) { rumbleOff_ = b; }

 private:
  enum Mode : uint8_t { kBypass = 0, kLowPass, kHighPass };
  void updateFilterCoefs();
  template <bool kMono> void run(float* l, float* r, int16_t* out, int frames, float* peakL, float* peakR);
  void rumble(float* x, int c, int frames);

  float sr_, invSr_, smoothK_, mixStep_, gainK_;
  float top_, lpLog2_, hpLog2_;
  bool coefDirty_;
  uint32_t faults_;
  // DJ filter
  Mode mode_;
  float pos_, posTarget_;       // smoothed knob position (within the current mode)
  float mix_;                   // 0 = dry (true bypass), 1 = filtered
  float mixTarget_;
  float resoK_;                 // 1/Q
  float a1_, a2_, a3_;
  float ic1_[2], ic2_[2];
  // rumble high-pass
  float hpG_, hpGk_, hpD_;
  float hp1_[2], hp2_[2];
  bool rumbleOff_;              // test hook only (zero-initialised .bss: filter on)
  int lastFilter_, lastReso_;
  // drive / volume
  float pre_, preTarget_, c3_, c3Target_, post_, postTarget_;
  int lastDrive_, lastVolume_;
  float driveGain_, volGain_;
};

}  // namespace engine_detail
}  // namespace pg
