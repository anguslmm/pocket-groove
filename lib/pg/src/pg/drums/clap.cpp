// Clap: white noise through a band-pass (the body, around 1 kHz) plus a bit
// of high-passed "air", shaped by the classic envelope: a few fast sawtooth
// bursts a centisecond apart (several hands) followed by a longer tail (the
// room). Summed into a cubic saturator for 909-style crunch.
#include "drums.h"

namespace pg {
namespace drums {

namespace {
const ParamInfo kParams[ClapVoice::kNumParams] = {
    {"TONE", "Tone",      0, 127, 57, ParamFormat::Number, nullptr},  // band-pass 500 Hz..3.2 kHz
    {"DCY",  "Decay",     0, 127, 56, ParamFormat::Number, nullptr},  // tail tau 20..500 ms
    {"SPRD", "Spread",    0, 127, 64, ParamFormat::Number, nullptr},  // burst spacing 4..28 ms
    {"TAIL", "Tail",      0, 127, 70, ParamFormat::Number, nullptr},  // tail level
    {"BRST", "Bursts",    0, 4,   3,  ParamFormat::Number, nullptr},  // early bursts before the tail
    {"RES",  "Resonance", 0, 127, 50, ParamFormat::Number, nullptr},  // band-pass Q 0.7..4.5
    {"AIR",  "Air",       0, 127, 48, ParamFormat::Number, nullptr},  // high-passed layer
    {"DRV",  "Drive",     0, 127, 30, ParamFormat::Number, nullptr},
};
constexpr float kLevel = 0.34f;
constexpr float kNominalPeak = 0.85f;
// Slightly uneven hands: spacing multipliers and levels per burst.
const float kGap[5]   = {1.0f, 0.84f, 1.13f, 0.93f, 1.06f};
const float kBurst[5] = {0.80f, 1.0f, 0.88f, 1.0f, 0.92f};
inline uint32_t bit(int i) { return 1u << i; }
}  // namespace

// While a macro moves (DrumVoice::setParamsFine): TONE, RESONANCE, AIR, TAIL
// and DRIVE follow continuously (the band-pass also inside a ringing hit);
// DECAY, SPREAD and BURSTS shape the envelope and take their new value at the
// next hit.
ClapVoice::ClapVoice() : DrumVoice(kParams, kNumParams) {
  contMask_ = bit(kTone) | bit(kReso) | bit(kAir) | bit(kTail) | bit(kDrive);
}

void ClapVoice::updateFine(uint32_t ch) {
  if (ch & (bit(kTone) | bit(kReso) | bit(kAir))) {
    toneHz_ = 500.0f * exp2Fast(2.6780719f * unit(pf_[kTone]));   // 500..3200 Hz
    q_ = 0.7f * exp2Fast(2.6844982f * unit(pf_[kReso]));         // 0.7..4.5
    airAmt_ = unit(pf_[kAir]);
    setFilter(true);
  }
  if (ch & bit(kTail)) tailAmt_ = 0.1f + 0.9f * unit(pf_[kTail]);
  if (ch & bit(kDrive)) {
    drive_ = 0.6f * exp2Fast(4.5f * unit(pf_[kDrive]));
    post_ = driveNorm(drive_, kNominalPeak);
  }
}

void ClapVoice::resetState() {
  be_ = te_ = 0.0f;
  t_ = 0;
  burstIdx_ = 0;
  nextBurst_ = 0.0f;
  svf_.reset();
  noteRatio_ = 1.0f;
  bright_ = 1.0f;
  tailBoost_ = 1.0f;
  level_ = 0.0f;
}

void ClapVoice::setFilter(bool fast) {
  // An accented clap is brighter: band a little higher, more air.
  const float fc = clampf(toneHz_ * noteRatio_ * (bright_ > 1.0f ? 1.15f : 1.0f), 200.0f, 6000.0f);
  if (fast) svf_.setFast(fc, q_, fs_);
  else svf_.set(fc, q_, fs_);
  // White noise (variance 1/3) through a band-pass of peak gain Q has
  // variance ~ (1/3) * pi * Q * fc / fs: normalise to a constant RMS.
  const float var = (1.0f / 3.0f) * kPi * q_ * fc / fs_;
  bpGain_ = 0.25f / sqrtf(var);
  airGain_ = 0.35f * airAmt_ * airAmt_ * bright_;
}

void ClapVoice::updateCoefs(uint32_t ch) {
  const float fs = fs_;
  if (ch & (bit(kTone) | bit(kReso) | bit(kAir))) {
    toneHz_ = expMap(unit(pf_[kTone]), 500.0f, 3200.0f);
    q_ = expMap(unit(pf_[kReso]), 0.7f, 4.5f);
    airAmt_ = unit(pf_[kAir]);
    setFilter();
  }
  if (ch & bit(kDecay)) tailCoef_ = decayCoef(expMap(unit(pf_[kDecay]), 0.02f, 0.5f), fs);
  if (ch & bit(kSpread)) {
    const float gap = expMap(unit(pf_[kSpread]), 0.004f, 0.028f);
    spacing_ = gap * fs;
    // Each burst falls to about 20% by the time the next one lands.
    burstCoef_ = decayCoef(gap / 1.6f, fs);
  }
  if (ch & bit(kTail)) tailAmt_ = 0.1f + 0.9f * unit(pf_[kTail]);
  if (ch & bit(kBursts)) numBursts_ = p_[kBursts];
  if (ch & bit(kDrive)) {
    drive_ = 0.6f * powf(2.0f, 4.5f * unit(pf_[kDrive]));
    post_ = driveNorm(drive_, kNominalPeak);
  }
}

void ClapVoice::noteOn(const NoteEvent& e) {
  applyPending();
  level_ = kLevel * velocityGain(e.vel) * (e.accent ? 1.3f : 1.0f);
  beginHit(true, level_);
  const float nr = noteRatio(int(e.note), 24);
  const float b = e.accent ? 1.5f : 1.0f;
  if (nr != noteRatio_ || b != bright_) {
    noteRatio_ = nr;
    bright_ = b;
    setFilter();
  }
  tailBoost_ = e.accent ? 1.2f : 1.0f;
  svf_.reset();
  t_ = 0;
  burstIdx_ = 0;
  nextBurst_ = 0.0f;
  be_ = te_ = 0.0f;
}

void ClapVoice::render(float* outL, float* outR, int frames) {
  // Envelope events land on quantum edges (0.36 ms resolution).
  while (burstIdx_ <= numBursts_ && float(t_) >= nextBurst_) {
    be_ = kBurst[burstIdx_];
    if (burstIdx_ == numBursts_) {
      // The last hit starts the tail; together they start at the burst level.
      te_ = tailAmt_ * tailBoost_;
      be_ = be_ > te_ ? be_ - te_ : 0.0f;
    }
    nextBurst_ += spacing_ * kGap[burstIdx_];
    ++burstIdx_;
  }
  t_ += frames;

  float be = be_, te = te_, s1 = svf_.s1, s2 = svf_.s2;
  const float bc = burstCoef_, tc = tailCoef_;
  const float fg = svf_.g, fgk = svf_.gk, fd = svf_.d;
  const float bg = bpGain_, ag = airGain_, g = drive_, post = post_ * level_;
  uint32_t ns = noise_.s;
  for (int i = 0; i < frames; ++i) {
    ns = ns * 1664525u + 1013904223u;
    const float n = float(int32_t(ns)) * (1.0f / 2147483648.0f);
    float hp, bp, lp;
    svfTick(n, fg, fgk, fd, s1, s2, hp, bp, lp);
    (void)lp;
    const float s = (bp * bg + hp * ag) * (be + te);
    be *= bc;
    te *= tc;
    outL[i] = saturate(s * g) * post;
  }
  noise_.s = ns;
  be_ = be < 1.0e-9f ? 0.0f : be;
  te_ = te;
  svf_.s1 = s1; svf_.s2 = s2;
  float est = 1.5f * g * (be + te);
  if (est > 1.0f) est = 1.0f;
  const bool pending = burstIdx_ <= numBursts_;
  endBlock(outL, outR, frames, pending || est * post > kSilence);
}

}  // namespace drums
}  // namespace pg
