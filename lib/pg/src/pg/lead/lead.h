// Pocket Groove lead track: 4-voice stereo poly synth for house chord stabs,
// plucks, organ / FM keys, techno stabs, short pads and mono lead lines.
//
// Signal path, per voice:
//
//   osc1 (SAW | PULSE | TRI | 5-saw SUPER | 2-op FM x3 | drawbar ORGAN) --+
//   osc2 (SAW | PULSE | TRI | SINE, interval + detune) -------------------+--> stereo pre-mix
//       --> 2x ZDF state-variable filter (L, R; LP/BP/HP, shared coefficients)
//       --> amp ADSR (per-sample ramp) --> bus --> drive / soft limiter (bounded at +-1.0)
//       --> DC blocker (8 Hz, faded in with DRIVE; exact pass-through at DRIVE 0)
//
// * Oscillators are mip-mapped band-limited wavetables (lead_tables.h).
// * Stereo: osc1 and osc2 of a voice sit either side of the voice's pan
//   position, chord notes are spread across the field. Panning is linear
//   (gL + gR = 2 for every source), so L+R does not depend on WIDTH at all:
//   the mono sum the device plays is exactly the WIDTH 0 signal, and L == R
//   bit for bit at WIDTH 0. Nothing is ever decorrelated by phase tricks.
// * Mono: setMono(true) (or WIDTH 0) renders that same mono sum with one
//   filter per voice instead of two. The hardware codec is mono, so this is
//   the path the firmware should use; stereo is for desktop renders.
// * Control rate = one quantum (16 frames): envelopes, glide, LFO, filter
//   coefficients, oscillator increments. Nothing transcendental per sample.
//   Detuned copies of low notes (below MIDI 64, none below 48) are not split
//   L/R, so wide patches never put the lows in anti-phase on headphones.
// * A stolen voice fades out over ~2.5 ms and then starts the new note, so
//   stealing never clicks (the stolen note starts ~2.5 ms late).
// * Levels: a full-velocity 4-note chord at defaults peaks about -9 dBFS.
//   Each note's gain is fixed at note start by the size of its chord (single
//   notes +3.5 dB, 2: +1.9, 3: +0.8, 4: 0) so lines and stabs sit close in
//   level without any voice-count pumping.
// * Param smoothing (5 ms) applies while voices sound; a note that starts
//   from silence snaps to the current params (p-locks start clean).
//
// Envelope times quoted by the *Seconds() helpers: attack = linear ramp time;
// decay / release = time to fall 60 dB (exponential, T60 = 6.9 time constants).
// The filter envelope is attack -> exponential decay to zero (no sustain: the
// CUT knob already sets the sustained brightness, so a sustain level would
// only duplicate it). It also drives FM index and organ percussion.
#pragma once
#include "../voice.h"

namespace pg {
namespace lead {

enum Param : uint8_t {
  // page 1: live tweaks
  kPCutoff = 0, kPReso, kPEnvAmt, kPFltDecay, kPWave, kPShape, kPDetune, kPDecay,
  // page 2: envelopes + filter (WIDTH lives here: the device is mono)
  kPAttack, kPWidth, kPSustain, kPRelease, kPFltAttack, kPFltType, kPKeyTrack, kPDrive,
  // page 3: osc 2 + modulation
  kPOsc2Wave, kPInterval, kPOscMix, kPLfoRate, kPVibrato, kPLfoShape, kPLfoCutoff, kPGlide,
  kNumParams
};
static_assert(kNumParams == kSoundParams, "lead uses all 24 sound params");

enum Wave : uint8_t { kWaveSaw = 0, kWavePulse, kWaveTri, kWaveSuper, kWaveFm1, kWaveFm2, kWaveBell, kWaveOrgan, kNumWaves };
enum Osc2Wave : uint8_t { kOsc2Saw = 0, kOsc2Pulse, kOsc2Tri, kOsc2Sine, kNumOsc2Waves };
enum FilterType : uint8_t { kFilterLP = 0, kFilterBP, kFilterHP, kNumFilterTypes };

struct Tables;   // lead_tables.h

constexpr int kIntervalCentre = 24;  // kPInterval value for 0 semitones (range +-24)
constexpr int kMaxChordNotes  = 4;

class LeadVoice final : public Voice {
 public:
  LeadVoice();

  void init(float sampleRate) override;
  const ParamInfo* paramInfo(int index) const override;
  void setParams(const uint8_t* soundParams) override;
  void noteOn(const NoteEvent& e) override;
  void noteOff(uint8_t note) override;
  void choke() override;
  void render(float* outL, float* outR, int frames) override;
  bool active() const override;

  // ---- introspection for tests / UI (not needed by the engine) -------------
  // Semitone offsets from the root for a ChordType; returns the note count (1..4).
  static int   chordIntervals(uint8_t chordType, int8_t* semis);
  static float cutoffHz(uint8_t v);          // CUT at key 60, before env / LFO
  static float attackSeconds(uint8_t v);     // linear ramp time
  static float decaySeconds(uint8_t v);      // T60
  static float releaseSeconds(uint8_t v);    // T60
  static float fltDecaySeconds(uint8_t v);   // T60
  static float fltAttackSeconds(uint8_t v);
  static float glideSeconds(uint8_t v);      // time constant
  static float lfoHz(uint8_t v);
  static float detuneCents(uint8_t v);       // osc1 <-> osc2 distance
  int soundingVoices() const;                // voices not idle

