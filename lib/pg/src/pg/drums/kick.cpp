// Kick: sine body (magic-circle oscillator, phase reset to zero on every hit)
// with an exponential pitch sweep, hold + exponential amp decay, a second
// harmonic for small-speaker presence, cubic saturation, a click layer
// (pulse + noise burst) and a 12 dB/oct tone low-pass.
#include "drums.h"

namespace pg {
namespace drums {

namespace {
const ParamInfo kParams[KickVoice::kNumParams] = {
    {"TUNE", "Tune",       0, 127, 60, ParamFormat::Number, nullptr},  // 28..101 Hz
    {"DCY",  "Decay",      0, 127, 46, ParamFormat::Number, nullptr},  // tau 20 ms..1 s
    {"PNCH", "Punch",      0, 127, 76, ParamFormat::Number, nullptr},  // pitch sweep depth
    {"SWP",  "Sweep Time", 0, 127, 58, ParamFormat::Number, nullptr},  // pitch sweep tau 3..120 ms
    {"CLK",  "Click",      0, 127, 72, ParamFormat::Number, nullptr},  // transient level
    {"DRV",  "Drive",      0, 127, 28, ParamFormat::Number, nullptr},  // saturation
    {"TONE", "Tone",       0, 127, 100, ParamFormat::Number, nullptr}, // low-pass 150 Hz..14 kHz
    {"HOLD", "Hold",       0, 127, 30, ParamFormat::Number, nullptr},  // 0..180 ms at full level
    {"HARM", "Harmonics",  0, 127, 36, ParamFormat::Number, nullptr},  // 2nd harmonic amount
    {"CTON", "Click Tone", 0, 127, 56, ParamFormat::Number, nullptr},  // tick .. noisy thwack
};
constexpr float kLevel = 0.47f;          // body ceiling; with the default click the peak is 0.5 (-6 dBFS)
constexpr uint32_t kNoiseSeed = 0x1F2E3D4Cu;  // identical click on every hit
inline uint32_t bit(int i) { return 1u << i; }
}  // namespace

KickVoice::KickVoice() : DrumVoice(kParams, kNumParams) {}

void KickVoice::resetState() {
  x_ = 0.0f; y_ = 1.0f;
  pe_ = amp_ = ce_ = lp1_ = lp2_ = 0.0f;
  hold_ = 0;
  level_ = 0.0f;
  noteRatio_ = 1.0f;
  accentDepth_ = 1.0f;
}

void KickVoice::setPitch() {
  const float f0 = clampf(tuneHz_ * noteRatio_, 16.0f, 1200.0f);
  eps0_ = oscEps(f0, fs_);
  // Keep the top of the sweep well under Nyquist.
  float d = depthBase_ * accentDepth_;
  const float dMax = 0.2f * fs_ / f0 - 1.0f;
  depth_ = d > dMax ? dMax : d;
}

void KickVoice::updateCoefs(uint32_t ch) {
  const float fs = fs_;
  if (ch & bit(kTune)) tuneHz_ = 28.0f * powf(2.0f, 1.85f * unit(p_[kTune]));
  if (ch & bit(kPunch)) {
    const float t = unit(p_[kPunch]);
    depthBase_ = 12.0f * t * t;
  }
  if (ch & (bit(kTune) | bit(kPunch))) setPitch();
  if (ch & bit(kDecay)) ampCoef_ = decayCoef(expMap(unit(p_[kDecay]), 0.02f, 1.0f), fs);
  if (ch & bit(kSweep)) pitchCoef_ = decayCoef(expMap(unit(p_[kSweep]), 0.003f, 0.12f), fs);
  if (ch & (bit(kClick) | bit(kClickTone))) {
    const float c = unit(p_[kClick]), t = unit(p_[kClickTone]);
    clickAmt_ = 0.75f * c * (0.8f + 0.2f * c);
    clickCoef_ = decayCoef(expMap(t, 0.0005f, 0.008f), fs);
    pulseShare_ = 0.6f * (1.0f - 0.8f * t);  // short: a tick with some pulse
    noiseShare_ = 0.6f + 0.4f * t;           // long: a noisy thwack
  }
  if (ch & (bit(kDrive) | bit(kHarm))) {
    const float t = unit(p_[kDrive]);
    h2_ = 0.7f * unit(p_[kHarm]);
    drive_ = 0.5f * powf(2.0f, 5.5f * t);
    post_ = driveNorm(drive_, 1.0f);
    // Heavy clipping raises RMS a lot at constant peak: pull the level down a bit.
    driveComp_ = 1.0f - 0.3f * t * t;
  }
  if (ch & bit(kTone)) lpG_ = onePoleCoef(150.0f * powf(2.0f, 6.6f * unit(p_[kTone])), fs);
  if (ch & bit(kHold)) {
    const float t = unit(p_[kHold]);
    holdSamples_ = int(0.18f * t * t * fs);
  }
}

void KickVoice::noteOn(const NoteEvent& e) {
  const float v = float(e.vel) * (1.0f / 127.0f);
  level_ = kLevel * velocityGain(e.vel) * (e.accent ? 1.3f : 1.0f);
  beginHit(true, 1.1f * level_);
  noteRatio_ = noteRatio(int(e.note), 60);
  accentDepth_ = e.accent ? 1.25f : 1.0f;
  setPitch();
  // Click: louder with accent, slightly softer on quiet hits.
  // It is added after the saturator (as on the analogue machines, where the
  // click joins at the mixer), so drive cannot swallow it. The pulse part is
  // negative: the body's first swing is positive, so the kick opens with one
  // fast edge and the pulse has mostly gone by the time the body peaks.
  float ck = clickAmt_ * (e.accent ? 1.5f : 1.0f) * (0.6f + 0.4f * v);
  if (ck > 0.8f) ck = 0.8f;
  clickPulse_ = -ck * pulseShare_;
  clickNoise_ = ck * noiseShare_;
  // Deterministic start: phase 0, zeroed filters, fixed noise seed.
  x_ = 0.0f; y_ = 1.0f;
  pe_ = 1.0f; amp_ = 1.0f; ce_ = 1.0f;
  lp1_ = lp2_ = 0.0f;
  hold_ = holdSamples_;
  noise_.s = kNoiseSeed;
}

void KickVoice::render(float* outL, float* outR, int frames) {
  float x = x_, y = y_, pe = pe_, amp = amp_, lp1 = lp1_, lp2 = lp2_;
  const float eps0 = eps0_, depth = depth_, pc = pitchCoef_, h2 = h2_, g = drive_;
  const float bg = post_ * driveComp_ * level_, lg = lpG_;
  float ac = ampCoef_;
  if (hold_ > 0) {
    hold_ -= frames;
    ac = 1.0f;
  }
  if (ce_ > 0.0f) {
    // Attack: body + click layer.
    float ce = ce_;
    // The int -> +-1 scale of the noise is folded into its gain.
    const float cc = clickCoef_, cp = clickPulse_, cn = clickNoise_ * (1.0f / 2147483648.0f);
    uint32_t ns = noise_.s;
    for (int i = 0; i < frames; ++i) {
      const float e = eps0 * (1.0f + depth * pe);
      pe *= pc;
      x += e * y;
      y -= e * x;
      const float s = (x + h2 * x * y) * amp;
      amp *= ac;
      ns = ns * 1664525u + 1013904223u;
      const float n = float(int32_t(ns));
      const float b = softClip(s * g);
      const float m = (b + ce * (cp + cn * n)) * bg;
      ce *= cc;
      lp1 += lg * (m - lp1);
      lp2 += lg * (lp1 - lp2);
      outL[i] = lp2;
    }
    noise_.s = ns;
    ce_ = ce < 1.0e-4f ? 0.0f : ce;
  } else {
    for (int i = 0; i < frames; ++i) {
      const float e = eps0 * (1.0f + depth * pe);
      pe *= pc;
      x += e * y;
      y -= e * x;
      const float s = (x + h2 * x * y) * amp;
      amp *= ac;
      const float m = softClip(s * g) * bg;
      lp1 += lg * (m - lp1);
      lp2 += lg * (lp1 - lp2);
      outL[i] = lp2;
    }
  }
  x_ = x; y_ = y; amp_ = amp; lp1_ = lp1; lp2_ = lp2;
  pe_ = pe < 1.0e-9f ? 0.0f : pe;
  // Output amplitude bound: |softClip(a)| <= min(1, 1.5 a).
  float est = 1.5f * (1.0f + 0.5f * h2) * amp * g;
  if (est > 1.0f) est = 1.0f;
  const bool alive = est * bg > kSilence || ce_ > 0.0f;
  endBlock(outL, outR, frames, alive);
}

}  // namespace drums
}  // namespace pg
