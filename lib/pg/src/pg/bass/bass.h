// Pocket Groove — monophonic bass synth (acid / bass-house).
//
// Signal path (everything at the base sample rate, one voice):
//
//   OSC A  naive saw <-> square <-> narrow pulse                --+
//   OSC B  2-operator FM sine pair (carrier at note pitch)        +--> sine wavefolder (naive domain)
//                                                                       |
//          16-tap table BLEP on OSC A's edges, each scaled by the *folded* jump,
//          so folding never sees a band-limited edge (fold on a saw stays ~-80 dBc clean)
//                                                                       |
//          4-pole ZDF diode ladder (TB-303 topology), saturating feedback, input drive
//                                                                       |
//          asymmetric soft-clip drive (1st-order antiderivative anti-aliasing)
//                                                                       |
//          1-pole high-pass (DC blocker; rises with SUB so the sine owns the low end)
//                                                                       |
//   SUB    clean sine (unison or -1 oct) -------------------------------+--> VCA --> out
//
// Control rate (once per 16-frame quantum): pitch glide, filter/accent/amp
// envelopes, tempo-synced LFO, cutoff smoothing, filter coefficients.
// No libm outside init(); no doubles; no allocation.
#pragma once
#include "../voice.h"

namespace pg {
namespace bass {

// Sound-param indices (Project::tracks[kTrackBass].params[0..23]).
enum Param : uint8_t {
  // ---- page 1: play it live
  kCutoff = 0,   // filter resonance frequency, 40 Hz .. 8.6 kHz (exponential; hard ceiling fs/5)
  kReso,         // 0 = gentle 1-pole-ish slope .. ~123 = edge of self-oscillation .. 127 = sings
  kEnvMod,       // filter envelope depth, 0 .. +6 octaves
  kDecay,        // filter envelope decay, 20 ms .. 2.5 s
  kAccent,       // accent amount: VCA boost + extra, faster, resonance-smoothed filter kick
  kDrive,        // filter input level + post-filter asymmetric overdrive (0 .. +30 dB)
  kLfoDepth,     // LFO -> cutoff, 0 .. +6 octaves (unipolar, opens the filter)
  kLfoRate,      // enum of note divisions, follows setTempo()
  // ---- page 2: oscillators / timbre
  kWave,         // 0 saw .. 64 square .. 127 10% pulse
  kSub,          // clean sine level, mixed after the distortion
  kFmMix,        // crossfade OSC A -> FM pair
  kFmDepth,      // FM index, 0 = pure sine
  kFmRatio,      // enum: modulator / carrier frequency ratio
  kFold,         // sine wavefolder amount (pre-filter)
  kTimbreEnv,    // how much the filter envelope scales FM index + fold (0 = static)
  kLfoShape,     // enum
  // ---- page 3: envelopes / setup
  kSlide,        // glide time for slid notes, 5 .. 640 ms (time to cover 95 %)
  kAmpDecay,     // VCA decay while the gate is held, 50 ms .. 4.5 s; 127 = sustain
  kRelease,      // VCA release after noteOff, 2 .. 500 ms time constant
  kAttack,       // VCA attack, 0.7 .. 180 ms
  kKeyTrack,     // cutoff follows pitch 0..100 %, pivot = C2 (MIDI 36)
  kLfoTimbre,    // LFO -> FM index + fold depth
  kSubOct,       // enum: unison / -1 octave
  kTune,         // OSC A + FM transpose, -24 .. +24 semitones (sub stays on the note)
  kNumParams
};
static_assert(kNumParams <= kSoundParams, "too many bass params");

class BassVoice final : public Voice {
 public:
  BassVoice();

  void init(float sampleRate) override;
  const ParamInfo* paramInfo(int index) const override;
  void setParams(const uint8_t* soundParams) override;
  void setTempo(float bpm) override;
  void noteOn(const NoteEvent& e) override;
  void noteOff(uint8_t note) override;
  void choke() override;
  void render(float* outL, float* outR, int frames) override;
  bool active() const override { return stage_ != kIdle; }

