// Pocket Groove drum voices: six live-synthesised drum sounds, no samples.
//
//   KickVoice   sine body + pitch sweep + click + saturation   (track Kick)
//   SnareVoice  two tuned modes + filtered noise               (track Snare)
//   ClapVoice   band-passed noise, multi-burst envelope + tail (track Clap)
//   HatVoice    six-square metallic bank + noise, HP filtered  (tracks Closed/Open hat)
//   PercVoice   Tom / Rim / Cowbell / Clave / Shaker / Zap     (track Perc)
//
// All are one-shots (noteOff is ignored), mono (same signal on L and R), and
// follow the real-time rules in voice.h. NoteEvent::note transposes every
// voice (60 = natural). Usage: construct, init(sampleRate), then per quantum
// setParams() -> noteOn()/choke() -> render() while active().
#pragma once
#include "../voice.h"
#include "drum_dsp.h"

namespace pg {
namespace drums {

// Shared plumbing: parameter cache / change detection, retrigger de-click,
// choke fade, active flag.
class DrumVoice : public Voice {
 public:
  static constexpr int kMaxParams = 12;

  void init(float sampleRate) override;
  const ParamInfo* paramInfo(int index) const override {
    return (index >= 0 && index < numParams_) ? &table_[index] : nullptr;
  }
  void setParams(const uint8_t* soundParams) override;
  // Fractional values (a macro in motion; voice.h). Each voice divides its
  // parameters into two kinds (contMask_, listed in each voice's comment below):
  //   continuous  follow at the engine's control rate, also while a hit rings,
  //               through updateFine (cheap: no libm, polynomials instead)
  //   latched     take their new value at the next hit (applyPending at the start
  //               of noteOn, through updateCoefs with libm, once per hit)
  // so a macro sweep over a roll recomputes no expf/powf per control tick and
  // never changes an envelope or a model in the middle of a hit. Back on the
  // 7-bit path (setParams), whatever the fractions touched is recomputed exactly.
  void setParamsFine(const float* soundParams) override;
  void noteOff(uint8_t) override {}
  void choke() override;
  bool active() const override { return active_; }
  bool mono() const override { return true; }   // endBlock copies L to R

 protected:
  DrumVoice(const ParamInfo* table, int numParams);
  // Clear all DSP state (init time).
  virtual void resetState() = 0;
  // Recompute coefficients for the params whose bit is set in `changed`, from pf_.
  virtual void updateCoefs(uint32_t changed) = 0;
  // The continuous params among `changed` (a subset of contMask_), cheaply.
  virtual void updateFine(uint32_t /*changed*/) {}
  // First thing in noteOn: latched params that moved since the last hit take effect.
  void applyPending() {
    if (!pend_) return;
    const uint32_t c = pend_;
    pend_ = 0;
    touched_ |= c;
    updateCoefs(c);
  }

  // Call first in noteOn. If the voice is still sounding and `declick` is
  // set, the last output value is turned into a short fading offset so the
  // restart from zero state is continuous. `ceiling` is the largest level
  // this hit should reach: while the fading offset is present, old tail +
  // new attack are soft-limited towards it (softLimit, not a clamp).
  void beginHit(bool declick, float ceiling);
  // Call last in render: applies de-click and choke, copies L to R, updates
  // the active flag.
  void endBlock(float* outL, float* outR, int frames, bool stillSounding);

  const ParamInfo* table_;
  int   numParams_;
  float fs_ = 44100.0f;
  uint8_t raw_[kMaxParams];  // last values seen by setParams
  uint8_t p_[kMaxParams];    // same, clamped to each param's range (rounded, while fractional)
  float pf_[kMaxParams];     // what the mappings read: float(p_), or the fractional values
  uint32_t contMask_ = 0;    // set by each voice: its continuous params (see setParamsFine)
  uint32_t pend_ = 0;        // latched params changed by fractions, applied at the next hit
  uint32_t touched_ = 0;     // params whose coefficients came from fractions
  bool  fine_ = false;       // pf_ holds fractional values
  bool  active_ = false;

 private:
  bool  choking_ = false;
  float chokeGain_ = 1.0f, chokeStep_ = 0.01f;
  float resid_ = 0.0f, residRamp_ = 0.0f, residStep_ = 0.01f, ceiling_ = 1.0f;
  float lastOut_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Macro motion: TUNE, PUNCH, DRIVE, HARM, TONE continuous (also mid-hit);
// DECAY, SWEEP, HOLD, CLICK, CLICK TONE latched at the next hit.
class KickVoice final : public DrumVoice {
 public:
  enum Param : uint8_t {
    kTune, kDecay, kPunch, kSweep, kClick, kDrive, kTone, kHold, kHarm, kClickTone, kNumParams
  };
  KickVoice();
  float debugTuneHz() const { return tuneHz_; }   // body pitch before the note and the sweep (tests)
  void noteOn(const NoteEvent& e) override;
  void render(float* outL, float* outR, int frames) override;