  // Mono output: both channels get the mono sum and each voice runs ONE filter
  // instead of two (saves ~25% CPU). The result equals (L+R)/2 of the stereo
  // render at any WIDTH, so nothing about the sound changes for a mono codec.
  // The Cardputer's ES8311 is mono: the firmware should call setMono(true).
  // WIDTH = 0 selects the same path automatically. Call from the audio thread
  // or before audio starts.
  void setMono(bool mono);
  // Voice contract: setMonoOutput(true) is setMono(true); mono() reports
  // whether render() currently writes L == R (only guaranteed in forced mono).
  void setMonoOutput(bool mono) override { setMono(mono); }
  bool mono() const override { return forceMono_; }

 private:
  enum Stage : uint8_t { kOff = 0, kAtk, kDec, kRel, kKill };

  struct NoteStart {
    uint8_t note, root, vel, chordIdx, chordSize, panSlot;
    bool accent, gateOff;
    uint32_t group;
  };

  struct V {
    // allocation
    uint8_t stage;
    bool gate, pending, fresh;
    uint8_t note, root, chordIdx, chordSize, panSlot;
    uint32_t group, age;
    NoteStart pend;
    // pitch (MIDI semitones)
    float pitch, target;
    // envelopes
    float amp, killStep, gain;
    int killLeft;
    float fenv;
    uint8_t fstage;
    float velAmp, envMul, envAdd, modVel;
    // stereo
    float pan, flip;
    // oscillators
    uint32_t ph1[5];   // osc1: [0] main, [1] second lookup | super: 5 saws | fm: carrier, modulator
    uint32_t ph2[2];   // osc2
    uint8_t two1, two2;   // previous quantum used the 2-lookup renderer
    float fmIdx, orgPos, orgPerc;   // previous-quantum values for per-sample ramps
    // filter state
    float ic1L, ic2L, ic1R, ic2R;
  };

  struct OscShape {
    uint8_t kind;        // OscKind
    uint8_t setA, setB;  // table sets
    float gA, gB;
    uint32_t offs;       // phase offset of the second lookup
  };

  void applyParam(int index);
  void globalQuantum();
  void shapeFor(int wave, OscShape& s) const;
  template <bool ST> void renderVoiceT(V& v, float* outL, float* outR);
  void startVoice(V& v, const NoteStart& n);
  void queueNote(int vi, const NoteStart& n);
  int  allocVoice(uint8_t note, uint32_t claimedMask) const;
  void releaseVoice(V& v);
  void setPan(V& v, uint8_t chordIdx, uint8_t chordSize, uint8_t slot);

  bool ready_ = false;
  const Tables* tab_ = nullptr;   // constant wavetable set for fs_ (flash), set by init()
  float fs_ = kDefaultSampleRate;
  float smooth_ = 0.07f, gainK_ = 0.0f;
  float quantumRate_ = kDefaultSampleRate / float(kQuantum);
  float incK_ = 0.0f, cutK_ = 0.0f, pitchMax_ = 120.0f, cutPitchMax_ = 120.0f;
  int   killQuanta_ = 7;

  uint8_t cur_[kSoundParams];
  bool paramsSnap_ = true;

  // ---- derived from params (targets) ----
  float cutT_ = 60.0f, kT_ = 1.0f, resCompT_ = 1.0f, envSemis_ = 0.0f;
  float fAtkStep_ = 1.0f, fDecCoef_ = 0.9f;
  uint8_t wave_ = 0, osc2Wave_ = 0, fltType_ = 0;
  float shapeT_ = 0.0f, detSemis_ = 0.0f, widthT_ = 0.0f;
  float superRatio_[5];
  float superDetMax_ = 0.0f;
  float atkStep_ = 1.0f, decCoef_ = 0.9f, sus_ = 0.5f, relCoef_ = 0.9f;
  float keyTrack_ = 0.5f, driveT_ = 0.0f;
  float interval_ = 0.0f, mixAT_ = 0.7f, mixBT_ = 0.7f;
  float lfoInc_ = 0.0f, vibDepth_ = 0.0f, lfoShapeDepth_ = 0.0f, lfoCutDepth_ = 0.0f;
  float glideCoef_ = 1.0f;

  // ---- smoothed (one-pole per quantum) ----
  float cutS_ = 60.0f, kS_ = 1.0f, resCompS_ = 1.0f, shapeS_ = 0.0f, widthS_ = 0.0f;
  float driveS_ = 0.0f, mixAS_ = 0.7f, mixBS_ = 0.7f;

  // ---- per-quantum globals ----
  float lfoPhase_ = 0.0f, lfo_ = 0.0f;
  float vibSemis_ = 0.0f, lfoCutSemis_ = 0.0f;
  OscShape shape1_, shape2_;
  float oscSpread_ = 0.0f, panScale_ = 0.0f;
  bool forceMono_ = false, monoNow_ = false;
  float superGC_ = 1.0f, superGS_ = 0.0f, superSpread_ = 0.0f;
  float fmIdxBase_ = 0.0f, orgPos_ = 0.0f;
  float limPre_ = 1.0f, limPost_ = 1.0f, limT_ = 0.6f, limQ_ = 0.0f, limMax_ = 1.4f;
  // DC blocker after the drive stage: y = x - dcAmt * LP8Hz(x). dcAmt is 0 at
  // DRIVE 0 (exact pass-through: the clean path has no DC) and 1 from DRIVE ~10%.
  float dcC_ = 0.001f, dcAmt_ = 0.0f, dcAmtPrev_ = 0.0f, dcL_ = 0.0f, dcR_ = 0.0f;

  V v_[kLeadVoices];
  uint32_t groupCounter_ = 0, ageCounter_ = 0, lastGroup_ = 0;
};

}  // namespace lead
}  // namespace pg