  // Read-only introspection for tests and UI meters (values as of the last rendered quantum).
  float debugCutoffHz() const { return dbgCutoffHz_; }
  float debugPitch() const { return noteCur_; }        // MIDI note, fractional while gliding
  float debugFilterEnv() const { return fenv_; }
  float debugAmpEnv() const { return aenv_; }
  float debugLfo() const { return lfoVal_; }           // 0..1

 private:
  enum Stage : uint8_t { kIdle = 0, kAttackStage, kDecayStage, kReleaseStage, kChokeStage };

  void mapParams();
  void resetAudioState();

  // ---- configuration
  float sr_ = kDefaultSampleRate;
  float invSr_ = 1.0f / kDefaultSampleRate;
  float ctlDt_ = float(kQuantum) / kDefaultSampleRate;  // seconds per quantum
  float bpm_ = 120.0f;
  uint8_t params_[kSoundParams];
  bool paramsValid_ = false;

  // ---- mapped parameter targets (updated in setParams when something changed)
  float pCutOct_ = 0, pK_ = 1, pMakeup_ = 2, pEnvOct_ = 0, pFDecCoef_ = 0.9f, pFDecCoefAcc_ = 0.9f;
  float pAccent_ = 0, pAccOct_ = 0, pAccCoef_ = 0.1f;
  float pPre_ = 0.5f, pDriveGain_ = 1, pBias_ = 0, pPostGain_ = 1;
  float pLfoOct_ = 0, pLfoBeats_ = 0.5f;
  float pPulseMix_ = 0, pPw_ = 0.5f, pOscNorm_ = 1;
  float pSub_ = 0, pSubHp_ = 0, pMix_ = 0, pFmIndex_ = 0, pFmRatio_ = 2;
  float pFoldMix_ = 0, pFoldCycles_ = 0.25f, pTimbreEnv_ = 0, pLfoTimbre_ = 0;
  float pGlideCoef_ = 1, pADecCoef_ = 1, pRelCoef_ = 0.5f, pAtkInc_ = 0.5f;
  float pKeyTrack_ = 0, pTuneRatio_ = 1, pSubRatio_ = 1;
  uint8_t pLfoShape_ = 0;

  // ---- note / control state
  Stage stage_ = kIdle;
  bool gate_ = false, glide_ = false, accentOn_ = false, snap_ = true;
  uint8_t curNote_ = 36;
  float noteCur_ = 36, noteTarget_ = 36;
  float levelTarget_ = 0, levelSm_ = 0;       // velocity * accent gain
  float aenv_ = 0, ampCur_ = 0, chokeDec_ = 0;
  float fenv_ = 0, fdecCoefCur_ = 0.9f, accLp_ = 0;
  float lfoPhase_ = 0, lfoInc_ = 0, lfoVal_ = 0, lfoHold_ = 0;
  uint32_t rng_ = 0x1234567u;
  float cutLag_ = 0, dbgCutoffHz_ = 0;
  // smoothed (per quantum) copies of the level-type params
  float sK_ = 1, sMakeup_ = 2, sPre_ = 0.5f, sDriveGain_ = 1, sBias_ = 0, sPostGain_ = 1;
  float sPulseMix_ = 0, sPw_ = 0.5f, sGainA_ = 1, sGainFm_ = 0, sSub_ = 0, sSubHp_ = 0;
  float sFoldMix_ = 0, sFoldCycles_ = 0.25f, sFmIndex_ = 0;

  // ---- audio-rate state
  uint32_t phA_ = 0, phC_ = 0, phM_ = 0, phS_ = 0;
  float ring_[16];            // BLEP mix-down ring (8 samples of look-ahead)
  uint32_t ringPos_ = 0;
  float s1_ = 0, s2_ = 0, s3_ = 0, s4_ = 0;   // ladder integrator states
  float drvZ_ = 0, drvF_ = 0;                 // ADAA history
  float hpLp_ = 0;                            // high-pass state
};

}  // namespace bass
}  // namespace pg
