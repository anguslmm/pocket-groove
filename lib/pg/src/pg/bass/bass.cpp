// Pocket Groove — bass voice implementation. See bass.h for the signal path.
#include "bass.h"

#include <cmath>     // init()/param-change only (sqrtf, log2f); never per sample
#include <cstring>

#include "bass_math.h"

namespace pg {
namespace bass {

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
// Top of the cutoff range: the summed CV (CUT + env + accent + LFO + key
// track, in octaves) runs into a soft knee that starts kKneeOct below the
// fs/5 ceiling and approaches it asymptotically (slope 1 at the knee, so
// everything below it is untouched). Inside the knee the ladder feedback
// tapers towards its minimum, so a cutoff parked at the ceiling cannot ring
// at a fixed pitch on every note; CUT keeps opening the filter up to 127.
constexpr float kKneeOct = 0.75f;   // knee from ~5.2 kHz at 44.1 kHz: above anything the default patch reaches
// Passband loss compensation goes from "half back" to "all back" as the
// knob's static cutoff (CUT + key track, octaves over 40 Hz) rises from
// ~640 Hz to ~3.6 kHz (CUT ~66 .. ~107 at key-track centre).
constexpr float kCompLoOct = 4.0f;
constexpr float kCompHiOct = 6.5f;
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
  sr_ = sampleRate > 8000.0f ? sampleRate : 8000.0f;
  invSr_ = 1.0f / sr_;
  ctlDt_ = float(kQuantum) * invSr_;
  kneeStart_ = log2f(0.2f * sr_ / kCutBaseHz) - kKneeOct;   // init only (libm is fine here)
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
namespace {

// The per-sample loop of BassVoice::render, specialised on which optional
// sources are on. A source whose gain is exactly 0 contributes exactly +0 to
// the sum, so skipping its sine lookups is bit-identical; its phase still
// advances (per block instead of per sample, same value mod 2^32).
struct BassLoop {
  uint32_t incA, incM, incS, pwOff;
  float invInc, gainA, gainFm, pulseMix, stepB, fmIdx24, foldMix, foldScale24;
  float pre, B, e1, e2, e3, f1, f2, f3, a1, a2, a3, a4, gamma, k, D;
  float postIn, bias, biasOut, postOut, hpC, subG, ampStep;
  // state (in/out)
  float amp;
  uint32_t phA, phC, phM, phS, pos;
  float s1, s2, s3, s4, drvZ, drvF, hpLp;
  float* ring;
};

template <bool kFm, bool kFold, bool kSub, bool kPulse>
void bassLoop(BassLoop& Lp, float* outL, float* outR, int n) {
  const uint32_t incA = Lp.incA, incM = Lp.incM, incS = Lp.incS, pwOff = Lp.pwOff;
  const float invInc = Lp.invInc, gainA = Lp.gainA, gainFm = Lp.gainFm, pulseMix = Lp.pulseMix, stepB = Lp.stepB;
  const float fmIdx24 = Lp.fmIdx24, foldMix = Lp.foldMix, foldScale24 = Lp.foldScale24;
  const float pre = Lp.pre, B = Lp.B, e1 = Lp.e1, e2 = Lp.e2, e3 = Lp.e3, f1 = Lp.f1, f2 = Lp.f2, f3 = Lp.f3;
  const float a1 = Lp.a1, a2 = Lp.a2, a3 = Lp.a3, a4 = Lp.a4, gamma = Lp.gamma, k = Lp.k, D = Lp.D;
  const float postIn = Lp.postIn, bias = Lp.bias, biasOut = Lp.biasOut, postOut = Lp.postOut, hpC = Lp.hpC;
  const float subG = Lp.subG, ampStep = Lp.ampStep;
  float amp = Lp.amp;
  uint32_t phA = Lp.phA, phC = Lp.phC, phM = Lp.phM, phS = Lp.phS, pos = Lp.pos;
  float s1 = Lp.s1, s2 = Lp.s2, s3 = Lp.s3, s4 = Lp.s4, drvZ = Lp.drvZ, drvF = Lp.drvF, hpLp = Lp.hpLp;
  float* ring = Lp.ring;
  (void)incM; (void)incS; (void)gainFm; (void)fmIdx24; (void)foldMix; (void)foldScale24; (void)subG;
  for (int i = 0; i < n; ++i) {
      // OSC A (naive saw minus a shifted saw) + OSC B (phase-modulated sine
      // pair), wavefolded, all in the naive domain 8 samples ahead of the
      // output. The waveform discontinuities are then band-limited with table
      // BLEPs scaled by the *folded* jump, so folding never sees a steep
      // band-limited edge (which would alias badly). With FOLD = 0 the jumps
      // reduce to the plain saw/pulse steps.
      phA += incA;
      const uint32_t phB = phA + pwOff;
      float naive = (kPulse ? float(int32_t(phA)) - pulseMix * float(int32_t(phB)) : float(int32_t(phA))) *
                    (kInt31ToFloat * gainA);
      if (kFm) {
        phM += incM;
        phC += incA;
        const float mod = sineLerp(phM);
        naive = naive + gainFm * sineLerp(phC + (uint32_t(int32_t(mod * fmIdx24)) << 8));
      }
      const float yN = kFold ? naive + foldMix * (sineLerp(uint32_t(int32_t(naive * foldScale24)) << 8) - naive) : naive;
      ring[(pos + 8u) & 15u] += yN;
      const uint32_t qA = phA + 0x80000000u, qB = phB + 0x80000000u;
      if (qA < incA) {
        // saw A wrapped: the value just before the edge was 2 * gainA higher
        const float pre0 = naive + 2.0f * gainA;
        const float yPre = kFold ? pre0 + foldMix * (sineLerp(uint32_t(int32_t(pre0 * foldScale24)) << 8) - pre0) : pre0;
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
      if (kPulse && qB < incA && pulseMix > 0.0f) {
        // saw B wrapped: the (subtracted) value just before was 2 * gainA * pulseMix lower
        const float pre0 = naive - stepB;
        const float yPre = kFold ? pre0 + foldMix * (sineLerp(uint32_t(int32_t(pre0 * foldScale24)) << 8) - pre0) : pre0;
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
      float o = drv - hpLp;
      if (kSub) {
        phS += incS;
        o += subG * sineLerp(phS);
      }
      o *= amp;
      amp += ampStep;
      outL[i] = o;
      outR[i] = o;
  }
  if (!kFm) {
    phM += incM * uint32_t(n);
    phC += incA * uint32_t(n);
  }
  if (!kSub) phS += incS * uint32_t(n);
  Lp.amp = amp;
  Lp.phA = phA; Lp.phC = phC; Lp.phM = phM; Lp.phS = phS; Lp.pos = pos;
  Lp.s1 = s1; Lp.s2 = s2; Lp.s3 = s3; Lp.s4 = s4; Lp.drvZ = drvZ; Lp.drvF = drvF; Lp.hpLp = hpLp;
}

using BassLoopFn = void (*)(BassLoop&, float*, float*, int);
#define PG_BASS_LOOPS(P)                                                                          \
  bassLoop<false, false, false, P>, bassLoop<false, false, true, P>, bassLoop<false, true, false, P>, \
      bassLoop<false, true, true, P>, bassLoop<true, false, false, P>, bassLoop<true, false, true, P>, \
      bassLoop<true, true, false, P>, bassLoop<true, true, true, P>
const BassLoopFn kBassLoops[16] = {PG_BASS_LOOPS(false), PG_BASS_LOOPS(true)};
#undef PG_BASS_LOOPS

}  // namespace

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
    // Soft knee towards the fs/5 ceiling (see kKneeOct). It starts at the
    // knee point or at the knob's own static cutoff (CUT + key track),
    // whichever is higher, so CUT itself is never compressed (it keeps
    // moving the filter up to 127); only the modulation travel above it is.
    float cutCv = cutLag_;
    float kneeDepth = 0.0f;                           // 0 below the knee .. -> 1 at the ceiling
    float openExt = 0.0f;                             // 0 .. 1: CUT's own position in the knee
    // The knob's own (static) cutoff: CUT + key tracking, no modulation.
    const float base = pCutOct_ + pKeyTrack_ * (noteCur_ - 36.0f) * (1.0f / 12.0f);
    {
      const float ceil = kneeStart_ + kKneeOct;
      const float s0 = clampf(base, kneeStart_, ceil - 0.02f);
      if (cutCv > s0) {
        const float w = ceil - s0, u = cutCv - s0;
        cutCv = s0 + w * u / (w + u);
      }
      if (cutCv > kneeStart_) kneeDepth = clampf((cutCv - kneeStart_) * (1.0f / kKneeOct), 0.0f, 1.0f);
      // "Open" at the top of the knob: once CUT itself is in the knee, the
      // resonance there is tapered away (below), so the fs/5 cap (which only
      // exists to keep a strong resonance from aliasing through the loop
      // saturator) can lift towards 0.36 fs: the last stretch of CUT opens the
      // filter fully instead of leaving its own roll-off on the top octave.
      openExt = clampf((base - kneeStart_) * (1.0f / kKneeOct), 0.0f, 1.0f);
    }
    float frN = kCutBaseHz * invSr_ * fastExp2(clampf(cutCv, -2.0f, 12.0f));   // cycles per sample
    // Ceiling at fs/5: the loop saturator's 3rd harmonic of the resonance then
    // aliases to >= 2x the cutoff, where the ladder has already rolled off.
    if (openExt > 0.0f) frN *= 1.0f + openExt * 0.9f;   // up to ~0.93 octave further at CUT 127
    frN = clampf(frN, 16.0f * invSr_, 0.2f + 0.16f * openExt);
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
    float k = sK_, makeup = sMakeup_;
    // Passband compensation. sMakeup_ gives back half of the ladder's
    // 1/(1+k) passband loss: right while the cutoff is low, where the
    // resonant peak sits on the strong low harmonics and carries the level
    // itself. Once the cutoff is up among the weak upper harmonics only the
    // passband is left, so the rest is given back too, or opening the filter
    // would hollow the bass out at high RES. Keyed to the knob's static
    // cutoff (blend over kCompLoOct .. kCompHiOct), so envelope sweeps of a
    // low-CUT patch sound exactly as before.
    if (base > kCompLoOct) {
      float b = clampf((base - kCompLoOct) * (1.0f / (kCompHiOct - kCompLoOct)), 0.0f, 1.0f);
      b = b * b * (3.0f - 2.0f * b);                                       // smoothstep
      makeup *= 1.0f + b * (sqrtf((1.0f + k) * (1.0f / (1.0f + kResMin))) - 1.0f);
    }
    if (kneeDepth > 0.0f) {
      // Resonance taper inside the knee (the ladder's passband gain is
      // 1/(1+k): compensate it fully so the taper changes tone, not level).
      // Gentle at first (the acid squelch survives most of the knee), gone
      // at the ceiling, where a parked cutoff would otherwise whistle.
      const float t = 1.0f - kneeDepth * kneeDepth * kneeDepth;
      const float kE = kResMin + (k - kResMin) * t;
      makeup *= (1.0f + kE) / (1.0f + k);   // keep the passband level where the RES knob put it
      k = kE;
    }
    const float D = 1.0f / (1.0f + k * gamma);
    const float pre = sPre_;
    const float postIn = makeup * sDriveGain_;         // ladder output -> clipper input
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
    BassLoop lp;
    lp.incA = incA; lp.incM = incM; lp.incS = incS; lp.pwOff = pwOff;
    lp.invInc = invInc; lp.gainA = gainA; lp.gainFm = gainFm; lp.pulseMix = pulseMix; lp.stepB = stepB;
    lp.fmIdx24 = fmIdx24; lp.foldMix = foldMix; lp.foldScale24 = foldScale24;
    lp.pre = pre; lp.B = B; lp.e1 = e1; lp.e2 = e2; lp.e3 = e3; lp.f1 = f1; lp.f2 = f2; lp.f3 = f3;
    lp.a1 = a1; lp.a2 = a2; lp.a3 = a3; lp.a4 = a4; lp.gamma = gamma; lp.k = k; lp.D = D;
    lp.postIn = postIn; lp.bias = bias; lp.biasOut = biasOut; lp.postOut = postOut; lp.hpC = hpC;
    lp.subG = subG; lp.ampStep = ampStep; lp.amp = amp;
    lp.phA = phA_; lp.phC = phC_; lp.phM = phM_; lp.phS = phS_; lp.pos = ringPos_;
    lp.s1 = s1_; lp.s2 = s2_; lp.s3 = s3_; lp.s4 = s4_; lp.drvZ = drvZ_; lp.drvF = drvF_; lp.hpLp = hpLp_;
    lp.ring = ring_;
    kBassLoops[(pulseMix != 0.0f ? 8 : 0) + (gainFm != 0.0f ? 4 : 0) + (foldMix != 0.0f ? 2 : 0) + (subG != 0.0f ? 1 : 0)](
        lp, outL, outR, n);
    phA_ = lp.phA; phC_ = lp.phC; phM_ = lp.phM; phS_ = lp.phS; ringPos_ = lp.pos;
    s1_ = lp.s1; s2_ = lp.s2; s3_ = lp.s3; s4_ = lp.s4;
    drvZ_ = lp.drvZ; drvF_ = lp.drvF; hpLp_ = lp.hpLp;
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