 private:
  void resetState() override;
  void updateCoefs(uint32_t changed) override;
  void updateFine(uint32_t changed) override;
  template <bool Fast> void setPitchT();   // Fast: no libm (macro motion)
  void setPitch() { setPitchT<false>(); }

  // coefficients
  float tuneHz_ = 50.0f, noteRatio_ = 1.0f, depthBase_ = 4.0f, accentDepth_ = 1.0f;
  float eps0_ = 0.007f, depth_ = 4.0f, pitchCoef_ = 0.99f, ampCoef_ = 0.999f;
  float drive_ = 1.0f, post_ = 1.0f, driveComp_ = 1.0f, h2_ = 0.0f, lpG_ = 0.5f;
  float clickAmt_ = 0.0f, clickCoef_ = 0.9f, pulseShare_ = 1.0f, noiseShare_ = 0.0f;
  int   holdSamples_ = 0;
  // per-hit
  float level_ = 0.0f, clickPulse_ = 0.0f, clickNoise_ = 0.0f;
  // state
  float x_ = 0.0f, y_ = 1.0f, pe_ = 0.0f, amp_ = 0.0f, ce_ = 0.0f, lp1_ = 0.0f, lp2_ = 0.0f;
  int   hold_ = 0;
  Noise noise_;
};

// ---------------------------------------------------------------------------
// Macro motion: TUNE, DETUNE, TONE, BEND, SNAPPY, RING, DRIVE continuous (pitch and
// noise filter also mid-hit); DECAY, BODY DECAY latched at the next hit.
class SnareVoice final : public DrumVoice {
 public:
  enum Param : uint8_t {
    kTune, kDecay, kSnap, kTone, kBody, kBend, kRing, kDetune, kDrive, kNumParams
  };
  SnareVoice();
  void noteOn(const NoteEvent& e) override;
  void render(float* outL, float* outR, int frames) override;

 private:
  void resetState() override;
  void updateCoefs(uint32_t changed) override;
  void updateFine(uint32_t changed) override;
  template <bool Fast> void setPitchT();   // Fast: no libm (macro motion)
  void setPitch() { setPitchT<false>(); }
  template <bool Fast> void setFilterT();
  void setFilter() { setFilterT<false>(); }

  float tuneHz_ = 185.0f, ratio_ = 1.75f, noteRatio_ = 1.0f, toneHz_ = 3000.0f, bright_ = 1.0f;
  float eps1_ = 0.02f, eps2_ = 0.04f, bend_ = 0.4f, pitchCoef_ = 0.99f;
  float c1_ = 0.999f, c2_ = 0.999f, cn_ = 0.999f, cf_ = 0.99f;
  float bodyAmt_ = 0.5f, ringAmt_ = 0.3f, noiseAmt_ = 0.4f, snapAmt_ = 0.3f;
  float drive_ = 1.0f, post_ = 1.0f, noiseNorm_ = 1.0f;
  float level_ = 0.0f;
  float x1_ = 0.0f, y1_ = 1.0f, x2_ = 0.0f, y2_ = 1.0f, pe_ = 0.0f;
  float a1_ = 0.0f, a2_ = 0.0f, an_ = 0.0f, af_ = 0.0f;
  Svf   svf_;
  Noise noise_;
};

// ---------------------------------------------------------------------------
// Macro motion: TONE, RESONANCE, AIR, TAIL, DRIVE continuous (the band-pass also
// mid-hit); DECAY, SPREAD, BURSTS latched at the next hit.
class ClapVoice final : public DrumVoice {
 public:
  enum Param : uint8_t {
    kTone, kDecay, kSpread, kTail, kBursts, kReso, kAir, kDrive, kNumParams
  };
  ClapVoice();
  void noteOn(const NoteEvent& e) override;
  void render(float* outL, float* outR, int frames) override;

 private:
  void resetState() override;
  void updateCoefs(uint32_t changed) override;
  void updateFine(uint32_t changed) override;
  template <bool Fast> void setFilterT();
  void setFilter() { setFilterT<false>(); }

