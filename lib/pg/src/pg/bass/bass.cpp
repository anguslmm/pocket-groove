// Pocket Groove — bass voice implementation. See bass.h for the signal path.
#include "bass.h"

#include <cmath>     // init()/param-change only (sinf, sqrtf); never per sample
#include <cstring>

#include "bass_math.h"

namespace pg {
namespace bass {

// ============================================================================
// Tables
// ============================================================================
float gSine[kSineSize + 1];
float gBlep[(kBlepPhases + 2) * kBlepTaps];
static bool gTablesBuilt = false;

namespace {

// Zeroth-order modified Bessel function (Kaiser window), float series.
float besselI0(float x) {
  float sum = 1.0f, term = 1.0f, h = 0.5f * x;
  for (int k = 1; k < 32; ++k) {
    float t = h / float(k);
    term *= t * t;
    sum += term;
    if (term < 1e-9f * sum) break;
  }
  return sum;
}

// Band-limiting kernel for the BLEP: Kaiser-windowed sinc, 16 samples long.
// kBlepCut is the -6 dB point in cycles/sample. It is deliberately below
// Nyquist: with only 16 taps the transition band is wide, and for a bass
// instrument trading a little "air" above 12 kHz for aliases that are >60 dB
// down is the right deal.
constexpr float kBlepCut = 0.40f;
constexpr float kBlepBeta = 8.0f;
constexpr int kBlepSub = 8;   // integration sub-steps per table phase

float blepKernel(float t) {   // t in samples, |t| <= 8
  float half = 0.5f * float(kBlepTaps);
  float u = t / half;
  float w2 = 1.0f - u * u;
  if (w2 <= 0.0f) return 0.0f;
  float win = besselI0(kBlepBeta * sqrtf(w2));
  float a = kTwoPi * kBlepCut * t;
  float sinc = (a > -1e-4f && a < 1e-4f) ? 1.0f : sinf(a) / a;
  return win * sinc;
}

}  // namespace

void buildTables() {
  if (gTablesBuilt) return;
  for (int i = 0; i <= kSineSize; ++i) gSine[i] = sinf(kTwoPi * float(i) / float(kSineSize));
  gSine[kSineSize] = 0.0f;

  // Integrate the kernel on a fine grid (trapezoid), twice: once for the
  // normalisation, once to fill the table. No big temporaries.
  const int fine = kBlepPhases * kBlepSub;              // steps per sample
  const int total = kBlepTaps * fine;
  const float dt = 1.0f / float(fine);
  const float t0 = -0.5f * float(kBlepTaps);
  float norm = 0.0f;
  {
    float prev = blepKernel(t0);
    for (int n = 1; n <= total; ++n) {
      float cur = blepKernel(t0 + float(n) * dt);
      norm += 0.5f * (prev + cur);
      prev = cur;
    }
  }
  float acc = 0.0f, prev = blepKernel(t0);
  for (int n = 0; n <= total; ++n) {
    if (n > 0) {
      float cur = blepKernel(t0 + float(n) * dt);
      acc += 0.5f * (prev + cur);
      prev = cur;
    }
    if (n % kBlepSub == 0) {
      int m = n / kBlepSub;                 // = j * kBlepPhases + p
      float blep = acc / norm;              // 0 .. 1
      // Entry (p, j) holds residual(t = j - 8 + p/phases). A fine index maps
      // to (p, j) and, at p == 0, also to (phases, j - 1): the latter is the
      // left limit, which matters at the step itself (t = 0).
      int j = m / kBlepPhases, p = m % kBlepPhases;
      if (j < kBlepTaps) gBlep[p * kBlepTaps + j] = blep - (j >= kBlepTaps / 2 ? 1.0f : 0.0f);
      if (p == 0 && j > 0) gBlep[kBlepPhases * kBlepTaps + (j - 1)] = blep - (j - 1 >= kBlepTaps / 2 ? 1.0f : 0.0f);
    }
  }
  // Guard row so a rounding t == 1.0 can never read past the table.
  for (int j = 0; j < kBlepTaps; ++j) gBlep[(kBlepPhases + 1) * kBlepTaps + j] = gBlep[kBlepPhases * kBlepTaps + j];
  gTablesBuilt = true;
}

// ============================================================================
// Parameter metadata
// ============================================================================
namespace {

const char* const kRateNames[] = {"2BAR", "1BAR", "1/2", "1/4.", "1/4", "1/8.", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
const float kRateBeats[] = {8.0f, 4.0f, 2.0f, 1.5f, 1.0f, 0.75f, 0.66666667f, 0.5f, 0.33333333f, 0.25f, 0.16666667f, 0.125f};
constexpr int kNumRates = 12;

const char* const kRatioNames[] = {"0.5", "1", "1.5", "2", "3", "3.5", "4", "5", "7"};
const float kRatios[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 3.5f, 4.0f, 5.0f, 7.0f};
constexpr int kNumRatios = 9;

enum LfoShape : uint8_t { kLfoTri = 0, kLfoSine, kLfoDown, kLfoUp, kLfoSquare, kLfoSH, kNumLfoShapes };
const char* const kShapeNames[] = {"TRI", "SINE", "DOWN", "UP", "SQR", "S&H"};
const char* const kSubOctNames[] = {"UNI", "-1OCT"};

const ParamInfo kInfo[kNumParams] = {
    // name   longName        min max  def  fmt                     enum
    {"CUT", "Cutoff", 0, 127, 46, ParamFormat::Number, nullptr},
    {"RES", "Resonance", 0, 127, 78, ParamFormat::Number, nullptr},
    {"ENV", "Env Mod", 0, 127, 70, ParamFormat::Number, nullptr},
    {"DCY", "Decay", 0, 127, 44, ParamFormat::Number, nullptr},
    {"ACC", "Accent", 0, 127, 90, ParamFormat::Number, nullptr},
    {"DRV", "Drive", 0, 127, 30, ParamFormat::Number, nullptr},
    {"LFO", "LFO Depth", 0, 127, 0, ParamFormat::Number, nullptr},
    {"RATE", "LFO Rate", 0, kNumRates - 1, 7, ParamFormat::Enum, kRateNames},

    {"WAVE", "Waveform", 0, 127, 0, ParamFormat::Number, nullptr},
    {"SUB", "Sub Level", 0, 127, 56, ParamFormat::Number, nullptr},
    {"FMIX", "FM Osc Mix", 0, 127, 0, ParamFormat::Number, nullptr},
    {"FM", "FM Depth", 0, 127, 64, ParamFormat::Number, nullptr},
    {"RATI", "FM Ratio", 0, kNumRatios - 1, 3, ParamFormat::Enum, kRatioNames},
    {"FOLD", "Fold", 0, 127, 0, ParamFormat::Number, nullptr},
    {"TENV", "Timbre Env", 0, 127, 90, ParamFormat::Number, nullptr},
    {"LSHP", "LFO Shape", 0, kNumLfoShapes - 1, kLfoTri, ParamFormat::Enum, kShapeNames},

    {"SLID", "Slide Time", 0, 127, 65, ParamFormat::Number, nullptr},
    {"ADCY", "Amp Decay", 0, 127, 96, ParamFormat::Number, nullptr},
    {"REL", "Release", 0, 127, 32, ParamFormat::Number, nullptr},
    {"ATK", "Attack", 0, 127, 18, ParamFormat::Number, nullptr},
    {"KTRK", "Key Track", 0, 127, 40, ParamFormat::Number, nullptr},
    {"LTMB", "LFO>Timbre", 0, 127, 0, ParamFormat::Number, nullptr},
    {"SUBO", "Sub Octave", 0, 1, 0, ParamFormat::Enum, kSubOctNames},
    {"TUNE", "Osc Tune", 0, 48, 24, ParamFormat::Semitones, nullptr},
};

// ---- voicing constants ------------------------------------------------------
constexpr float kCutBaseHz = 40.0f;      // CUT = 0
constexpr float kCutRangeOct = 7.75f;    // CUT = 127 -> 8.6 kHz
constexpr float kEnvRangeOct = 6.0f;
constexpr float kLfoRangeOct = 6.0f;
constexpr float kAccRangeOct = 3.0f;
constexpr float kAccGain = 0.50f;        // VCA: up to +3.5 dB at ACC = 127
constexpr float kResMin = 3.0f;          // loop gain at RES = 0 (0 would be a very dull 6 dB/oct slope)
constexpr float kResOnset = 0.97f;       // RES position where the loop gain reaches 17 (self-oscillation)
constexpr float kOutGain = 0.33f;        // sets the default patch to about -9 dBFS peak
constexpr float kSmooth = 0.2f;          // per-quantum one-pole for level-type params (~1.6 ms)
constexpr float kCutLag = 0.26f;         // per-quantum one-pole on the cutoff CV (~1.2 ms)
constexpr float kTwo32 = 4294967296.0f;
constexpr float kTwo24 = 16777216.0f;
constexpr float kInt31ToFloat = 1.0f / 2147483648.0f;

inline float unit(uint8_t v) { return float(v) * (1.0f / 127.0f); }
inline void smooth(float& s, float target) { s += kSmooth * (target - s); }

}  // namespace

// ============================================================================
// Lifecycle / params
// ============================================================================
BassVoice::BassVoice() {
  for (int i = 0; i < kSoundParams; ++i) params_[i] = i < kNumParams ? kInfo[i].def : uint8_t(0);
  std::memset(ring_, 0, sizeof ring_);
}

void BassVoice::init(float sampleRate) {
  buildTables();
  sr_ = sampleRate > 8000.0f ? sampleRate : 8000.0f;
  invSr_ = 1.0f / sr_;
  ctlDt_ = float(kQuantum) * invSr_;
  stage_ = kIdle;
  gate_ = false;
  accLp_ = 0.0f;
  resetAudioState();
  mapParams();
  paramsValid_ = true;
  snap_ = true;
}

const ParamInfo* BassVoice::paramInfo(int index) const {
  return (index >= 0 && index < kNumParams) ? &kInfo[index] : nullptr;
}

void BassVoice::setParams(const uint8_t* p) {
  if (paramsValid_ && std::memcmp(p, params_, kNumParams) == 0) return;
  for (int i = 0; i < kNumParams; ++i) {
    uint8_t v = p[i];
    if (v < kInfo[i].min) v = kInfo[i].min;
    if (v > kInfo[i].max) v = kInfo[i].max;
    params_[i] = v;
  }
  paramsValid_ = true;
  mapParams();
}

void BassVoice::setTempo(float bpm) {
  bpm_ = clampf(bpm, 20.0f, 400.0f);
  lfoInc_ = bpm_ * (1.0f / 60.0f) / pLfoBeats_ * ctlDt_;
}

// Map the uint8 params to physical units. Runs only when a param changed;
// uses the fast exp so a knob sweep costs well under one sample's budget.
void BassVoice::mapParams() {
  const uint8_t* p = params_;
  const float perQ = ctlDt_;

  pCutOct_ = unit(p[kCutoff]) * kCutRangeOct;

  // Resonance: damping (1 - k/17) falls as a power curve so the knob's
  // whole travel is useful; the last few steps cross into self-oscillation.
  {
    float r = unit(p[kReso]);
    float k;
    if (r < kResOnset) {
      float d = 1.0f - r * (1.0f / kResOnset);
      k = kResMin + (17.0f - kResMin) * (1.0f - d * sqrtf(d));      // d^1.5
    } else {
      k = 17.0f + (r - kResOnset) * (0.9f / (1.0f - kResOnset));
    }
    pK_ = k;
    // Passband gain of the ladder is 1/(1+k). Give back half of it (in dB):
    // the body thins out as on a 303 but doesn't vanish.
    pMakeup_ = sqrtf((1.0f + kResMin) * (1.0f + k));
    pAccCoef_ = 1.0f - fastExp(-perQ / (0.003f + 0.022f * r));
  }

  pEnvOct_ = unit(p[kEnvMod]) * kEnvRangeOct;
  {
    float tau = 0.020f * fastExp2(unit(p[kDecay]) * 7.0f);
    pFDecCoef_ = fastExp(-perQ / tau);
    // Accent forces a short decay (303), scaled by the accent amount so ACC 0 is a no-op.
    // Fully short from ACC ~ 1/3 up; below that it fades in.
    const float acc = clampf(unit(p[kAccent]) * 3.0f, 0.0f, 1.0f);
    const float tauShort = tau < 0.070f ? tau : 0.070f;
    pFDecCoefAcc_ = fastExp(-perQ / (tau + acc * (tauShort - tau)));
  }
  pAccent_ = unit(p[kAccent]);
  pAccOct_ = pAccent_ * kAccRangeOct;

  {
    float d = unit(p[kDrive]);
    pPre_ = 0.5f * fastExp2(d * 1.6f);              // 0.5 .. 1.5 into the ladder
    pDriveGain_ = fastExp2(d * 5.0f);               // 0 .. +30 dB into the clipper
    pBias_ = 0.35f * d;                             // asymmetry -> even harmonics
    pPostGain_ = 1.0f / (1.0f + 1.5f * d);          // keep loudness roughly level
  }

  pLfoOct_ = unit(p[kLfoDepth]) * kLfoRangeOct;
  pLfoBeats_ = kRateBeats[p[kLfoRate] < kNumRates ? p[kLfoRate] : kNumRates - 1];
  lfoInc_ = bpm_ * (1.0f / 60.0f) / pLfoBeats_ * perQ;

  {
    float w = unit(p[kWave]);
    float m = w * 2.0f;
    pPulseMix_ = m < 1.0f ? m : 1.0f;
    float n = (w - 0.5f) * 2.0f;
    pPw_ = 0.5f - 0.4f * (n > 0.0f ? n : 0.0f);
    pOscNorm_ = 1.0f - 0.35f * pPulseMix_;          // square is ~4.8 dB hotter than a saw
  }

  {
    float s = unit(p[kSub]);
    pSub_ = 1.1f * s * (0.3f + 0.7f * s);
    pSubHp_ = 1.3f * s * s;                         // high-pass corner as a multiple of the sub frequency
  }
  pMix_ = unit(p[kFmMix]);
  {
    float f = unit(p[kFmDepth]);
    pFmIndex_ = 1.25f * f * f;                      // cycles (x 2 pi = modulation index, up to 7.9)
  }
  pFmRatio_ = kRatios[p[kFmRatio] < kNumRatios ? p[kFmRatio] : kNumRatios - 1];
  {
    float f = unit(p[kFold]);
    float m = f * 4.0f;
    pFoldMix_ = m < 1.0f ? m : 1.0f;
    pFoldCycles_ = 0.25f + 1.5f * f * (0.4f + 0.6f * f);   // max 1.75: never ends on a pure-sine fold of the saw
  }
  pTimbreEnv_ = unit(p[kTimbreEnv]);
  pLfoTimbre_ = unit(p[kLfoTimbre]);
  pLfoShape_ = p[kLfoShape] < kNumLfoShapes ? p[kLfoShape] : uint8_t(0);

  {
    float t = 0.005f * fastExp2(unit(p[kSlide]) * 7.0f);      // 95 % time
    pGlideCoef_ = 1.0f - fastExp(-perQ * 3.0f / t);
  }
  if (p[kAmpDecay] >= 127) {
    pADecCoef_ = 1.0f;
  } else {
    float tau = 0.050f * fastExp2(float(p[kAmpDecay]) * (6.5f / 126.0f));
    pADecCoef_ = fastExp(-perQ / tau);
  }
  {
    float tau = 0.002f * fastExp2(unit(p[kRelease]) * 8.0f);
    pRelCoef_ = fastExp(-perQ / tau);
  }
  {
    float t = 0.0007f * fastExp2(unit(p[kAttack]) * 8.0f);
    float inc = perQ / t;
    pAtkInc_ = inc < 0.5f ? inc : 0.5f;             // never faster than two quanta
  }
  pKeyTrack_ = unit(p[kKeyTrack]);
  pSubRatio_ = p[kSubOct] ? 0.5f : 1.0f;
  pTuneRatio_ = fastExp2((float(p[kTune]) - 24.0f) * (1.0f / 12.0f));
}

void BassVoice::resetAudioState() {
  phA_ = phC_ = phM_ = phS_ = 0;
  std::memset(ring_, 0, sizeof ring_);
  ringPos_ = 0;
  s1_ = s2_ = s3_ = s4_ = 0.0f;
  drvZ_ = drvF_ = 0.0f;
  hpLp_ = 0.0f;
  aenv_ = ampCur_ = 0.0f;
  fenv_ = 0.0f;
}

// ============================================================================
// Note handling
// ============================================================================
void BassVoice::noteOn(const NoteEvent& e) {
  const bool wasIdle = (stage_ == kIdle) || (stage_ == kChokeStage);
  const float x = float(e.vel > 127 ? 127 : e.vel) * (1.0f / 127.0f);
  const float velGain = x * (0.3f + 0.7f * x);      // ~ -10 dB at vel 64, -18 dB at 32
  levelTarget_ = kOutGain * velGain * (e.accent ? 1.0f + kAccGain * pAccent_ : 1.0f);
  noteTarget_ = float(e.note);
  curNote_ = e.note;

  // 303 slide: gate stays high, pitch glides, nothing retriggers.
  if (e.slide && !wasIdle && gate_) {
    glide_ = true;
    return;
  }

  if (e.slide && !wasIdle) {
    glide_ = true;                 // gate already released: glide, but the note must sound
  } else {
    glide_ = false;
    noteCur_ = noteTarget_;
  }
  if (stage_ == kIdle) {
    resetAudioState();
    snap_ = true;
  } else if (aenv_ < 0.02f) {
    phA_ = phC_ = phM_ = phS_ = 0;  // inaudible: restart phases so every attack is identical
  }
  gate_ = true;
  stage_ = kAttackStage;
  accentOn_ = e.accent;
  fenv_ = 1.0f;
  fdecCoefCur_ = e.accent ? pFDecCoefAcc_ : pFDecCoef_;
  lfoPhase_ = 0.0f;
  rng_ = rng_ * 1664525u + 1013904223u;
  lfoHold_ = float(rng_ >> 8) * (1.0f / kTwo24);
}

void BassVoice::noteOff(uint8_t note) {
  if (!gate_ || note != curNote_) return;
  gate_ = false;
  if (stage_ != kIdle && stage_ != kChokeStage) stage_ = kReleaseStage;
}

void BassVoice::choke() {
  gate_ = false;
  if (stage_ == kIdle) return;
  stage_ = kChokeStage;
  float quanta = 0.002f / ctlDt_;                   // ~2 ms
  if (quanta < 2.0f) quanta = 2.0f;
  chokeDec_ = aenv_ / quanta + 1e-6f;
}

// ============================================================================
// Render
// ============================================================================
void BassVoice::render(float* outL, float* outR, int frames) {
  while (frames > 0) {
    const int n = frames < kQuantum ? frames : kQuantum;
    frames -= n;
    if (stage_ == kIdle) {
      for (int i = 0; i < n; ++i) outL[i] = outR[i] = 0.0f;
      outL += n;
      outR += n;
      continue;
    }

    // ------------------------------------------------------------------ pitch
    if (glide_) {
      float d = noteTarget_ - noteCur_;
      if (d < 0.002f && d > -0.002f) {
        noteCur_ = noteTarget_;
        glide_ = false;
      } else {
        noteCur_ += d * pGlideCoef_;
      }
    }
    const float fNote = 8.1757989f * fastExp2(noteCur_ * (1.0f / 12.0f));
    float fOscN = fNote * pTuneRatio_ * invSr_;       // cycles per sample
    if (fOscN > 0.2f) fOscN = 0.2f;
    float fSubN = fNote * pSubRatio_ * invSr_;
    if (fSubN > 0.2f) fSubN = 0.2f;
    const uint32_t incA = uint32_t(int32_t(fOscN * kTwo32));
    const float incAf = float(int32_t(incA));
    const float invInc = 1.0f / incAf;
    const uint32_t incS = uint32_t(int32_t(fSubN * kTwo32));
    float incMf = incAf * pFmRatio_;
    if (incMf > 2.0e9f) incMf = 2.0e9f;
    const uint32_t incM = uint32_t(int32_t(incMf));

    // -------------------------------------------------------------- envelopes
    const float fenv = fenv_;                         // value used for this quantum
    fenv_ *= fdecCoefCur_;
    accLp_ += pAccCoef_ * ((accentOn_ ? fenv : 0.0f) - accLp_);

    // LFO, unipolar 0..1
    float lfo;
    {
      const float ph = lfoPhase_;
      switch (pLfoShape_) {
        default:
        case kLfoTri: lfo = ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph; break;
        case kLfoSine: lfo = 0.5f + 0.5f * sineLerp(uint32_t(int32_t((ph + 0.75f) * kTwo24)) << 8); break;
        case kLfoDown: lfo = 1.0f - ph; break;
        case kLfoUp: lfo = ph; break;
        case kLfoSquare: lfo = ph < 0.5f ? 1.0f : 0.0f; break;
        case kLfoSH: lfo = lfoHold_; break;
      }
      lfoPhase_ += lfoInc_;
      if (lfoPhase_ >= 1.0f) {
        lfoPhase_ -= 1.0f;
        if (lfoPhase_ >= 1.0f) lfoPhase_ = 0.0f;
        rng_ = rng_ * 1664525u + 1013904223u;
        lfoHold_ = float(rng_ >> 8) * (1.0f / kTwo24);
      }
    }
    lfoVal_ = lfo;

    // ----------------------------------------------------------------- cutoff
    float cutOct = pCutOct_ + pKeyTrack_ * (noteCur_ - 36.0f) * (1.0f / 12.0f) + pEnvOct_ * fenv +
                   pAccOct_ * accLp_ + pLfoOct_ * lfo;
    if (snap_) cutLag_ = cutOct;
    cutLag_ += kCutLag * (cutOct - cutLag_);
    float frN = kCutBaseHz * invSr_ * fastExp2(clampf(cutLag_, -2.0f, 12.0f));   // cycles per sample
    // Ceiling at fs/5: the loop saturator's 3rd harmonic of the resonance then
    // aliases to >= 2x the cutoff, where the ladder has already rolled off.
    frN = clampf(frN, 16.0f * invSr_, 0.2f);
    dbgCutoffHz_ = frN * sr_;

    // ------------------------------------------------- smoothed level params
    const float gainATarget = (1.0f - pMix_) * pOscNorm_;
    if (snap_) {
      sK_ = pK_; sMakeup_ = pMakeup_; sPre_ = pPre_; sDriveGain_ = pDriveGain_; sBias_ = pBias_;
      sPostGain_ = pPostGain_; sPulseMix_ = pPulseMix_; sPw_ = pPw_; sGainA_ = gainATarget;
      sGainFm_ = pMix_; sSub_ = pSub_; sSubHp_ = pSubHp_; sFoldMix_ = pFoldMix_;
      sFoldCycles_ = pFoldCycles_; sFmIndex_ = pFmIndex_;
      levelSm_ = levelTarget_;
      snap_ = false;
    } else {
      smooth(sK_, pK_); smooth(sMakeup_, pMakeup_); smooth(sPre_, pPre_);
      smooth(sDriveGain_, pDriveGain_); smooth(sBias_, pBias_); smooth(sPostGain_, pPostGain_);
      smooth(sPulseMix_, pPulseMix_); smooth(sPw_, pPw_); smooth(sGainA_, gainATarget);
      smooth(sGainFm_, pMix_); smooth(sSub_, pSub_); smooth(sSubHp_, pSubHp_);
      smooth(sFoldMix_, pFoldMix_); smooth(sFoldCycles_, pFoldCycles_); smooth(sFmIndex_, pFmIndex_);
      smooth(levelSm_, levelTarget_);
    }

    // Timbre modulation (FM index and fold depth follow the filter envelope / LFO).
    const float tm = (1.0f - pTimbreEnv_ * (1.0f - fenv)) * (1.0f - pLfoTimbre_ * (1.0f - lfo));
    // Keep the FM sidebands below Nyquist: f_c + (beta + 3) f_m < 0.45 fs
    // (Carson's rule plus two more sideband pairs, which brings the first
    // sideband past the limit to < -45 dB). Only bites on high notes / ratios.
    float betaMax = (0.45f / fOscN - 1.0f) / pFmRatio_ - 3.0f;
    float idxMax = betaMax > 0.0f ? betaMax * (1.0f / kTwoPi) : 0.0f;
    float idx = sFmIndex_ * tm;
    if (idx > idxMax) idx = idxMax;
    const float fmIdx24 = idx * kTwo24;
    // Same idea for the folder: its output bandwidth is roughly
    // 2 pi cycles x (peak instantaneous frequency of its input). Key-scale the
    // depth so high notes stay clean instead of turning into alias mush.
    float foldCyc = 0.25f + (sFoldCycles_ - 0.25f) * tm;
    float foldMix = sFoldMix_;
    {
      const float slope = kTwoPi * fOscN * (1.0f + kTwoPi * idx * pFmRatio_ * sGainFm_);
      // peak instantaneous frequency of the folded output <= 0.12 fs
      const float cycMax = 0.12f / slope;
      if (foldCyc > cycMax) {
        foldCyc = cycMax > 0.25f ? cycMax : 0.25f;
        // Even the gentlest shape (a quarter-cycle sine) is too wide: fade the folder out.
        if (cycMax < 0.25f) foldMix *= cycMax * 4.0f;
      }
    }
    const float foldScale24 = foldCyc * kTwo24;

    // ---------------------------------------------------- ladder coefficients
    // Zero-delay-feedback diode ladder:
    //   y1' = wc (u + y2 - y1),  y2' = wc ((y1 + y3)/2 - y2),
    //   y3' = wc ((y2 + y4)/2 - y3),  y4' = wc (y3/2 - y4),  u = sat(x - k y4)
    // H(s) = 1 / (8s^4 + 32s^3 + 40s^2 + 16s + 1 + k): self-oscillation at
    // k = 17, at w = wc / sqrt(2). Pre-warp so that frequency is exact.
    const float g = 1.41421356f * fastTan(kPi * frN);
    const float B = 1.0f / (1.0f + g);
    const float G = g * B;
    const float h = 0.5f * G;
    const float d3 = 1.0f / (1.0f - h * h);
    const float a3 = h * d3;
    const float d2 = 1.0f / (1.0f - h * a3);
    const float a2 = h * d2;
    const float d1 = 1.0f / (1.0f - G * a2);
    const float a1 = G * d1;
    const float a4 = h;
    const float e3 = h * d3, f3 = B * d3;
    const float e2 = h * d2, f2 = B * d2;
    const float e1 = G * d1, f1 = B * d1;
    const float gamma = a1 * a2 * a3 * a4;
    const float k = sK_;
    const float D = 1.0f / (1.0f + k * gamma);
    const float pre = sPre_;
    const float postIn = sMakeup_ * sDriveGain_;       // ladder output -> clipper input
    const float bias = sBias_;
    const float postOut = sPostGain_;
    const float biasOut = softClip(bias);

    // -------------------------------------------------------------------- amp
    switch (stage_) {
      case kAttackStage:
        aenv_ += pAtkInc_;
        if (aenv_ >= 1.0f) { aenv_ = 1.0f; stage_ = kDecayStage; }
        break;
      case kDecayStage: aenv_ *= pADecCoef_; break;
      case kReleaseStage: aenv_ *= pRelCoef_; break;
      case kChokeStage: aenv_ -= chokeDec_; break;
      default: break;
    }
    bool finished = false;
    if (aenv_ < 1.0e-4f && stage_ != kAttackStage) { aenv_ = 0.0f; finished = true; }
    const float ampTarget = aenv_ * levelSm_;
    const float ampStep = (ampTarget - ampCur_) / float(n);
    float amp = ampCur_;

    // high-pass corner: 12 Hz DC blocker, rising toward the sub's pitch as SUB comes up
    float hpC = kTwoPi * (12.0f * invSr_ + sSubHp_ * fSubN);
    if (hpC > 0.5f) hpC = 0.5f;
    const float subG = sSub_;
    const float gainA = sGainA_, gainFm = sGainFm_, pulseMix = sPulseMix_;
    const uint32_t pwOff = uint32_t(int32_t(sPw_ * kTwo24)) << 8;
    const float stepB = 2.0f * gainA * pulseMix;

    // ------------------------------------------------------------- inner loop
    uint32_t phA = phA_, phC = phC_, phM = phM_, phS = phS_, pos = ringPos_;
    float s1 = s1_, s2 = s2_, s3 = s3_, s4 = s4_;
    float drvZ = drvZ_, drvF = drvF_, hpLp = hpLp_;
    float* ring = ring_;

    for (int i = 0; i < n; ++i) {
      // OSC A (naive saw minus a shifted saw) + OSC B (phase-modulated sine
      // pair), wavefolded, all in the naive domain 8 samples ahead of the
      // output. The waveform discontinuities are then band-limited with table
      // BLEPs scaled by the *folded* jump, so folding never sees a steep
      // band-limited edge (which would alias badly). With FOLD = 0 the jumps
      // reduce to the plain saw/pulse steps.
      phA += incA;
      const uint32_t phB = phA + pwOff;
      phM += incM;
      phC += incA;
      const float mod = sineLerp(phM);
      const float naive = (float(int32_t(phA)) - pulseMix * float(int32_t(phB))) * (kInt31ToFloat * gainA) +
                          gainFm * sineLerp(phC + (uint32_t(int32_t(mod * fmIdx24)) << 8));
      const float yN = naive + foldMix * (sineLerp(uint32_t(int32_t(naive * foldScale24)) << 8) - naive);
      ring[(pos + 8u) & 15u] += yN;
      const uint32_t qA = phA + 0x80000000u, qB = phB + 0x80000000u;
      if (qA < incA) {
        // saw A wrapped: the value just before the edge was 2 * gainA higher
        const float pre0 = naive + 2.0f * gainA;
        const float yPre = pre0 + foldMix * (sineLerp(uint32_t(int32_t(pre0 * foldScale24)) << 8) - pre0);
        const float jump = yN - yPre;
        float t = float(int32_t(qA)) * invInc * float(kBlepPhases);
        int ip = int(t);
        float fr = t - float(ip);
        const float* r0 = gBlep + ip * kBlepTaps;
        for (uint32_t j = 0; j < 16u; ++j) {
          float v = r0[j] + fr * (r0[j + kBlepTaps] - r0[j]);
          ring[(pos + j) & 15u] += jump * v;
        }
      }
      if (qB < incA && pulseMix > 0.0f) {
        // saw B wrapped: the (subtracted) value just before was 2 * gainA * pulseMix lower
        const float pre0 = naive - stepB;
        const float yPre = pre0 + foldMix * (sineLerp(uint32_t(int32_t(pre0 * foldScale24)) << 8) - pre0);
        const float jump = yN - yPre;
        float t = float(int32_t(qB)) * invInc * float(kBlepPhases);
        int ip = int(t);
        float fr = t - float(ip);
        const float* r0 = gBlep + ip * kBlepTaps;
        for (uint32_t j = 0; j < 16u; ++j) {
          float v = r0[j] + fr * (r0[j + kBlepTaps] - r0[j]);
          ring[(pos + j) & 15u] += jump * v;
        }
      }
      float x = ring[pos];
      ring[pos] = 0.0f;
      pos = (pos + 1u) & 15u;

      x *= pre;

      // Diode ladder, zero-delay feedback with one saturator at the summing node.
      const float c4 = B * s4;
      const float c3 = e3 * c4 + f3 * s3;
      const float c2 = e2 * c3 + f2 * s2;
      const float c1 = e1 * c2 + f1 * s1;
      const float sig = a4 * (a3 * (a2 * c1 + c2) + c3) + c4;
      const float y4lin = (gamma * x + sig) * D;        // linear ZDF solution
      const float u = softClip(x - k * y4lin);
      const float y1 = a1 * u + c1;
      const float y2 = a2 * y1 + c2;
      const float y3 = a3 * y2 + c3;
      const float y4 = a4 * y3 + c4;
      s1 = 2.0f * y1 - s1;
      s2 = 2.0f * y2 - s2;
      s3 = 2.0f * y3 - s3;
      s4 = 2.0f * y4 - s4;

      // Post drive: asymmetric soft clip with first-order ADAA.
      const float z = y4 * postIn + bias;
      const float F = softClipInt(z);
      const float dz = z - drvZ;
      float dv;
#ifndef PG_BASS_NO_ADAA
      if (dz > 2.0e-3f || dz < -2.0e-3f) dv = (F - drvF) / dz;
      else dv = softClip(0.5f * (z + drvZ));
#else
      (void)dz; dv = softClip(z);
#endif
      drvZ = z;
      drvF = F;
      const float drv = (dv - biasOut) * postOut;

      // DC blocker / crossover high-pass, then the clean sub, then the VCA.
      hpLp += hpC * (drv - hpLp);
      phS += incS;
      const float o = ((drv - hpLp) + subG * sineLerp(phS)) * amp;
      amp += ampStep;
      outL[i] = o;
      outR[i] = o;
    }

    phA_ = phA; phC_ = phC; phM_ = phM; phS_ = phS; ringPos_ = pos;
    s1_ = s1; s2_ = s2; s3_ = s3; s4_ = s4;
    drvZ_ = drvZ; drvF_ = drvF; hpLp_ = hpLp;
    ampCur_ = ampTarget;
    if (finished) {
      stage_ = kIdle;
      gate_ = false;
    }
    outL += n;
    outR += n;
  }
}

}  // namespace bass
}  // namespace pg
