// Hi-hat (closed and open): six free-running square oscillators at the
// inharmonic TR-808 ratios, summed and differentiated in the integer domain
// (cheap, and it flattens the spectrum so the top end is dense), mixed with
// white noise, then a resonant 12 dB/oct high-pass. Envelope is a slow
// exponential plus a short "tick" so even long hats start crisply.
// The oscillators are never reset: like the analogue original, every hit
// catches the bank at a different phase.
#include "drums.h"

namespace pg {
namespace drums {

namespace {
const ParamInfo kClosedParams[HatVoice::kNumParams] = {
    {"TUNE", "Tune",    0, 127, 64, ParamFormat::Number, nullptr},  // metal pitch
    {"DCY",  "Decay",   0, 127, 58, ParamFormat::Number, nullptr},  // tau 4..120 ms
    {"TONE", "Tone",    0, 127, 80, ParamFormat::Number, nullptr},  // high-pass 3..12 kHz
    {"MTL",  "Metal",   0, 127, 84, ParamFormat::Number, nullptr},  // noise .. metal
    {"COLR", "Colour",  0, 127, 56, ParamFormat::Number, nullptr},  // filter resonance / ring
    {"SNAP", "Snap",    0, 127, 40, ParamFormat::Number, nullptr},  // attack tick
    {"SPRD", "Spread",  0, 127, 64, ParamFormat::Number, nullptr},  // partial spacing
};
const ParamInfo kOpenParams[HatVoice::kNumParams] = {
    {"TUNE", "Tune",    0, 127, 64, ParamFormat::Number, nullptr},
    {"DCY",  "Decay",   0, 127, 52, ParamFormat::Number, nullptr},  // tau 40 ms..1 s
    {"TONE", "Tone",    0, 127, 72, ParamFormat::Number, nullptr},
    {"MTL",  "Metal",   0, 127, 84, ParamFormat::Number, nullptr},
    {"COLR", "Colour",  0, 127, 56, ParamFormat::Number, nullptr},
    {"SNAP", "Snap",    0, 127, 32, ParamFormat::Number, nullptr},
    {"SPRD", "Spread",  0, 127, 64, ParamFormat::Number, nullptr},
};
constexpr float kClosedLevel = 0.15f;
constexpr float kOpenLevel = 0.13f;
// TR-808 cymbal oscillator bank, relative to its lowest (205.3 Hz).
const float kRatio[6] = {1.0f, 1.4827f, 1.8003f, 2.5460f, 2.6303f, 3.8967f};
inline uint32_t bit(int i) { return 1u << i; }
}  // namespace

HatVoice::HatVoice(bool open)
    : DrumVoice(open ? kOpenParams : kClosedParams, kNumParams), open_(open) {
  // Spread the starting phases so the bank is not correlated at power-up.
  for (int i = 0; i < 6; ++i) ph_[i] = 0x9E3779B9u * uint32_t(i + 1);
  noise_.s = open ? 0x7A3C59E1u : 0x13579BDFu;
}

void HatVoice::resetState() {
  es_ = ef_ = 0.0f;
  prev_ = 0;
  for (int i = 0; i < 6; ++i) prev_ += int32_t(ph_[i]) >> 31;  // so the first edge is not a big step
  tilt_ = 0.0f;
  svf_.reset();
  noteRatio_ = 1.0f;
  bright_ = 1.0f;
}

void HatVoice::setPitch() {
  const float base = 205.3f * powf(2.0f, -0.6f + 2.2f * centred(p_[kTune])) * noteRatio_;
  const float spread = 0.55f + 0.9f * centred(p_[kSpread]);
  float density = 0.0f;  // edges per sample = RMS^2 of the differentiated bank
  for (int i = 0; i < 6; ++i) {
    inc_[i] = phaseInc(base * powf(kRatio[i], spread), fs_);
    density += float(inc_[i]) * (2.0f / 4294967296.0f);
  }
  // Same loudness whatever the tuning, spread, note or sample rate.
  metalNorm_ = clampf(sqrtf(0.1766f / (density + 1.0e-6f)), 0.3f, 3.0f);
}

void HatVoice::setFilter() {
  const float t = unit(p_[kTone]);
  const float fc = clampf(expMap(t, 3000.0f, 12000.0f) * bright_, 1000.0f, 0.42f * fs_);
  const float c = unit(p_[kColour]);
  const float q = expMap(c, 0.6f, 5.0f);
  svf_.set(fc, q, fs_);
  bpMix_ = 0.3f + 0.6f * c;
  // One-pole low-pass a little above the high-pass: the band falls gently
  // towards Nyquist instead of being white (which reads as fizz, not metal).
  tiltK_ = 1.0f - onePoleCoef(1.3f * fc, fs_);
  // Resonance and cutoff change how much gets through: hold the level steady.
  gainComp_ = sqrtf(1.5f / q) * (1.35f - 0.5f * t);
}

void HatVoice::updateCoefs(uint32_t ch) {
  const float fs = fs_;
  if (ch & (bit(kTune) | bit(kSpread))) setPitch();
  if (ch & (bit(kTone) | bit(kColour))) setFilter();
  if (ch & bit(kDecay)) {
    const float t = unit(p_[kDecay]);
    const float tau = open_ ? expMap(t, 0.04f, 1.0f) : expMap(t, 0.004f, 0.12f);
    cs_ = decayCoef(tau, fs);
    cf_ = decayCoef(open_ ? 0.006f : 0.0025f, fs);
  }
  if (ch & bit(kMetal)) {
    const float m = unit(p_[kMetal]);
    // Roughly equal-power crossfade (the two sources are uncorrelated).
    metalAmt_ = 1.3f * m * (1.6f - 0.6f * m);
    noiseAmt_ = (1.0f - m) * (1.0f + 0.6f * m);
  }
  if (ch & bit(kSnap)) snapAmt_ = 2.0f * unit(p_[kSnap]);
}

void HatVoice::noteOn(const NoteEvent& e) {
  const bool wasActive = active_;
  beginHit(false, 1.0f);  // no state reset, so no de-click offset is needed
  const float nr = noteRatio(int(e.note), 24);
  if (nr != noteRatio_) {
    noteRatio_ = nr;
    setPitch();
  }
  const float b = e.accent ? 1.25f : 1.0f;
  if (b != bright_) {
    bright_ = b;
    setFilter();
  }
  if (!wasActive) { svf_.reset(); tilt_ = 0.0f; }
  const float level = (open_ ? kOpenLevel : kClosedLevel) * velocityGain(e.vel) *
                      (e.accent ? 1.35f : 1.0f);
  es_ = level;
  ef_ = level * snapAmt_ * (e.accent ? 1.4f : 1.0f);
  limit_ = 3.0f * level;  // safety ceiling for freak coincidences of edges
}

void HatVoice::render(float* outL, float* outR, int frames) {
  uint32_t p0 = ph_[0], p1 = ph_[1], p2 = ph_[2], p3 = ph_[3], p4 = ph_[4], p5 = ph_[5];
  const uint32_t i0 = inc_[0], i1 = inc_[1], i2 = inc_[2], i3 = inc_[3], i4 = inc_[4], i5 = inc_[5];
  int32_t prev = prev_;
  float es = es_, ef = ef_, s1 = svf_.s1, s2 = svf_.s2, tilt = tilt_;
  const float cs = cs_, cf = cf_, tk = tiltK_;
  const float fg = svf_.g, fgk = svf_.gk, fd = svf_.d;
  // Source gains, with the tilt filter's input gain and the noise's int -> +-1 scale folded in.
  const float ma = metalAmt_ * metalNorm_ * gainComp_ * (1.0f - tk);
  const float na = noiseAmt_ * gainComp_ * (1.0f - tk) * (1.0f / 2147483648.0f);
  const float bm = bpMix_, lim = limit_;
  uint32_t ns = noise_.s;
  for (int i = 0; i < frames; ++i) {
    p0 += i0; p1 += i1; p2 += i2; p3 += i3; p4 += i4; p5 += i5;
    // Each term is 0 or -1: sum of six squares.
    const int32_t sq = (int32_t(p0) >> 31) + (int32_t(p1) >> 31) + (int32_t(p2) >> 31) +
                       (int32_t(p3) >> 31) + (int32_t(p4) >> 31) + (int32_t(p5) >> 31);
    const int32_t d = sq - prev;  // differentiate: an impulse per edge
    prev = sq;
    ns = ns * 1664525u + 1013904223u;
    tilt = tilt * tk + (float(d) * ma + float(int32_t(ns)) * na);
    float hp, bp, lp;
    svfTick(tilt, fg, fgk, fd, s1, s2, hp, bp, lp);
    (void)lp;
    outL[i] = clampf((hp + bm * bp) * (es + ef), -lim, lim);
    es *= cs;
    ef *= cf;
  }
  ph_[0] = p0; ph_[1] = p1; ph_[2] = p2; ph_[3] = p3; ph_[4] = p4; ph_[5] = p5;
  prev_ = prev;
  noise_.s = ns;
  es_ = es;
  ef_ = ef < 1.0e-9f ? 0.0f : ef;
  svf_.s1 = s1; svf_.s2 = s2;
  tilt_ = tilt;
  endBlock(outL, outR, frames, 1.5f * (es + ef) > kSilence);
}

}  // namespace drums
}  // namespace pg