  float toneHz_ = 1150.0f, q_ = 1.6f, noteRatio_ = 1.0f, bright_ = 1.0f;
  float bpGain_ = 1.0f, airGain_ = 0.2f, airAmt_ = 0.3f;
  float burstCoef_ = 0.99f, tailCoef_ = 0.999f, tailAmt_ = 0.6f, spacing_ = 480.0f;
  float drive_ = 1.0f, post_ = 1.0f;
  int   numBursts_ = 3;
  float level_ = 0.0f, tailBoost_ = 1.0f;
  float be_ = 0.0f, te_ = 0.0f, nextBurst_ = 0.0f;
  int   t_ = 0, burstIdx_ = 0;
  Svf   svf_;
  Noise noise_;
};

// ---------------------------------------------------------------------------
// One class for both hat tracks; `open` only selects the default values and
// the range of the decay knob.
// Macro motion: TUNE, SPREAD, TONE, COLOUR, METAL, SNAP continuous (bank pitch and
// filter also mid-hit, so an open hat sweeps); DECAY latched at the next hit.
class HatVoice final : public DrumVoice {
 public:
  enum Param : uint8_t { kTune, kDecay, kTone, kMetal, kColour, kSnap, kSpread, kNumParams };
  explicit HatVoice(bool open = false);
  bool isOpen() const { return open_; }
  void noteOn(const NoteEvent& e) override;
  void render(float* outL, float* outR, int frames) override;

 private:
  void resetState() override;
  void updateCoefs(uint32_t changed) override;
  void updateFine(uint32_t changed) override;
  template <bool Fast> void setPitchT();   // Fast: no libm (macro motion)
  void setPitch() { setPitchT<false>(); }
  template <bool Fast> void setFilterT();
  void setFilter() { setFilterT<false>(); }

  bool  open_;
  float noteRatio_ = 1.0f, bright_ = 1.0f;
  float metalAmt_ = 0.5f, noiseAmt_ = 0.5f, metalNorm_ = 1.0f, bpMix_ = 0.5f, gainComp_ = 1.0f, tiltK_ = 0.5f;
  float cs_ = 0.999f, cf_ = 0.99f, snapAmt_ = 1.0f;
  uint32_t inc_[6] = {0, 0, 0, 0, 0, 0};
  uint32_t ph_[6];
  int32_t  prev_ = 0;
  float es_ = 0.0f, ef_ = 0.0f, tilt_ = 0.0f, limit_ = 1.0f, invLimit_ = 1.0f;
  Svf   svf_;
  Noise noise_;
};

// ---------------------------------------------------------------------------
// Macro motion: every parameter latched at the next hit (a change recomputes the
// whole model with some 15 libm calls: fine per hit, too dear per control tick).
class PercVoice final : public DrumVoice {
 public:
  enum Param : uint8_t { kModel, kTune, kDecay, kTone, kBend, kHarm, kNoise, kDrive, kNumParams };
  enum Model : uint8_t { kTom, kRim, kCowbell, kClave, kShaker, kZap, kNumModels };
  PercVoice();
  void noteOn(const NoteEvent& e) override;
  void render(float* outL, float* outR, int frames) override;

 private:
  void resetState() override;
  void updateCoefs(uint32_t changed) override;
  void computeModel();
  bool renderTonal(float* out, int frames);
  bool renderCowbell(float* out, int frames);
  bool renderShaker(float* out, int frames);

  uint8_t model_ = kRim;       // latched at note-on
  float noteRatio_ = 1.0f, level_ = 0.0f, accentMul_ = 1.0f;
  // coefficients (meaning depends on model)
  float eps1_ = 0.05f, eps2_ = 0.1f, bend_ = 0.0f, pitchCoef_ = 0.99f;
  float c1_ = 0.99f, c2_ = 0.99f, cn_ = 0.99f;
  float amt1_ = 1.0f, amt2_ = 0.0f, amtN_ = 0.0f;
  float noiseG_ = 0.5f, noiseLp_ = 0.0f, noiseHp_ = 1.0f;
  float drive_ = 1.0f, post_ = 1.0f, modelLevel_ = 0.25f;
  float grain_ = 0.0f, dry_ = 0.0f, srcGain_ = 1.0f, svfNorm_ = 1.0f;
  uint32_t inc1_ = 0, inc2_ = 0;
  // state
  float x1_ = 0.0f, y1_ = 1.0f, x2_ = 0.0f, y2_ = 1.0f, pe_ = 0.0f;
  float a1_ = 0.0f, a2_ = 0.0f, an_ = 0.0f, nl_ = 0.0f;
  uint32_t ph1_ = 0, ph2_ = 0;
  Svf   svf_;
  Noise noise_;
};

}  // namespace drums
}  // namespace pg
