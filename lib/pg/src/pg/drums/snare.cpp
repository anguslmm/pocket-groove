// Snare: two tuned sine modes with a shared pitch bend, plus white noise
// through a resonant high-pass (snares). The noise has a slow envelope (the
// rattle) and a fast one (the crack). Everything is summed into a cubic
// saturator. Covers 909-style snares through short, pitched, rimmy ones.
#include "drums.h"

namespace pg {
namespace drums {

namespace {
const ParamInfo kParams[SnareVoice::kNumParams] = {
    {"TUNE", "Tune",       0, 127, 60, ParamFormat::Number, nullptr},  // body 110..330 Hz
    {"DCY",  "Decay",      0, 127, 56, ParamFormat::Number, nullptr},  // noise tau 15..400 ms
    {"SNAP", "Snappy",     0, 127, 80, ParamFormat::Number, nullptr},  // noise vs body
    {"TONE", "Tone",       0, 127, 70, ParamFormat::Number, nullptr},  // noise HP 700 Hz..9 kHz
    {"BODY", "Body Decay", 0, 127, 52, ParamFormat::Number, nullptr},  // tone tau 12..250 ms
    {"BEND", "Pitch Bend", 0, 127, 60, ParamFormat::Number, nullptr},  // downward sweep depth
    {"RING", "Ring",       0, 127, 64, ParamFormat::Number, nullptr},  // level of upper mode
    {"DTUN", "Detune",     0, 127, 50, ParamFormat::Number, nullptr},  // upper mode ratio 1.2..2.6
    {"DRV",  "Drive",      0, 127, 26, ParamFormat::Number, nullptr},
};
constexpr float kLevel = 0.36f;
constexpr float kNominalPeak = 0.8f;  // of body + noise before the saturator
inline uint32_t bit(int i) { return 1u << i; }
}  // namespace

SnareVoice::SnareVoice() : DrumVoice(kParams, kNumParams) {}

void SnareVoice::resetState() {
  x1_ = x2_ = 0.0f; y1_ = y2_ = 1.0f;
  pe_ = a1_ = a2_ = an_ = af_ = 0.0f;
  svf_.reset();
  noteRatio_ = 1.0f;
  bright_ = 1.0f;
  level_ = 0.0f;
}

void SnareVoice::setPitch() {
  const float f1 = clampf(tuneHz_ * noteRatio_, 40.0f, 2000.0f);
  eps1_ = oscEps(f1, fs_);
  eps2_ = oscEps(f1 * ratio_, fs_);
}

void SnareVoice::setFilter() {
  const float fc = clampf(toneHz_ * bright_, 300.0f, 14000.0f), q = 0.8f;
  svf_.set(fc, q, fs_);
  // Normalise the band-passed noise to a constant RMS (see clap.cpp).
  noiseNorm_ = 0.3f / sqrtf((1.0f / 3.0f) * kPi * q * fc / fs_);
}

void SnareVoice::updateCoefs(uint32_t ch) {
  const float fs = fs_;
  if (ch & bit(kTune)) tuneHz_ = expMap(unit(p_[kTune]), 110.0f, 330.0f);
  if (ch & bit(kDetune)) ratio_ = 1.2f + 1.4f * unit(p_[kDetune]);
  if (ch & (bit(kTune) | bit(kDetune))) setPitch();
  if (ch & bit(kDecay)) {
    const float tau = expMap(unit(p_[kDecay]), 0.015f, 0.4f);
    cn_ = decayCoef(tau, fs);
    cf_ = decayCoef(0.004f + 0.08f * tau, fs);
  }
  if (ch & bit(kBody)) {
    const float tau = expMap(unit(p_[kBody]), 0.012f, 0.25f);
    c1_ = decayCoef(tau, fs);
    c2_ = decayCoef(0.7f * tau, fs);
    pitchCoef_ = decayCoef(clampf(0.25f * tau, 0.004f, 0.03f), fs);
  }
  if (ch & bit(kBend)) {
    const float t = unit(p_[kBend]);
    bend_ = 2.0f * t * t;
  }
  if (ch & (bit(kSnap) | bit(kRing))) {
    const float s = unit(p_[kSnap]), r = unit(p_[kRing]);
    bodyAmt_ = 0.45f - 0.25f * s;
    ringAmt_ = bodyAmt_ * 1.1f * r;
    noiseAmt_ = 0.1f + 0.68f * s;
    snapAmt_ = 0.6f * s;
  }
  if (ch & bit(kTone)) {
    toneHz_ = expMap(unit(p_[kTone]), 700.0f, 9000.0f);
    setFilter();
  }
  if (ch & bit(kDrive)) {
    drive_ = 0.6f * powf(2.0f, 4.5f * unit(p_[kDrive]));
    post_ = driveNorm(drive_, kNominalPeak);
  }
}

void SnareVoice::noteOn(const NoteEvent& e) {
  level_ = kLevel * velocityGain(e.vel) * (e.accent ? 1.3f : 1.0f);
  beginHit(true, level_);
  noteRatio_ = noteRatio(int(e.note), 24);
  setPitch();
  const float b = e.accent ? 1.3f : 1.0f;
  if (b != bright_) {
    bright_ = b;
    setFilter();
  }
  x1_ = x2_ = 0.0f; y1_ = y2_ = 1.0f;
  pe_ = 1.0f;
  a1_ = bodyAmt_;
  a2_ = ringAmt_;
  an_ = noiseAmt_ * (e.accent ? 1.15f : 1.0f);
  af_ = snapAmt_ * (e.accent ? 1.6f : 1.0f);
  svf_.reset();
}

void SnareVoice::render(float* outL, float* outR, int frames) {
  float x1 = x1_, y1 = y1_, x2 = x2_, y2 = y2_, pe = pe_;
  float a1 = a1_, a2 = a2_, an = an_, af = af_;
  float s1 = svf_.s1, s2 = svf_.s2;
  const float eps1 = eps1_, eps2 = eps2_, bend = bend_, pc = pitchCoef_;
  const float c1 = c1_, c2 = c2_, cn = cn_, cf = cf_;
  const float fg = svf_.g, fgk = svf_.gk, fd = svf_.d;
  const float g = drive_, post = post_ * level_, nn = noiseNorm_;
  uint32_t ns = noise_.s;
  for (int i = 0; i < frames; ++i) {
    const float pm = 1.0f + bend * pe;
    pe *= pc;
    const float e1 = eps1 * pm, e2 = eps2 * pm;
    x1 += e1 * y1;
    y1 -= e1 * x1;
    x2 += e2 * y2;
    y2 -= e2 * x2;
    const float body = x1 * a1 + x2 * a2;
    a1 *= c1;
    a2 *= c2;
    ns = ns * 1664525u + 1013904223u;
    const float n = float(int32_t(ns)) * (1.0f / 2147483648.0f);
    float hp, bp, lp;
    svfTick(n, fg, fgk, fd, s1, s2, hp, bp, lp);
    (void)lp;
    const float nz = (bp * nn + 0.2f * hp) * (an + af);
    an *= cn;
    af *= cf;
    outL[i] = softClip((body + nz) * g) * post;
  }
  noise_.s = ns;
  x1_ = x1; y1_ = y1; x2_ = x2; y2_ = y2;
  pe_ = pe < 1.0e-9f ? 0.0f : pe;
  a1_ = a1 < 1.0e-9f ? 0.0f : a1;  // flush: the body can die long before the noise
  a2_ = a2 < 1.0e-9f ? 0.0f : a2;
  an_ = an < 1.0e-9f ? 0.0f : an;
  af_ = af < 1.0e-9f ? 0.0f : af;
  svf_.s1 = s1; svf_.s2 = s2;
  float est = 1.5f * g * (a1 + a2 + an + af);
  if (est > 1.0f) est = 1.0f;
  endBlock(outL, outR, frames, est * post > kSilence);
}

}  // namespace drums
}  // namespace pg
