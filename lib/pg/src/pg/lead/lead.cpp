#include "lead.h"

#include <cmath>
#include <cstring>

#include "lead_math.h"
#include "lead_tables.h"

namespace pg {
namespace lead {
namespace {

// ---------------------------------------------------------------------------
// Parameter table
// ---------------------------------------------------------------------------
const char* const kWaveNames[kNumWaves] = {"SAW", "PULS", "TRI", "SUPR", "FM1", "FM2", "BELL", "ORGN"};
const char* const kOsc2Names[kNumOsc2Waves] = {"SAW", "PULS", "TRI", "SINE"};
const char* const kFilterNames[kNumFilterTypes] = {"LP", "BP", "HP"};

const ParamInfo kInfo[kNumParams] = {
    // ---- page 1: live tweaks
    {"CUT", "Cutoff", 0, 127, 72, ParamFormat::Number, nullptr},
    {"RES", "Resonance", 0, 127, 30, ParamFormat::Number, nullptr},
    {"ENV", "Env Amount", 0, 127, 48, ParamFormat::Number, nullptr},
    {"FDCY", "Env Decay", 0, 127, 60, ParamFormat::Number, nullptr},
    {"WAVE", "Osc1 Wave", 0, kNumWaves - 1, kWaveSaw, ParamFormat::Enum, kWaveNames},
    {"SHP", "Shape", 0, 127, 0, ParamFormat::Number, nullptr},
    {"DET", "Detune", 0, 127, 56, ParamFormat::Number, nullptr},
    {"DCY", "Decay", 0, 127, 70, ParamFormat::Number, nullptr},
    // ---- page 2: envelopes + filter (stereo width here: the device is mono)
    {"ATK", "Attack", 0, 127, 0, ParamFormat::Number, nullptr},
    {"WID", "Width", 0, 127, 96, ParamFormat::Percent, nullptr},
    {"SUS", "Sustain", 0, 127, 80, ParamFormat::Number, nullptr},
    {"REL", "Release", 0, 127, 44, ParamFormat::Number, nullptr},
    {"FATK", "Env Attack", 0, 127, 0, ParamFormat::Number, nullptr},
    {"FLT", "Filter Type", 0, kNumFilterTypes - 1, kFilterLP, ParamFormat::Enum, kFilterNames},
    {"KTRK", "Key Track", 0, 127, 64, ParamFormat::Percent, nullptr},
    {"DRV", "Drive", 0, 127, 0, ParamFormat::Number, nullptr},
    // ---- page 3: osc 2 + modulation
    {"OSC2", "Osc2 Wave", 0, kNumOsc2Waves - 1, kOsc2Saw, ParamFormat::Enum, kOsc2Names},
    {"INTV", "Osc2 Pitch", 0, 2 * kIntervalCentre, kIntervalCentre, ParamFormat::Semitones, nullptr},
    {"MIX", "Osc Mix", 0, 127, 64, ParamFormat::Bipolar, nullptr},
    {"LFO", "LFO Rate", 0, 127, 94, ParamFormat::Number, nullptr},
    {"VIB", "Vibrato", 0, 127, 0, ParamFormat::Number, nullptr},
    {"LSHP", "LFO>Shape", 0, 127, 0, ParamFormat::Number, nullptr},
    {"LCUT", "LFO>Cutoff", 0, 127, 0, ParamFormat::Number, nullptr},
    {"GLID", "Glide", 0, 127, 64, ParamFormat::Number, nullptr},
};

// ---------------------------------------------------------------------------
// Chords. Max 4 notes, voiced for stabs in the middle register.
// ---------------------------------------------------------------------------
struct ChordDef {
  uint8_t n;
  int8_t semis[kMaxChordNotes];
};
const ChordDef kChords[kChordNum] = {
    {1, {0, 0, 0, 0}},     // None
    {2, {0, 12, 0, 0}},    // Oct
    {3, {0, 7, 12, 0}},    // Fifth: power chord with octave
    {4, {0, 4, 7, 12}},    // Maj:  triad + octave
    {4, {0, 3, 7, 12}},    // Min:  triad + octave
    {4, {0, 4, 7, 11}},    // Maj7
    {4, {0, 3, 7, 10}},    // Min7
    {4, {0, 4, 7, 10}},    // Dom7
    {4, {0, 5, 7, 12}},    // Sus4 + octave
    {4, {0, 3, 10, 14}},   // Min9: root, b3, b7, 9 (fifth dropped: the deep-house voicing)
};

// Pan slots (scaled by the current pan width). Lowest note stays near the centre.
const float kPan1[4] = {0.0f, -1.0f, 1.0f, -0.5f};   // single notes: first free slot
const float kPan2[2] = {-0.7f, 0.7f};
const float kPan3[3] = {0.0f, -1.0f, 1.0f};
const float kPan4[4] = {-0.35f, 1.0f, -1.0f, 0.35f};
// osc2 start phase relative to osc1. The device is mono, so what matters is
// the SUM of the two detuned oscillators: osc2 (the sharp one) starts a hair
// behind osc1 and passes through "exactly in phase" this many seconds into
// the note. Every note therefore starts coherent and full (fundamental and
// low harmonics add, no comb notch below ~12x the fundamental) and then
// spreads into the usual detune shimmer, identically on every hit. The four
// chord notes line up at slightly different times so their peaks don't stack.
const float kOsc2InPhaseAt[4] = {0.012f, 0.022f, 0.017f, 0.028f};
// Per-note gain by the size of the chord it belongs to, fixed at note start
// (so nothing pumps as voices come and go): a single-note line or pluck sits
// within ~3 dB of a 4-note stab instead of 7 dB below it. Overlapping single
// notes in poly mode simply add, like any poly synth.
const float kSizeGain[4] = {1.5f, 1.25f, 1.1f, 1.0f};
// Super-saw detune pattern (fractions of the outer spread) and sides.
const float kSuperDet[5] = {0.0f, -1.0f, 0.97f, -0.37f, 0.41f};

constexpr float kSilence   = 1.0e-4f;   // -80 dB: voice is freed
constexpr float kLevel     = 0.090f;    // per-voice gain: 4-note chord at defaults peaks ~-9 dBFS
constexpr float kT60       = 6.9078f;   // ln(1000)
constexpr float kInvQ      = 1.0f / float(kQuantum);
constexpr float kFmGain    = 0.80f;     // sine vs saw loudness trims
constexpr float kOrganGain = 1.44f;
constexpr float kOrganPerc = 0.42f;     // level of the 4' percussion partial at full velocity

enum OscKind : uint8_t { kOne = 0, kTwo };
enum TabSet : uint8_t { kSetSaw = 0, kSetTri, kSetSine };

// One-pole smoothing that lands exactly on the target (no denormal tails).
inline void smoothTo(float& s, float target, float c) {
  const float d = target - s;
  s = (d < 1.0e-5f && d > -1.0e-5f) ? target : s + d * c;
}
inline float norm01(uint8_t v) { return float(v) * (1.0f / 127.0f); }
inline uint32_t cycles(float x) { return uint32_t(int32_t(x * 16777216.0f)) << 8; }
inline uint32_t toInc(float x) { return uint32_t(int32_t(x)); }

// ---------------------------------------------------------------------------
// Inner loops. All of them ADD into the voice's pre-filter buffers. ST is a
// compile-time flag: true = stereo (L and R), false = mono (only L is touched,
// gL is the gain). The mono build of every loop is what the device runs: its
// codec is mono, so the firmware calls LeadVoice::setMono(true).
// ---------------------------------------------------------------------------
#define PG_LEAD_LOOK(out, tab, sh, mk, fsc, phase)                    \
  {                                                                   \
    const uint32_t ph_ = (phase);                                     \
    const uint32_t i_ = ph_ >> (sh);                                  \
    const float fr_ = float(int32_t(ph_ & (mk))) * (fsc);             \
    const int32_t a_ = (tab)[i_];                                     \
    const int32_t b_ = (tab)[i_ + 1];                                 \
    out = float(a_) + float(b_ - a_) * fr_;                           \
  }

// One lookup per sample: saw, triangle, sine.
template <bool ST>
void oscOne(uint32_t& phase, uint32_t inc, const Tab& t, float gL, float gR, float* L, float* R) {
  const int16_t* d = t.d;
  const uint32_t sh = t.shift, mk = t.mask;
  const float fsc = t.fscale;
  uint32_t p = phase;
  for (int i = 0; i < kQuantum; ++i) {
    float s;
    PG_LEAD_LOOK(s, d, sh, mk, fsc, p);
    p += inc;
    L[i] += s * gL;
    if (ST) R[i] += s * gR;
  }
  phase = p;
}

// Two lookups with independent phases and gains: pulse (saw - shifted saw),
// double saw, triangle/saw morph. The second phase has its own increment so a
// moving phase offset (PWM) ramps smoothly instead of stepping per quantum.
template <bool ST>
void oscTwo(uint32_t& phaseA, uint32_t& phaseB, uint32_t incA, uint32_t incB, const Tab& ta, const Tab& tb,
            float gAL, float gAR, float gBL, float gBR, float* L, float* R) {
  const int16_t* da = ta.d;
  const int16_t* db = tb.d;
  const uint32_t sha = ta.shift, mka = ta.mask, shb = tb.shift, mkb = tb.mask;
  const float fa = ta.fscale, fb = tb.fscale;
  uint32_t pa = phaseA, pb = phaseB;
  for (int i = 0; i < kQuantum; ++i) {
    float a, b;
    PG_LEAD_LOOK(a, da, sha, mka, fa, pa);
    PG_LEAD_LOOK(b, db, shb, mkb, fb, pb);
    pa += incA;
    pb += incB;
    L[i] += a * gAL + b * gBL;
    if (ST) R[i] += a * gAR + b * gBR;
  }
  phaseA = pa;
  phaseB = pb;
}

// Five detuned saws: [0] centre, [1],[4] left group, [2],[3] right group.
template <bool ST>
void oscSuper(uint32_t* ph, const uint32_t* inc, const Tab& t, float cL, float cR, float lL, float lR, float rL,
              float rR, float* L, float* R) {
  const int16_t* d = t.d;
  const uint32_t sh = t.shift, mk = t.mask;
  const float fsc = t.fscale;
  uint32_t p0 = ph[0], p1 = ph[1], p2 = ph[2], p3 = ph[3], p4 = ph[4];
  const uint32_t i0 = inc[0], i1 = inc[1], i2 = inc[2], i3 = inc[3], i4 = inc[4];
  for (int i = 0; i < kQuantum; ++i) {
    float c, l1, l2, r1, r2;
    PG_LEAD_LOOK(c, d, sh, mk, fsc, p0);
    PG_LEAD_LOOK(l1, d, sh, mk, fsc, p1);
    PG_LEAD_LOOK(r1, d, sh, mk, fsc, p2);
    PG_LEAD_LOOK(r2, d, sh, mk, fsc, p3);
    PG_LEAD_LOOK(l2, d, sh, mk, fsc, p4);
    p0 += i0; p1 += i1; p2 += i2; p3 += i3; p4 += i4;
    if (ST) {
      const float l = l1 + l2, r = r1 + r2;
      L[i] += c * cL + l * lL + r * rL;
      R[i] += c * cR + l * lR + r * rR;
    } else {
      L[i] += c * cL + (l1 + l2 + r1 + r2) * lL;
    }
  }
  ph[0] = p0; ph[1] = p1; ph[2] = p2; ph[3] = p3; ph[4] = p4;
}

// Two-operator phase modulation. idx is in units of 2^-24 cycles per table unit.
template <bool ST>
void oscFm(uint32_t& car, uint32_t& mod, uint32_t cinc, uint32_t minc, const Tab& t, float idx, float didx,
           float gL, float gR, float* L, float* R) {
  const int16_t* d = t.d;
  const uint32_t sh = t.shift, mk = t.mask;
  const float fsc = t.fscale;
  uint32_t pc = car, pm = mod;
  for (int i = 0; i < kQuantum; ++i) {
    float m, s;
    PG_LEAD_LOOK(m, d, sh, mk, fsc, pm);
    const uint32_t off = uint32_t(int32_t(m * idx)) << 8;
    PG_LEAD_LOOK(s, d, sh, mk, fsc, pc + off);
    idx += didx;
    pc += cinc;
    pm += minc;
    L[i] += s * gL;
    if (ST) R[i] += s * gR;
  }
  car = pc;
  mod = pm;
}

// Drawbar organ: crossfade between two registration tables plus a decaying
// 4' percussion partial. The phase runs at half the note frequency (16').
template <bool ST>
void oscOrgan(uint32_t& phase, uint32_t inc, const Tab& ta, const Tab& tb, const Tab& ts, float x, float dx,
              float perc, float dperc, float gL, float gR, float* L, float* R) {
  const int16_t* da = ta.d;
  const int16_t* db = tb.d;
  const int16_t* ds = ts.d;
  const uint32_t sh = ta.shift, mk = ta.mask, shs = ts.shift, mks = ts.mask;
  const float fsc = ta.fscale, fss = ts.fscale;
  uint32_t p = phase;
  for (int i = 0; i < kQuantum; ++i) {
    float a, b, c;
    PG_LEAD_LOOK(a, da, sh, mk, fsc, p);
    PG_LEAD_LOOK(b, db, sh, mk, fsc, p);
    PG_LEAD_LOOK(c, ds, shs, mks, fss, p << 2);
    const float s = a + (b - a) * x + c * perc;
    x += dx;
    perc += dperc;
    p += inc;
    L[i] += s * gL;
    if (ST) R[i] += s * gR;
  }
  phase = p;
}

// Stereo ZDF (trapezoidal) state-variable filter + amp ramp, added to the bus.
// TYPE is a compile-time constant so each response gets its own tight loop.
struct FilterState {
  float ic1L, ic2L, ic1R, ic2R;
};
template <int TYPE, bool ST>
void filterAmp(FilterState& st, const float* inL, const float* inR, float a1, float a2, float a3, float k, float g,
               float dg, float* outL, float* outR) {
  (void)k;
  if (ST) {
    float ic1L = st.ic1L, ic2L = st.ic2L, ic1R = st.ic1R, ic2R = st.ic2R;
    for (int i = 0; i < kQuantum; ++i) {
      const float xl = inL[i], xr = inR[i];
      const float v3l = xl - ic2L, v3r = xr - ic2R;
      const float v1l = a1 * ic1L + a2 * v3l, v1r = a1 * ic1R + a2 * v3r;
      const float v2l = ic2L + a2 * ic1L + a3 * v3l, v2r = ic2R + a2 * ic1R + a3 * v3r;
      ic1L = 2.0f * v1l - ic1L;
      ic1R = 2.0f * v1r - ic1R;
      ic2L = 2.0f * v2l - ic2L;
      ic2R = 2.0f * v2r - ic2R;
      float yl, yr;
      if (TYPE == kFilterLP) {
        yl = v2l;
        yr = v2r;
      } else if (TYPE == kFilterBP) {
        yl = k * v1l;
        yr = k * v1r;
      } else {
        yl = xl - k * v1l - v2l;
        yr = xr - k * v1r - v2r;
      }
      g += dg;
      outL[i] += yl * g;
      outR[i] += yr * g;
    }
    st.ic1L = ic1L; st.ic2L = ic2L; st.ic1R = ic1R; st.ic2R = ic2R;
  } else {
    float ic1 = st.ic1L, ic2 = st.ic2L;
    for (int i = 0; i < kQuantum; ++i) {
      const float x = inL[i];
      const float v3 = x - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      float y;
      if (TYPE == kFilterLP) y = v2;
      else if (TYPE == kFilterBP) y = k * v1;
      else y = x - k * v1 - v2;
      g += dg;
      outL[i] += y * g;
    }
    // Keep the right-channel state in step so a later switch to stereo is seamless.
    st.ic1L = st.ic1R = ic1;
    st.ic2L = st.ic2R = ic2;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Param mappings (also used by the tests)
// ---------------------------------------------------------------------------
static inline float cutPitchOf(uint8_t v) { return 30.0f + float(v) * (105.0f / 127.0f); }
float LeadVoice::cutoffHz(uint8_t v) { return 440.0f * powf(2.0f, (cutPitchOf(v) - 69.0f) / 12.0f); }
float LeadVoice::attackSeconds(uint8_t v) { return 0.001f * powf(4000.0f, norm01(v)); }
float LeadVoice::decaySeconds(uint8_t v) { return 0.02f * powf(600.0f, norm01(v)); }
float LeadVoice::releaseSeconds(uint8_t v) { return 0.02f * powf(500.0f, norm01(v)); }
float LeadVoice::fltDecaySeconds(uint8_t v) { return 0.03f * powf(400.0f, norm01(v)); }
float LeadVoice::fltAttackSeconds(uint8_t v) { return v == 0 ? 0.0f : 0.001f * powf(2000.0f, norm01(v)); }
float LeadVoice::glideSeconds(uint8_t v) { return 0.002f * powf(250.0f, norm01(v)); }
float LeadVoice::lfoHz(uint8_t v) { return 0.1f * powf(200.0f, norm01(v)); }
float LeadVoice::detuneCents(uint8_t v) { return 80.0f * norm01(v) * norm01(v); }

int LeadVoice::chordIntervals(uint8_t chordType, int8_t* semis) {
  const ChordDef& c = kChords[chordType < kChordNum ? chordType : 0];
  for (int i = 0; i < c.n; ++i) semis[i] = c.semis[i];
  return c.n;
}

// ---------------------------------------------------------------------------
LeadVoice::LeadVoice() {
  for (int i = 0; i < kSoundParams; ++i) cur_[i] = kInfo[i].def;
  for (int j = 0; j < 5; ++j) superRatio_[j] = 1.0f;
  std::memset(v_, 0, sizeof v_);
  std::memset(&shape1_, 0, sizeof shape1_);
  std::memset(&shape2_, 0, sizeof shape2_);
}

void LeadVoice::init(float sampleRate) {
  fs_ = sampleRate > 8000.0f ? sampleRate : 8000.0f;
  buildTables(fs_);
  quantumRate_ = fs_ / float(kQuantum);
  incK_ = 4294967296.0f * 8.17579892f / fs_;   // phase increment of MIDI pitch 0
  cutK_ = kPi * 8.17579892f / fs_;             // pi * f / fs of MIDI pitch 0
  pitchMax_ = 12.0f * log2f(0.45f * fs_ / 8.17579892f);
  cutPitchMax_ = pitchMax_;
  killQuanta_ = int(0.0025f * quantumRate_ + 0.999f);
  if (killQuanta_ < 2) killQuanta_ = 2;
  smooth_ = 1.0f - expf(-1.0f / (0.005f * quantumRate_));
  std::memset(v_, 0, sizeof v_);
  groupCounter_ = ageCounter_ = lastGroup_ = 0;
  dcC_ = 1.0f - expf(-2.0f * kPi * 8.0f / fs_);
  dcAmt_ = dcAmtPrev_ = dcL_ = dcR_ = 0.0f;
  lfoPhase_ = 0.0f;
  lfo_ = 0.0f;
  for (int i = 0; i < kSoundParams; ++i) {
    cur_[i] = kInfo[i].def;
    applyParam(i);
  }
  paramsSnap_ = true;
  ready_ = true;
  globalQuantum();
}

const ParamInfo* LeadVoice::paramInfo(int index) const {
  return (index >= 0 && index < kNumParams) ? &kInfo[index] : nullptr;
}

void LeadVoice::setParams(const uint8_t* p) {
  for (int i = 0; i < kNumParams; ++i) {
    uint8_t v = p[i];
    if (v < kInfo[i].min) v = kInfo[i].min;
    if (v > kInfo[i].max) v = kInfo[i].max;
    if (v != cur_[i]) {
      cur_[i] = v;
      applyParam(i);
    }
  }
}

// Called only when a value changes (libm is fine here).
void LeadVoice::applyParam(int index) {
  const uint8_t v = cur_[index];
  const float x = norm01(v);
  switch (index) {
    case kPCutoff: cutT_ = cutPitchOf(v); break;
    case kPReso: {
      const float q = 0.6f * powf(20.0f, x);   // 0.6 .. 12
      kT_ = 1.0f / q;
      // Mild level compensation (-6 dB at max): keeps bright resonant patches
      // about as loud as open ones; the bus soft limiter bounds the rare peak
      // of a resonance landing on a strong low harmonic.
      resCompT_ = q > 0.7f ? powf(0.7f / q, 0.25f) : 1.0f;
      break;
    }
    case kPEnvAmt: envSemis_ = 72.0f * x; break;
    case kPFltDecay: fDecCoef_ = expf(-kT60 / (fltDecaySeconds(v) * quantumRate_)); break;
    case kPWave: wave_ = v; break;
    case kPShape: shapeT_ = x; break;
    case kPDetune:
      detSemis_ = detuneCents(v) * 0.01f;
      superDetMax_ = 0.65f * detSemis_;
      for (int j = 0; j < 5; ++j) superRatio_[j] = powf(2.0f, kSuperDet[j] * superDetMax_ / 12.0f);
      break;
    case kPWidth: widthT_ = x; break;
    case kPAttack: {
      const float s = 1.0f / (attackSeconds(v) * quantumRate_);
      atkStep_ = s > 1.0f ? 1.0f : s;
      break;
    }
    case kPDecay: decCoef_ = expf(-kT60 / (decaySeconds(v) * quantumRate_)); break;
    case kPSustain: sus_ = x * x; break;
    case kPRelease: relCoef_ = expf(-kT60 / (releaseSeconds(v) * quantumRate_)); break;
    case kPFltAttack: {
      const float t = fltAttackSeconds(v);
      const float s = t > 0.0f ? 1.0f / (t * quantumRate_) : 1.0f;
      fAtkStep_ = s > 1.0f ? 1.0f : s;
      break;
    }
    case kPFltType: fltType_ = v; break;
    case kPKeyTrack: keyTrack_ = x; break;
    case kPDrive: driveT_ = x; break;
    case kPOsc2Wave: osc2Wave_ = v; break;
    case kPInterval: interval_ = float(int(v) - kIntervalCentre); break;
    case kPOscMix:
      mixAT_ = cosf(x * (0.5f * kPi));
      mixBT_ = sinf(x * (0.5f * kPi));
      if (v == 0) mixBT_ = 0.0f;
      if (v == 127) mixAT_ = 0.0f;
      break;
    case kPLfoRate: lfoInc_ = lfoHz(v) / quantumRate_; break;
    case kPVibrato: vibDepth_ = x * x; break;               // up to +-1 semitone
    case kPLfoShape: lfoShapeDepth_ = x; break;
    case kPLfoCutoff: lfoCutDepth_ = 24.0f * x * x; break;  // up to +-2 octaves
    case kPGlide: glideCoef_ = 1.0f - expf(-1.0f / (glideSeconds(v) * quantumRate_)); break;
    default: break;
  }
}

// Oscillator shape for the three classic waves + sine, from the smoothed SHAPE
// value and the LFO. Cheap: runs twice per quantum for the whole synth.
void LeadVoice::shapeFor(int wave, OscShape& s) const {
  const float t = shapeS_;
  const float ls = lfo_ * lfoShapeDepth_;
  s.gB = 0.0f;
  s.offs = 0;
  s.setB = kSetSaw;
  switch (wave) {
    case kWaveSaw: {  // saw -> saw + half-cycle-shifted saw (octave-up saw at SHAPE max)
      s.setA = kSetSaw;
      if (t < 0.002f) {
        s.kind = kOne;
        s.gA = 1.0f;
      } else {
        const float nrm = rsqrtFast(0.75f * (1.0f - t) * (1.0f - t) + 0.25f * (1.0f + t) * (1.0f + t));
        s.kind = kTwo;
        s.gA = nrm;
        s.gB = nrm * t;
        s.offs = cycles(0.5f + 0.2f * ls);
      }
      break;
    }
    case kWavePulse: {  // width 50% .. 4%, LFO = PWM
      const float w = clampf(0.5f - 0.46f * t + 0.4f * ls, 0.03f, 0.97f);
      const float nrm = 0.58f * (1.0f + 0.9f * (1.0f - 4.0f * w * (1.0f - w)));
      s.kind = kTwo;
      s.setA = kSetSaw;
      s.gA = nrm;
      s.gB = -nrm;
      s.offs = cycles(w);
      break;
    }
    case kWaveTri: {  // triangle -> saw morph
      s.setA = kSetTri;
      if (t < 0.002f) {
        s.kind = kOne;
        s.gA = 1.0f;
      } else {
        const float u = 1.0f - t;
        const float nrm = rsqrtFast(u * u + t * t + 1.5f * u * t);
        s.kind = kTwo;
        s.gA = u * nrm;
        s.gB = t * nrm;
        s.offs = cycles(0.15f * ls);
      }
      break;
    }
    default:  // sine
      s.kind = kOne;
      s.setA = kSetSine;
      s.gA = kFmGain;
      break;
  }
}

// Once per quantum for the whole synth: smoothing, LFO, shared gains.
void LeadVoice::globalQuantum() {
  if (paramsSnap_) {
    cutS_ = cutT_; kS_ = kT_; resCompS_ = resCompT_; shapeS_ = shapeT_; widthS_ = widthT_;
    driveS_ = driveT_; mixAS_ = mixAT_; mixBS_ = mixBT_;
    paramsSnap_ = false;
  } else {
    const float c = smooth_;
    smoothTo(cutS_, cutT_, c);
    smoothTo(kS_, kT_, c);
    smoothTo(resCompS_, resCompT_, c);
    smoothTo(shapeS_, shapeT_, c);
    smoothTo(widthS_, widthT_, c);
    smoothTo(driveS_, driveT_, c);
    smoothTo(mixAS_, mixAT_, c);
    smoothTo(mixBS_, mixBT_, c);
  }

  // LFO: parabolic sine, free running.
  lfoPhase_ += lfoInc_;
  if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;
  const float lx = 2.0f * lfoPhase_ - 1.0f;
  lfo_ = 4.0f * lx * (1.0f - (lx < 0.0f ? -lx : lx));
  vibSemis_ = lfo_ * vibDepth_;
  lfoCutSemis_ = lfo_ * lfoCutDepth_;
  const float ls = lfo_ * lfoShapeDepth_;

  shapeFor(wave_, shape1_);
  shapeFor(osc2Wave_ == kOsc2Sine ? 99 : int(osc2Wave_), shape2_);

  // Stereo layout. bal = 1 when both oscillators are equally loud, 0 when only one sounds.
  const float w = widthS_;
  const float bal = 4.0f * mixAS_ * mixAS_ * mixBS_ * mixBS_;
  if (wave_ <= kWaveTri) {
    oscSpread_ = 0.7f * w * bal;                  // osc1 / osc2 either side of the voice
    panScale_ = w * (0.3f + 0.4f * (1.0f - bal)); // single-osc patches lean on voice panning
    superSpread_ = 0.0f;
  } else if (wave_ == kWaveSuper) {
    oscSpread_ = 0.0f;
    panScale_ = 0.3f * w;
    superSpread_ = 0.7f * w;
  } else {
    oscSpread_ = 0.0f;
    panScale_ = 0.7f * w;
    superSpread_ = 0.0f;
  }
  // Mono output (forced, or WIDTH exactly 0): no panning at all, and the voices
  // run the single-filter loops. Because the pan law is linear, this is the
  // same signal as (L+R)/2 of the stereo path at ANY width.
  monoNow_ = forceMono_ || (widthS_ == 0.0f && widthT_ == 0.0f);
  if (monoNow_) oscSpread_ = panScale_ = superSpread_ = 0.0f;

  // Super saw: SHAPE moves energy from the centre saw to the four side saws.
  {
    const float c = 1.0f - 0.45f * shapeS_;
    const float sd = 0.35f + 0.65f * shapeS_;
    const float n = rsqrtFast(c * c + 4.0f * sd * sd);
    superGC_ = c * n;
    superGS_ = sd * n;
  }
  // FM: modulation index in radians at full envelope / velocity.
  {
    const float iMax = wave_ == kWaveFm1 ? 7.0f : (wave_ == kWaveFm2 ? 5.0f : 4.5f);
    const float t = shapeS_;
    fmIdxBase_ = iMax * t * (0.3f + 0.7f * t) * (1.0f + 0.6f * ls);
  }
  orgPos_ = clampf(shapeS_ * float(kOrgRegs - 1) + ls, 0.0f, float(kOrgRegs - 1));

  // Output stage: clean below limT_, parabolic knee up to exactly 1.0. DRIVE
  // pushes the bus into the knee and lowers it.
  {
    const float d = driveS_;
    limPre_ = exp2Fast(3.0f * d);
    limT_ = 0.6f - 0.5f * d;
    limQ_ = 0.25f / (1.0f - limT_);
    limMax_ = 2.0f - limT_;
    limPost_ = 1.0f / (1.0f + 1.2f * d);
    // A symmetric clipper still makes DC from asymmetric waves (organ drawbar
    // phases) and slow intermodulation from detuned stacks: block it.
    // Slew-limited (full swing in ~20 quanta) and ramped per sample in render().
    dcAmtPrev_ = dcAmt_;
    dcAmt_ += clampf(clampf(10.0f * d, 0.0f, 1.0f) - dcAmt_, -0.05f, 0.05f);
  }
  gainK_ = kLevel * resCompS_ * (1.0f / kTabAmp);
}

// ---------------------------------------------------------------------------
// Voice management
// ---------------------------------------------------------------------------
void LeadVoice::setPan(V& v, uint8_t chordIdx, uint8_t chordSize, uint8_t slot) {
  switch (chordSize) {
    case 2: v.pan = kPan2[chordIdx & 1]; v.flip = (chordIdx & 1) ? -1.0f : 1.0f; break;
    case 3: v.pan = kPan3[chordIdx % 3]; v.flip = (chordIdx & 1) ? -1.0f : 1.0f; break;
    case 4: v.pan = kPan4[chordIdx & 3]; v.flip = (chordIdx & 1) ? -1.0f : 1.0f; break;
    default:  // single notes: alternate which side osc1 takes, so lines don't lean
      v.pan = kPan1[slot & 3];
      v.flip = ((slot + ageCounter_) & 1) ? -1.0f : 1.0f;
      break;
  }
}

void LeadVoice::startVoice(V& v, const NoteStart& n) {
  v.pending = false;
  v.fresh = true;
  v.note = n.note;
  v.root = n.root;
  v.chordIdx = n.chordIdx;
  v.chordSize = n.chordSize;
  v.panSlot = n.panSlot;
  v.group = n.group;
  v.age = ageCounter_;
  v.pitch = v.target = float(n.note);
  const float vel = norm01(n.vel);
  const float a = 0.25f + 0.75f * vel;
  v.velAmp = a * a * (n.accent ? 1.4f : 1.0f) * kSizeGain[(n.chordSize - 1) & 3];
  v.envMul = (0.55f + 0.45f * vel) * (n.accent ? 1.25f : 1.0f);
  v.envAdd = n.accent ? 8.0f : 0.0f;
  v.modVel = (0.5f + 0.5f * vel) * (n.accent ? 1.3f : 1.0f);
  v.stage = kAtk;
  v.gate = !n.gateOff;
  v.amp = 0.0f;
  v.gain = 0.0f;
  v.fenv = 0.0f;
  v.fstage = 0;
  // Deterministic, well-spread start phases: every stab sounds the same.
  for (int j = 0; j < 5; ++j) v.ph1[j] = cycles(0.137f + 0.61803399f * float((n.chordIdx * 5 + j) % 13));
  if (wave_ >= kWaveFm1 && wave_ <= kWaveBell) v.ph1[0] = v.ph1[1] = 0;   // sine-phase PM: no DC, clean onset
  {
    // beat frequency between osc1 and osc2 for this note (Hz), valid when INTV is 0
    const float beat = 8.17579892f * exp2Fast(float(n.note) * (1.0f / 12.0f)) * detSemis_ * 0.057762f;
    const float lag = clampf(beat * kOsc2InPhaseAt[n.chordIdx & 3], 0.0f, 0.2f);
    v.ph2[0] = v.ph1[0] - cycles(lag);
  }
  v.ph2[1] = v.ph2[0];
  v.two1 = v.two2 = 0;
  v.ic1L = v.ic2L = v.ic1R = v.ic2R = 0.0f;
  setPan(v, n.chordIdx, n.chordSize, n.panSlot);
}

void LeadVoice::queueNote(int vi, const NoteStart& n) {
  V& v = v_[vi];
  if (v.stage == kOff || (v.amp <= 0.0f && v.gain <= 0.0f)) {
    startVoice(v, n);
    return;
  }
  v.pend = n;
  v.pending = true;
  v.gate = false;
  if (v.stage != kKill) {
    v.stage = kKill;
    v.killLeft = killQuanta_;
    v.killStep = v.amp / float(killQuanta_);
  }
}

// Pick the voice a new note should use. Preference: a voice already playing
// this note, an idle voice, a voice that is fading out anyway, the quietest
// released voice, and only then the oldest held voice.
int LeadVoice::allocVoice(uint8_t note, uint32_t claimedMask) const {
  int best = 0;
  int bestClass = 1000;
  float bestVal = 0.0f;
  for (int i = 0; i < kLeadVoices; ++i) {
    if (claimedMask & (1u << i)) continue;
    const V& v = v_[i];
    int cls;
    float val = 0.0f;
    const bool sounding = v.stage != kOff;
    const uint8_t curNote = v.pending ? v.pend.note : v.note;
    if (sounding && curNote == note) {
      cls = 0;
    } else if (!sounding) {
      cls = 1;
    } else if (v.stage == kKill && !v.pending) {
      cls = 2;
    } else if (v.stage == kRel) {
      cls = 3;
      val = v.amp * v.velAmp;
    } else {
      cls = 4;
      val = float(v.pending ? ageCounter_ : v.age);   // oldest first; a queued note is the newest
    }
    if (cls < bestClass || (cls == bestClass && val < bestVal)) {
      best = i;
      bestClass = cls;
      bestVal = val;
    }
  }
  return best;
}

void LeadVoice::releaseVoice(V& v) {
  v.gate = false;
  if (v.stage == kAtk || v.stage == kDec) v.stage = kRel;
  if (v.fstage == 0) v.fstage = 1;
}

void LeadVoice::noteOn(const NoteEvent& e) {
  if (!ready_) return;
  // Nothing sounding: jump straight to the current params instead of gliding
  // there (a p-locked step must start with its own sound, not a 5 ms sweep
  // from the previous one). Smoothing only matters under sounding voices.
  if (!active()) {
    paramsSnap_ = true;
    dcL_ = dcR_ = 0.0f;
  }
  int8_t iv[kMaxChordNotes];
  const int n = chordIntervals(e.chord, iv);
  const uint8_t vel = e.vel < 1 ? 1 : (e.vel > 127 ? 127 : e.vel);
  const uint32_t group = ++groupCounter_;
  ++ageCounter_;

  // Legato: reuse the voices of the most recent note/chord without retriggering.
  int legatoVoice[kMaxChordNotes] = {-1, -1, -1, -1};
  bool legato = false;
  if (e.slide) {
    for (int i = 0; i < kLeadVoices; ++i) {
      const V& v = v_[i];
      if (v.pending) {
        if (v.pend.group == lastGroup_) { legatoVoice[v.pend.chordIdx & 3] = i; legato = true; }
      } else if (v.stage != kOff && v.stage != kKill && v.group == lastGroup_) {
        legatoVoice[v.chordIdx & 3] = i;
        legato = true;
      }
    }
  }

  // Pan slot for single notes: the first one no held single note is using.
  uint8_t slot = 0;
  if (n == 1) {
    uint32_t used = 0;
    for (int i = 0; i < kLeadVoices; ++i) {
      const V& v = v_[i];
      if (v.pending) {
        if (v.pend.chordSize == 1) used |= 1u << v.pend.panSlot;
      } else if (v.gate && v.stage != kOff && v.chordSize == 1) {
        used |= 1u << v.panSlot;
      }
    }
    while (slot < 3 && (used & (1u << slot))) ++slot;
  }

  uint32_t claimed = 0;
  if (legato)
    for (int i = 0; i < kMaxChordNotes; ++i)
      if (legatoVoice[i] >= 0) claimed |= 1u << legatoVoice[i];

  for (int i = 0; i < n; ++i) {
    int nn = int(e.note) + iv[i];
    const uint8_t note = uint8_t(nn < 0 ? 0 : (nn > 127 ? 127 : nn));
    if (legato && legatoVoice[i] >= 0) {
      V& v = v_[legatoVoice[i]];
      if (v.pending) {
        v.pend.note = note;
        v.pend.root = e.note;
        v.pend.group = group;
        v.pend.gateOff = false;
      } else {
        v.target = float(note);
        v.note = note;
        v.root = e.note;
        v.group = group;
        v.age = ageCounter_;
        if (!v.gate) {   // gate had already ended: glide, but this is audibly a new note
          v.gate = true;
          v.stage = kAtk;
          v.fstage = 0;
          v.fenv = 0.0f;
        }
      }
      continue;
    }
    NoteStart ns;
    ns.note = note;
    ns.root = e.note;
    ns.vel = vel;
    ns.chordIdx = uint8_t(i);
    ns.chordSize = uint8_t(n);
    ns.panSlot = slot;
    ns.accent = e.accent;
    ns.gateOff = false;
    ns.group = group;
    const int vi = allocVoice(note, claimed);
    claimed |= 1u << vi;
    queueNote(vi, ns);
  }
  if (legato) {
    for (int i = n; i < kMaxChordNotes; ++i) {
      if (legatoVoice[i] < 0) continue;
      V& v = v_[legatoVoice[i]];
      if (v.pending) v.pending = false; else releaseVoice(v);
    }
  }
  lastGroup_ = group;
}

void LeadVoice::noteOff(uint8_t note) {
  for (int i = 0; i < kLeadVoices; ++i) {
    V& v = v_[i];
    if (v.pending) {
      if (v.pend.root == note) v.pend.gateOff = true;
    } else if (v.gate && v.stage != kOff && v.root == note) {
      releaseVoice(v);
    }
  }
}

void LeadVoice::choke() {
  for (int i = 0; i < kLeadVoices; ++i) {
    V& v = v_[i];
    v.pending = false;
    v.gate = false;
    if (v.stage != kOff && v.stage != kKill) {
      v.stage = kKill;
      v.killLeft = killQuanta_;
      v.killStep = v.amp / float(killQuanta_);
    }
  }
}

bool LeadVoice::active() const {
  for (int i = 0; i < kLeadVoices; ++i)
    if (v_[i].stage != kOff) return true;
  // Let the DC blocker's tail settle (only non-zero with DRIVE up).
  const float t = dcAmt_ * (fabsf(dcL_) + fabsf(dcR_));
  return t > 1.0e-5f;
}

void LeadVoice::setMono(bool mono) { forceMono_ = mono; }

int LeadVoice::soundingVoices() const {
  int n = 0;
  for (int i = 0; i < kLeadVoices; ++i)
    if (v_[i].stage != kOff) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
template <bool ST>
void LeadVoice::renderVoiceT(V& v, float* outL, float* outR) {
  const Tables& T = tables();

  // ---- pitch (glide only ever has work to do after a legato retarget)
  v.pitch += (v.target - v.pitch) * glideCoef_;
  const float pb = v.pitch + vibSemis_;

  // ---- amp envelope: one step per quantum, linear ramp per sample
  bool ending = false;
  switch (v.stage) {
    case kAtk:
      v.amp += atkStep_;
      if (v.amp >= 1.0f) {
        v.amp = 1.0f;
        v.stage = v.gate ? uint8_t(kDec) : uint8_t(kRel);
      }
      break;
    case kDec:
      v.amp = sus_ + (v.amp - sus_) * decCoef_;
      if (v.amp < kSilence) { v.amp = 0.0f; ending = true; }
      break;
    case kRel:
      v.amp *= relCoef_;
      if (v.amp < kSilence) { v.amp = 0.0f; ending = true; }
      break;
    default:  // kKill
      v.amp -= v.killStep;
      if (--v.killLeft <= 0 || v.amp <= 0.0f) { v.amp = 0.0f; ending = true; }
      break;
  }
  const float g0 = v.gain;
  const float g1 = v.amp * v.velAmp * gainK_;
  const float dg = (g1 - g0) * kInvQ;
  v.gain = g1;

  // ---- filter envelope (attack -> decay to zero)
  if (v.fstage == 0) {
    v.fenv += fAtkStep_;
    if (v.fenv >= 1.0f) { v.fenv = 1.0f; v.fstage = 1; }
  } else {
    v.fenv *= fDecCoef_;
    if (v.fenv < 1.0e-5f) v.fenv = 0.0f;   // -100 dB: stop before the float goes denormal
  }

  // ---- filter coefficients: g = tan(x) by Pade (3,2), one divide in total
  const float k = kS_;
  float a1, a2, a3;
  {
    float cp = cutS_ + keyTrack_ * (v.pitch - 60.0f) + (envSemis_ * v.envMul + v.envAdd) * v.fenv + lfoCutSemis_;
    cp = clampf(cp, 0.0f, cutPitchMax_);
    const float x = cutK_ * exp2Fast(cp * (1.0f / 12.0f));
    const float x2 = x * x;
    const float num = x * (15.0f - x2);
    const float den = 15.0f - 6.0f * x2;
    const float inv = 1.0f / (den * den + num * num + k * num * den);
    a1 = den * den * inv;
    a2 = num * den * inv;
    a3 = num * num * inv;
  }

  // ---- oscillators into the pre-filter stereo buffer
  float bL[kQuantum], bR[kQuantum];
  for (int i = 0; i < kQuantum; ++i) bL[i] = 0.0f;
  if (ST)
    for (int i = 0; i < kQuantum; ++i) bR[i] = 0.0f;

  // Detuned copies of a LOW note are not split left/right: that makes
  // anti-phase lows on headphones. No detune spread at MIDI <= 48, full from
  // MIDI 64. (Whole-note panning stays: it never puts anything in anti-phase.)
  // Linear pan law: the mono sum does not change.
  const float ws = clampf((v.pitch - 48.0f) * (1.0f / 16.0f), 0.0f, 1.0f);
  const float pv = v.pan * panScale_;
  const float oscSpread = oscSpread_ * ws;
  const float mixA = mixAS_, mixB = mixBS_;
  const bool classic = wave_ <= kWaveTri;
  const float det = detSemis_;

  if (mixA > 1.0e-4f) {
    if (classic) {
      const float p = clampf(pb - 0.5f * det, 0.0f, pitchMax_);
      const uint32_t inc = toInc(incK_ * exp2Fast(p * (1.0f / 12.0f)));
      const int band = bandForPitch(p);
      const float pan = clampf(pv - oscSpread * v.flip, -1.0f, 1.0f);
      const float gL = mixA * (1.0f - pan), gR = mixA * (1.0f + pan);
      const OscShape& s = shape1_;
      const Tab& ta = s.setA == kSetSaw ? T.saw[band] : T.tri[band];
      if (s.kind == kOne) {
        oscOne<ST>(v.ph1[0], inc, ta, s.gA * gL, s.gA * gR, bL, bR);
        v.two1 = 0;
      } else {
        if (!v.two1) v.ph1[1] = v.ph1[0] + s.offs;
        v.two1 = 1;
        const int32_t err = int32_t(v.ph1[0] + s.offs - v.ph1[1]);
        const uint32_t incB = inc + uint32_t(err / kQuantum);
        oscTwo<ST>(v.ph1[0], v.ph1[1], inc, incB, ta, T.saw[band], s.gA * gL, s.gA * gR, s.gB * gL, s.gB * gR, bL, bR);
      }
    } else if (wave_ == kWaveSuper) {
      const float p = clampf(pb, 0.0f, pitchMax_ - 1.0f);
      const float base = incK_ * exp2Fast(p * (1.0f / 12.0f));
      uint32_t inc[5];
      for (int j = 0; j < 5; ++j) inc[j] = toInc(base * superRatio_[j]);
      const int band = bandForPitch(p + superDetMax_);
      const float sp = superSpread_ * ws;
      const float pl = clampf(pv - sp, -1.0f, 1.0f), pr = clampf(pv + sp, -1.0f, 1.0f);
      const float gc = superGC_ * mixA, gs = superGS_ * mixA;
      oscSuper<ST>(v.ph1, inc, T.saw[band], gc * (1.0f - pv), gc * (1.0f + pv), gs * (1.0f - pl), gs * (1.0f + pl),
               gs * (1.0f - pr), gs * (1.0f + pr), bL, bR);
    } else if (wave_ == kWaveOrgan) {
      const float p = clampf(pb, 0.0f, pitchMax_);
      const uint32_t inc = toInc(incK_ * exp2Fast(p * (1.0f / 12.0f))) >> 1;
      const int lvl = p < T.organPitch1 ? 0 : (p < T.organPitch2 ? 1 : 2);
      const float percT = kOrganPerc * v.fenv * v.modVel;
      if (v.fresh) { v.orgPos = orgPos_; v.orgPerc = percT; }
      int reg = int(v.orgPos);
      if (reg > kOrgRegs - 2) reg = kOrgRegs - 2;
      const float x0 = v.orgPos - float(reg), x1 = orgPos_ - float(reg);
      const float g = kOrganGain * mixA;
      oscOrgan<ST>(v.ph1[0], inc, T.organ[reg][lvl], T.organ[reg + 1][lvl], T.sine, x0, (x1 - x0) * kInvQ, v.orgPerc,
               (percT - v.orgPerc) * kInvQ, g * (1.0f - pv), g * (1.0f + pv), bL, bR);
      v.orgPos = orgPos_;
      v.orgPerc = percT;
    } else {  // FM
      const float p = clampf(pb, 0.0f, pitchMax_);
      const uint32_t cinc = toInc(incK_ * exp2Fast(p * (1.0f / 12.0f)));
      uint32_t minc;
      float ratio;
      if (wave_ == kWaveFm1) { minc = cinc; ratio = 1.0f; }
      else if (wave_ == kWaveFm2) { minc = cinc * 2u; ratio = 2.0f; }
      else { minc = (cinc >> 1) * 7u; ratio = 3.5f; }
      // Index: SHAPE x envelope x velocity, a little less up the keyboard, and
      // capped so that the first sideband order landing above Nyquist (order n)
      // stays below about -50 dB: J_n(I) ~ (I/2)^n / n!  =>  I < 0.55 (n - 0.5) - 1.3.
      float idx = fmIdxBase_ * (0.25f + 0.75f * v.fenv) * v.modVel * clampf(1.0f - (p - 60.0f) * 0.012f, 0.35f, 1.5f);
      const float nAlias = (2147483648.0f / float(cinc) - 1.0f) / ratio + 1.0f;
      const float lim = 0.55f * nAlias - 1.575f;
      if (idx > lim) idx = lim;
      if (idx < 0.0f) idx = 0.0f;
      const float idxT = idx * (16777216.0f / (2.0f * kPi * kTabAmp));
      if (v.fresh) v.fmIdx = idxT;
      const float g = kFmGain * mixA;
      oscFm<ST>(v.ph1[0], v.ph1[1], cinc, minc, T.sine, v.fmIdx, (idxT - v.fmIdx) * kInvQ, g * (1.0f - pv),
            g * (1.0f + pv), bL, bR);
      v.fmIdx = idxT;
    }
  }

  if (mixB > 1.0e-4f) {
    const float p = clampf(pb + interval_ + (classic ? 0.5f * det : det), 0.0f, pitchMax_);
    const uint32_t inc = toInc(incK_ * exp2Fast(p * (1.0f / 12.0f)));
    const int band = bandForPitch(p);
    const float pan = clampf(pv + oscSpread * v.flip, -1.0f, 1.0f);
    const float gL = mixB * (1.0f - pan), gR = mixB * (1.0f + pan);
    const OscShape& s = shape2_;
    const Tab& ta = s.setA == kSetSaw ? T.saw[band] : (s.setA == kSetTri ? T.tri[band] : T.sine);
    if (s.kind == kOne) {
      oscOne<ST>(v.ph2[0], inc, ta, s.gA * gL, s.gA * gR, bL, bR);
      v.two2 = 0;
    } else {
      if (!v.two2) v.ph2[1] = v.ph2[0] + s.offs;
      v.two2 = 1;
      const int32_t err = int32_t(v.ph2[0] + s.offs - v.ph2[1]);
      const uint32_t incB = inc + uint32_t(err / kQuantum);
      oscTwo<ST>(v.ph2[0], v.ph2[1], inc, incB, ta, T.saw[band], s.gA * gL, s.gA * gR, s.gB * gL, s.gB * gR, bL, bR);
    }
  }
  v.fresh = false;

  // ---- filter + amp
  FilterState st = {v.ic1L, v.ic2L, v.ic1R, v.ic2R};
  if (fltType_ == kFilterLP) filterAmp<kFilterLP, ST>(st, bL, bR, a1, a2, a3, k, g0, dg, outL, outR);
  else if (fltType_ == kFilterBP) filterAmp<kFilterBP, ST>(st, bL, bR, a1, a2, a3, k, g0, dg, outL, outR);
  else filterAmp<kFilterHP, ST>(st, bL, bR, a1, a2, a3, k, g0, dg, outL, outR);
  v.ic1L = st.ic1L; v.ic2L = st.ic2L; v.ic1R = st.ic1R; v.ic2R = st.ic2R;

  if (ending) {
    v.stage = kOff;
    v.gate = false;
    v.gain = 0.0f;
    if (v.pending) startVoice(v, v.pend);
  }
}

void LeadVoice::render(float* outL, float* outR, int frames) {
  int done = 0;
  while (frames - done >= kQuantum) {
    float* L = outL + done;
    float* R = outR + done;
    for (int i = 0; i < kQuantum; ++i) L[i] = R[i] = 0.0f;
    if (ready_) {
      globalQuantum();
      const bool mono = monoNow_;
      for (int vi = 0; vi < kLeadVoices; ++vi) {
        if (v_[vi].stage == kOff) continue;
        if (mono) renderVoiceT<false>(v_[vi], L, R);
        else renderVoiceT<true>(v_[vi], L, R);
      }

      // Drive / soft limiter: linear up to limT_, parabolic knee, never above
      // 1.0; then the drive-dependent DC blocker.
      const float pre = limPre_, post = limPost_, t = limT_, q = limQ_, mx = limMax_;
      const float dc = dcC_, dd = (dcAmt_ - dcAmtPrev_) * kInvQ;
      float da = dcAmtPrev_;
      if (mono) {
        float zl = dcL_;
        for (int i = 0; i < kQuantum; ++i) {
          float l = L[i] * pre;
          if (l > t) { l = l < mx ? l : mx; const float d = l - t; l = t + d - d * d * q; }
          else if (l < -t) { l = l > -mx ? l : -mx; const float d = -l - t; l = -(t + d - d * d * q); }
          l *= post;
          zl += dc * (l - zl);
          da += dd;
          L[i] = R[i] = l - da * zl;
        }
        dcL_ = dcR_ = zl;
      } else {
        float zl = dcL_, zr = dcR_;
        for (int i = 0; i < kQuantum; ++i) {
          float l = L[i] * pre, r = R[i] * pre;
          if (l > t) { l = l < mx ? l : mx; const float d = l - t; l = t + d - d * d * q; }
          else if (l < -t) { l = l > -mx ? l : -mx; const float d = -l - t; l = -(t + d - d * d * q); }
          if (r > t) { r = r < mx ? r : mx; const float d = r - t; r = t + d - d * d * q; }
          else if (r < -t) { r = r > -mx ? r : -mx; const float d = -r - t; r = -(t + d - d * d * q); }
          l *= post;
          r *= post;
          zl += dc * (l - zl);
          zr += dc * (r - zr);
          da += dd;
          L[i] = l - da * zl;
          R[i] = r - da * zr;
        }
        dcL_ = zl;
        dcR_ = zr;
      }
      // Denormal guard for the blocker state once everything is silent.
      if (fabsf(dcL_) < 1.0e-20f) dcL_ = 0.0f;
      if (fabsf(dcR_) < 1.0e-20f) dcR_ = 0.0f;
    }
    done += kQuantum;
  }
  for (; done < frames; ++done) outL[done] = outR[done] = 0.0f;
}

}  // namespace lead
}  // namespace pg
