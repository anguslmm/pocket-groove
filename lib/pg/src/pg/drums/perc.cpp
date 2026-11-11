// Perc: six small models behind one parameter set.
//
//   Tom, Rim, Clave, Zap  "tonal" engine: two sine modes with a shared pitch
//                         bend, a filtered noise burst, cubic saturation.
//   Cowbell               two square oscillators -> band-pass, two-stage decay.
//   Shaker                (optionally sparse) noise -> resonant high-pass,
//                         soft attack / exponential decay.
//
// Shared params, per model:
//            TUNE        DCY     TONE            BEND          HARM          NOIS         DRV
//   Tom      45-290 Hz   tail    upper mode+nz   pitch drop    mode ratio    stick noise  drive
//   Rim      250-1000Hz  tail    upper mode lvl  pitch drop    mode ratio    click        drive
//   Cowbell  270-1080Hz  tail    band-pass freq  +-pitch env   osc interval  breath       drive
//   Clave    0.9-6.7 kHz tail    upper mode lvl  pitch drop    mode ratio    click        drive
//   Shaker   filter freq tail    resonance       attack time   graininess    dry noise    drive
//   Zap      60-1200 Hz  length  octave partial  sweep depth   partial ratio noise        drive
//
// The model is latched at note-on, so changing it never glitches a sounding hit.
#include "drums.h"

namespace pg {
namespace drums {

namespace {
const char* const kModelNames[PercVoice::kNumModels] = {"TOM", "RIM", "COWB", "CLAV", "SHKR", "ZAP"};
const ParamInfo kParams[PercVoice::kNumParams] = {
    {"MODL", "Model",     0, PercVoice::kNumModels - 1, PercVoice::kRim, ParamFormat::Enum, kModelNames},
    {"TUNE", "Tune",      0, 127, 64, ParamFormat::Number, nullptr},
    {"DCY",  "Decay",     0, 127, 64, ParamFormat::Number, nullptr},
    {"TONE", "Tone",      0, 127, 64, ParamFormat::Number, nullptr},
    {"BEND", "Bend",      0, 127, 64, ParamFormat::Number, nullptr},
    {"HARM", "Harmonic",  0, 127, 64, ParamFormat::Number, nullptr},
    {"NOIS", "Noise",     0, 127, 64, ParamFormat::Number, nullptr},
    {"DRV",  "Drive",     0, 127, 32, ParamFormat::Number, nullptr},
};
inline uint32_t bit(int i) { return 1u << i; }
}  // namespace

PercVoice::PercVoice() : DrumVoice(kParams, kNumParams) {}

void PercVoice::resetState() {
  x1_ = x2_ = 0.0f; y1_ = y2_ = 1.0f;
  pe_ = a1_ = a2_ = an_ = nl_ = 0.0f;
  ph1_ = ph2_ = 0;
  svf_.reset();
  noteRatio_ = 1.0f;
  accentMul_ = 1.0f;
  level_ = 0.0f;
  model_ = p_[kModel];
}

void PercVoice::updateCoefs(uint32_t) {
  // A model change waits for the next note-on; everything else is live.
  if (!active_) model_ = p_[kModel];
  computeModel();
}

void PercVoice::computeModel() {
  const float fs = fs_;
  const float tune = unit(p_[kTune]), dcy = unit(p_[kDecay]), tone = unit(p_[kTone]);
  const float bendT = unit(p_[kBend]), harmC = centred(p_[kHarm]), nois = unit(p_[kNoise]);
  const float drv = unit(p_[kDrive]);
  const float acc = accentMul_;  // 1, or > 1 for an accented hit: brighter
  float g = 0.5f * powf(2.0f, 4.5f * drv);
  float nominal = 0.9f;

  // Defaults for the tonal engine.
  float f1 = 440.0f, ratio = 2.0f, tau1 = 0.05f, tau2k = 0.5f, bend = 0.0f, tauP = 0.01f;
  float tauN = 0.005f, noiseHz = 3000.0f;
  bool noiseHigh = true;

  switch (model_) {
    case kTom:
      f1 = 45.0f * powf(6.5f, tune);
      ratio = 1.0f + 1.2f * harmC;
      tau1 = expMap(dcy, 0.025f, 0.45f);
      tau2k = 0.45f;
      bend = 1.6f * bendT * bendT;
      tauP = clampf(0.3f * tau1, 0.005f, 0.06f);
      amt1_ = 0.62f;
      amt2_ = 0.4f * tone * acc;
      amtN_ = 0.5f * nois * nois * acc;
      tauN = 0.012f;
      noiseHz = expMap(tone, 800.0f, 8000.0f);
      noiseHigh = false;
      modelLevel_ = 0.30f;
      break;
    case kRim:
      f1 = 250.0f * powf(4.0f, tune);
      ratio = 1.5f + 2.5f * harmC;
      tau1 = expMap(dcy, 0.003f, 0.05f);
      tau2k = 0.8f;
      bend = 0.5f * bendT * bendT;
      tauP = 0.003f;
      amt1_ = 0.5f;
      amt2_ = (0.2f + 0.7f * tone) * acc;
      amtN_ = 0.7f * nois * acc;
      tauN = 0.0015f;
      noiseHz = 3000.0f;
      g *= 2.5f;  // the rimshot is a clipped sound by nature
      modelLevel_ = 0.24f;
      break;
    case kClave:
      f1 = 900.0f * powf(7.5f, tune);
      ratio = 1.5f + 2.0f * harmC;
      tau1 = expMap(dcy, 0.004f, 0.08f);
      tau2k = 0.5f;
      bend = 0.3f * bendT * bendT;
      tauP = 0.002f;
      amt1_ = 0.68f;
      amt2_ = 0.6f * tone * tone * acc;
      amtN_ = 0.4f * nois * acc;
      tauN = 0.001f;
      noiseHz = 4000.0f;
      modelLevel_ = 0.22f;
      break;
    case kZap:
      f1 = 60.0f * powf(20.0f, tune);
      ratio = 1.0f + 2.0f * harmC;
      tau1 = expMap(dcy, 0.01f, 0.5f);
      tau2k = 1.0f;
      bend = 30.0f * bendT * bendT * acc;
      tauP = expMap(dcy, 0.004f, 0.12f);
      amt1_ = 0.62f;
      amt2_ = 0.45f * tone;
      amtN_ = 0.3f * nois * nois;
      tauN = 0.02f;
      noiseHz = 5000.0f;
      noiseHigh = false;
      modelLevel_ = 0.24f;
      break;
    case kCowbell: {
      const float fa = clampf(540.0f * powf(2.0f, 2.0f * centred(p_[kTune]) - 1.0f) * noteRatio_,
                              40.0f, 0.2f * fs);
      const float fb = clampf(fa * (1.18f + 0.6f * harmC), 40.0f, 0.3f * fs);
      eps1_ = fa / fs * 4294967296.0f;  // phase increments, as floats
      eps2_ = fb / fs * 4294967296.0f;
      bend_ = 0.5f * (centred(p_[kBend]) - 0.5f);
      pitchCoef_ = decayCoef(0.03f, fs / float(kQuantum));
      svf_.set(expMap(tone, 1200.0f, 5500.0f) * acc, 1.3f, fs);
      c1_ = decayCoef(expMap(dcy, 0.015f, 0.5f), fs);
      c2_ = decayCoef(0.010f, fs);
      amt1_ = 0.5f;
      amt2_ = 0.5f;
      srcGain_ = 0.9f;
      amtN_ = 0.4f * nois * nois;
      nominal = 0.8f;
      modelLevel_ = 0.20f;
      break;
    }
    case kShaker: {
      const float fc = clampf(expMap(tune, 2500.0f, 11000.0f) * noteRatio_ * acc, 500.0f, 0.42f * fs);
      const float q = expMap(tone, 0.6f, 3.5f);
      svf_.set(fc, q, fs);
      // Constant-RMS band-passed noise (see clap.cpp), nominal RMS 0.25.
      svfNorm_ = 0.25f / sqrtf((1.0f / 3.0f) * kPi * q * fc / fs);
      const float tauD = expMap(dcy, 0.008f, 0.25f);
      float tauA = expMap(bendT, 0.0004f, 0.04f);
      if (tauA > 0.5f * tauD) tauA = 0.5f * tauD;
      c1_ = decayCoef(tauD, fs);
      c2_ = decayCoef(tauA, fs);
      // Peak of exp(-t/tauD) - exp(-t/tauA), to normalise the envelope to 1.
      const float tp = logf(tauD / tauA) * tauA * tauD / (tauD - tauA);
      amt1_ = 1.0f / (expf(-tp / tauD) - expf(-tp / tauA));
      // Grain: fraction of noise samples let through (1 = white .. 1.5% = rattle).
      const float dens = expMap(harmC * harmC, 1.0f, 0.015f);
      grain_ = dens * 4096.0f;
      srcGain_ = 1.0f / sqrtf(dens);
      if (srcGain_ > 4.0f) srcGain_ = 4.0f;
      dry_ = 0.5f * nois * nois;
      amtN_ = 0.3f;  // high-pass share: mostly band-pass, so the top is not white
      nominal = 0.9f;
      modelLevel_ = 0.15f;
      break;
    }
    default: break;
  }

  if (model_ != kCowbell && model_ != kShaker) {
    f1 = clampf(f1 * noteRatio_, 20.0f, 0.2f * fs);
    const float f2 = clampf(f1 * ratio, 20.0f, 0.22f * fs);
    eps1_ = oscEps(f1, fs);
    eps2_ = oscEps(f2, fs);
    const float bendMax = 0.23f * fs / (f2 > f1 ? f2 : f1) - 1.0f;
    bend_ = bend > bendMax ? bendMax : bend;
    pitchCoef_ = decayCoef(tauP, fs);
    c1_ = decayCoef(tau1, fs);
    c2_ = decayCoef(tau1 * tau2k, fs);
    cn_ = decayCoef(tauN, fs);
    noiseG_ = onePoleCoef(noiseHz, fs);
    if (noiseHigh) { noiseHp_ = 1.0f; noiseLp_ = -1.0f; }  // n - lp(n)
    else           { noiseHp_ = 0.0f; noiseLp_ = 1.6f; }
    nominal = clampf(amt1_ + 0.6f * amt2_ + 0.3f * amtN_, 0.3f, 1.5f);
  }
  drive_ = g;
  post_ = driveNorm(g, nominal);
}

void PercVoice::noteOn(const NoteEvent& e) {
  const bool wasActive = active_;
  const float nr = noteRatio(int(e.note), 24);
  const float acc = e.accent ? 1.3f : 1.0f;
  if (model_ != p_[kModel] || nr != noteRatio_ || acc != accentMul_) {
    model_ = p_[kModel];
    noteRatio_ = nr;
    accentMul_ = acc;
    computeModel();
  }
  level_ = modelLevel_ * velocityGain(e.vel) * (e.accent ? 1.3f : 1.0f);
  active_ = wasActive;
  beginHit(true, level_);
  x1_ = x2_ = 0.0f; y1_ = y2_ = 1.0f;
  pe_ = 1.0f;
  nl_ = 0.0f;
  ph1_ = ph2_ = 0;
  svf_.reset();
  switch (model_) {
    case kCowbell: a1_ = amt1_; a2_ = amt2_; an_ = 0.0f; break;
    case kShaker:  a1_ = 1.0f;  a2_ = 1.0f;  an_ = 0.0f; break;
    default:       a1_ = amt1_; a2_ = amt2_; an_ = amtN_; break;
  }
}

bool PercVoice::renderTonal(float* out, int frames) {
  float x1 = x1_, y1 = y1_, x2 = x2_, y2 = y2_, pe = pe_;
  float a1 = a1_, a2 = a2_, an = an_, nl = nl_;
  const float eps1 = eps1_, eps2 = eps2_, bend = bend_, pc = pitchCoef_;
  const float c1 = c1_, c2 = c2_, cn = cn_, ng = noiseG_, nh = noiseHp_, nlm = noiseLp_;
  const float g = drive_, post = post_ * level_;
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
    nl += ng * (n - nl);
    const float nz = (n * nh + nl * nlm) * an;
    an *= cn;
    out[i] = softClip((body + nz) * g) * post;
  }
  noise_.s = ns;
  x1_ = x1; y1_ = y1; x2_ = x2; y2_ = y2; nl_ = nl;
  pe_ = pe < 1.0e-9f ? 0.0f : pe;
  a1_ = a1 < 1.0e-9f ? 0.0f : a1;
  a2_ = a2 < 1.0e-9f ? 0.0f : a2;
  an_ = an < 1.0e-9f ? 0.0f : an;
  float est = 1.5f * g * (a1 + a2 + an);
  if (est > 1.0f) est = 1.0f;
  return est * post > kSilence;
}

bool PercVoice::renderCowbell(float* out, int frames) {
  // Pitch envelope at control rate (the oscillators are integer phase accumulators).
  const float pm = 1.0f + bend_ * pe_;
  pe_ *= pitchCoef_;
  if (pe_ < 1.0e-9f) pe_ = 0.0f;
  const uint32_t inc1 = uint32_t(eps1_ * pm), inc2 = uint32_t(eps2_ * pm);
  uint32_t ph1 = ph1_, ph2 = ph2_;
  float a1 = a1_, a2 = a2_, s1 = svf_.s1, s2 = svf_.s2;
  const float c1 = c1_, c2 = c2_, sg = srcGain_, na = amtN_;
  const float fg = svf_.g, fgk = svf_.gk, fd = svf_.d;
  const float g = drive_, post = post_ * level_;
  uint32_t ns = noise_.s;
  for (int i = 0; i < frames; ++i) {
    ph1 += inc1;
    ph2 += inc2;
    const int32_t sq = (int32_t(ph1) >> 31) + (int32_t(ph2) >> 31) + 1;  // -1, 0, +1
    ns = ns * 1664525u + 1013904223u;
    const float n = float(int32_t(ns)) * (1.0f / 2147483648.0f);
    const float src = float(sq) * sg + n * na;
    float hp, bp, lp;
    svfTick(src, fg, fgk, fd, s1, s2, hp, bp, lp);
    (void)hp; (void)lp;
    out[i] = softClip(bp * (a1 + a2) * g) * post;
    a1 *= c1;
    a2 *= c2;
  }
  noise_.s = ns;
  ph1_ = ph1; ph2_ = ph2;
  a1_ = a1;
  a2_ = a2 < 1.0e-9f ? 0.0f : a2;
  svf_.s1 = s1; svf_.s2 = s2;
  float est = 1.5f * g * (a1 + a2);
  if (est > 1.0f) est = 1.0f;
  return est * post > kSilence;
}

bool PercVoice::renderShaker(float* out, int frames) {
  float a1 = a1_, a2 = a2_, s1 = svf_.s1, s2 = svf_.s2;
  const float c1 = c1_, c2 = c2_, norm = amt1_, bm = amtN_, dry = dry_, sg = srcGain_ * svfNorm_;
  const uint32_t dens = uint32_t(grain_);
  const float fg = svf_.g, fgk = svf_.gk, fd = svf_.d;
  const float g = drive_, post = post_ * level_;
  uint32_t ns = noise_.s;
  for (int i = 0; i < frames; ++i) {
    ns = ns * 1664525u + 1013904223u;
    // Top 12 bits gate the sample (sparse noise = grains), the rest are its value.
    const float n = float(int32_t(ns << 12)) * (1.0f / 2147483648.0f);
    const float src = (ns >> 20) < dens ? n * sg : 0.0f;
    float hp, bp, lp;
    svfTick(src, fg, fgk, fd, s1, s2, hp, bp, lp);
    (void)lp;
    const float env = (a1 - a2) * norm;
    a1 *= c1;
    a2 *= c2;
    out[i] = softClip((bp + bm * hp + dry * src) * env * g) * post;
  }
  noise_.s = ns;
  a1_ = a1;
  a2_ = a2 < 1.0e-9f ? 0.0f : a2;
  svf_.s1 = s1; svf_.s2 = s2;
  float est = 1.5f * g * a1 * norm;
  if (est > 1.0f) est = 1.0f;
  return est * post > kSilence;
}

void PercVoice::render(float* outL, float* outR, int frames) {
  bool alive;
  switch (model_) {
    case kCowbell: alive = renderCowbell(outL, frames); break;
    case kShaker:  alive = renderShaker(outL, frames); break;
    default:       alive = renderTonal(outL, frames); break;
  }
  endBlock(outL, outR, frames, alive);
}

}  // namespace drums
}  // namespace pg
