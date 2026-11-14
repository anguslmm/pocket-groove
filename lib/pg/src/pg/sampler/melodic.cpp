// MelodicSampler (melodic.h). No libm anywhere: every mapping uses the drums'
// polynomial exp2 / sin / cos (drum_dsp.h), so the byte path and the fractional
// path compute the same way and the control-rate updates are cheap.
#include "melodic.h"

#include <cstring>

#include "../drums/drum_dsp.h"
#include "../lead/lead.h"

namespace pg {
namespace sampler {

namespace {

using drums::clampf;
using drums::exp2Fast;

constexpr float kSilentEnv = 1.0e-4f;   // -80 dB: a released note ends here
constexpr float kPiF = 3.14159265f;
constexpr float kQ = float(kQuantum);
constexpr float kInv32768 = 1.0f / 32768.0f;
// Per-note gain by the size of its chord (1 / sqrt(n): a chord's notes add up to
// about the level of one note, so lines and chords sit close without pumping).
constexpr float kChordGain[mel::kMaxNotes + 1] = {1.0f, 1.0f, 0.70710678f, 0.57735027f, 0.5f};

inline float absf(float x) { return x < 0.0f ? -x : x; }
inline float unit(float v) { return v * (1.0f / 127.0f); }

// The window's minimum length in a sample of len samples at `rate`.
int32_t winMin(uint32_t len, uint32_t rate) {
  int32_t minW = int32_t(MelodicSampler::kMinWindowMs * 0.001f * float(rate));
  if (minW < SamplerVoice::kMinRegion) minW = SamplerVoice::kMinRegion;
  if (minW > int32_t(len)) minW = int32_t(len);
  return minW;
}

// Where START puts a window's start (0 = the sample's beginning), leaving room for
// the minimum window before the end.
int32_t winStart(float start, uint32_t len, uint32_t rate) {
  const int32_t L = int32_t(len), minW = winMin(len, rate);
  int32_t a = int32_t(float(L) * clampf(start, 0.0f, 127.0f) * (1.0f / 128.0f));
  if (L - a < minW) a = L - minW;
  return a < 0 ? 0 : a;
}

// The window's end for LENGTH from its start a: the minimum (0) .. the sample's end
// (127), a square law between.
int32_t winEnd(int32_t a, float length, uint32_t len, uint32_t rate) {
  const int32_t L = int32_t(len), minW = winMin(len, rate);
  const float f = clampf(length, 0.0f, 127.0f) * (1.0f / 127.0f);
  float rest = float(L - a - minW);
  if (rest < 0.0f) rest = 0.0f;
  int32_t b = a + minW + int32_t(rest * f * f);
  if (b > L) b = L;
  if (b - a < 2) b = a + 2 <= L ? a + 2 : L;
  return b;
}

// sin(2 pi p) for p in [0, 1), a parabola pair (control rate only: the drift's wave)
float sinTurn(float p) {
  const float x = p < 0.5f ? p : p - 0.5f;          // 0 .. 0.5
  const float y = 16.0f * x * (0.5f - x);           // 0 .. 1 .. 0
  return p < 0.5f ? y : -y;
}

}  // namespace

MelodicSampler::MelodicSampler() {
  for (int i = 0; i < mel::kNumParams; ++i) {
    raw_[i] = mel::paramInfo(i)->def;
    pf_[i] = float(raw_[i]);
  }
  std::memset(static_cast<void*>(n_), 0, sizeof n_);
  for (int k = 0; k < mel::kMaxNotes; ++k) playhead_[k].store(-1, std::memory_order_relaxed);
}

void MelodicSampler::init(float sampleRate) {
  fs_ = clampf(sampleRate, 8000.0f, 192000.0f);
  invFs_ = 1.0f / fs_;
  // de-click offset: time constant 0.6 ms (-60 dB in ~4 ms)
  residK_ = exp2Fast(-1.44269504f / (0.0006f * fs_));
  killQuanta_ = int(0.002f * fs_ / kQ + 0.5f);
  if (killQuanta_ < 1) killQuanta_ = 1;
  std::memset(static_cast<void*>(n_), 0, sizeof n_);
  groupCounter_ = ageCounter_ = lastGroup_ = 0;
  fenv_ = 0.0f;
  s1_ = s2_ = 0.0f;
  holdPh_ = holdVal_ = 0.0f;
  resid_ = residNext_ = 0.0f;
  driftPh_ = driftWander_ = driftTarget_ = drift_ = 0.0f;
  rng_ = 0x9E3779B9u;
  fine_ = false;
  active_ = false;
  toneDirty_ = true;
  updateTone();
  for (int k = 0; k < mel::kMaxNotes; ++k) playhead_[k].store(-1, std::memory_order_relaxed);
}

void MelodicSampler::region(const uint8_t* p, uint32_t length, uint32_t rate, int32_t* a, int32_t* b) {
  *a = winStart(float(p[mel::kStart]), length, rate);
  *b = winEnd(*a, float(p[mel::kLength]), length, rate);
}

void MelodicSampler::setParams(const uint8_t* p) {
  if (!fine_ && std::memcmp(p, raw_, mel::kNumParams) == 0) return;
  fine_ = false;
  for (int i = 0; i < mel::kNumParams; ++i) {
    const ParamInfo* pi = mel::paramInfo(i);
    raw_[i] = p[i];
    pf_[i] = float(p[i] < pi->min ? pi->min : (p[i] > pi->max ? pi->max : p[i]));
  }
  updateTone();
}

void MelodicSampler::setParamsFine(const float* v) {
  fine_ = true;
  bool changed = false;
  for (int i = 0; i < mel::kNumParams; ++i) {
    const ParamInfo* pi = mel::paramInfo(i);
    const float x = clampf(v[i], float(pi->min), float(pi->max));
    if (x != pf_[i]) {
      pf_[i] = x;
      raw_[i] = uint8_t(int(x + 0.5f));
      changed = true;
    }
  }
  if (changed) updateTone();
}

// The track's tune: only the pitch moves (the notes take it at their next quantum).
void MelodicSampler::setTune(float semitones) {
  if (semitones == trackTune_) return;
  trackTune_ = semitones;
  tune_ = (pf_[mel::kPitch] - 64.0f) + (pf_[mel::kFine] - 64.0f) * (1.0f / 64.0f) + trackTune_;
}

// Everything derived from the parameters (control rate).
void MelodicSampler::updateTone() {
  const float qs = kQ * invFs_;   // seconds per quantum
  const float atk = 0.001f * exp2Fast(10.966f * unit(pf_[mel::kAttack]));       // 1 ms .. 2 s
  atkStepQ_ = qs / atk;
  const float dec = 0.02f * exp2Fast(9.229f * unit(pf_[mel::kDecay]));         // T60 20 ms .. 12 s
  decCoefQ_ = exp2Fast(-9.9658f * qs / dec);
  sus_ = unit(pf_[mel::kSustain]);
  const float rel = 0.005f * exp2Fast(9.644f * unit(pf_[mel::kRelease]));      // T60 5 ms .. 4 s
  relCoefQ_ = exp2Fast(-9.9658f * qs / rel);
  const float gl = 0.002f * exp2Fast(7.966f * unit(pf_[mel::kGlide]));         // tau 2 ms .. 0.5 s
  glideCoef_ = 1.0f - exp2Fast(-1.44269504f * qs / gl);
  const float fd = 0.03f * exp2Fast(7.059f * unit(pf_[mel::kFltDecay]));       // T60 30 ms .. 4 s
  fdecCoefQ_ = exp2Fast(-9.9658f * qs / fd);
  envOct_ = 5.0f * unit(pf_[mel::kEnvAmt]);                                     // 0 .. 60 semitones
  tune_ = (pf_[mel::kPitch] - 64.0f) + (pf_[mel::kFine] - 64.0f) * (1.0f / 64.0f) + trackTune_;
  const float dd = unit(pf_[mel::kDrift]);
  driftDepth_ = 0.6f * dd * dd;                                                  // 0 .. 0.6 semitones
  driftInc_ = 0.1f * exp2Fast(6.322f * unit(pf_[mel::kDriftRate])) * qs;        // 0.1 .. 8 Hz, turns per quantum

  ++winGen_;   // the notes' windows follow START / LENGTH
  ftype_ = uint8_t(clampf(float(int(pf_[mel::kFilter] + 0.5f)), 0.0f, float(kNumFilters - 1)));
  cutOct_ = 7.45f * unit(pf_[mel::kCutoff]);
  const float rr = unit(pf_[mel::kReso]);
  k_ = 1.0f / (0.6f + 11.0f * rr * rr);
  filterOn_ = !(ftype_ == kLP && pf_[mel::kCutoff] >= 126.5f && pf_[mel::kEnvAmt] < 0.5f);

  const float bits = pf_[mel::kBits];
  if (bits > 0.5f) {
    const float nb = 16.0f - 14.0f * bits * (1.0f / 127.0f);
    crushQ_ = exp2Fast(1.0f - nb);
    crushInv_ = 1.0f / crushQ_;
  } else {
    crushQ_ = 0.0f;
  }
  const float dn = pf_[mel::kDown];
  holdInc_ = 22050.0f * exp2Fast(-5.0f * dn * (1.0f / 127.0f)) * invFs_;
  down_ = dn > 0.5f && holdInc_ < 1.0f;
  const float dv = unit(pf_[mel::kDrive]);
  driveOn_ = dv > 0.002f;
  drive_ = 1.0f + 15.0f * dv * dv;
  driveNorm_ = 0.9f / drums::saturate(0.9f * drive_);
  toneDirty_ = true;
}

// The note's window and its edges in 32.32. ONE: the start is the note's own
// (latched), LENGTH moves the end. PONG: START and LENGTH move the whole window.
// Only when the parameters moved (winGen_), so it stays out of the per-sample path.
__attribute__((noinline)) void MelodicSampler::window(Note& n) const {
  const int32_t wa = n.loop ? winStart(pf_[mel::kStart], n.len, n.rate) : n.startPos;
  n.a = wa;
  n.b = winEnd(wa, pf_[mel::kLength], n.len, n.rate);
  n.loF = int64_t(n.a) << 32;
  n.hiF = (int64_t(n.b - 1) << 32) - 1;   // reads [i, i + 1] stay below b
  n.winGen = winGen_;
}

// The playhead crossed the window's edge during a quantum (a turn, or a one-shot's
// end: rare, so out of the loop). PONG: it turns, continuing as far inside as it
// went past (the waveform stays continuous); false: a one-shot ends.
__attribute__((noinline)) bool MelodicSampler::edge(const Note& n, int64_t& pos, int64_t& step) const {
  if (!n.loop) return false;
  if (pos > n.hiF) {
    if (step > 0) { pos = n.hiF - (pos - n.hiF); step = -step; }
  } else if (pos < n.loF) {
    if (step < 0) { pos = n.loF + (n.loF - pos); step = -step; }
  }
  const int64_t maxF = (int64_t(n.len - 1) << 32) - 1;
  if (uint64_t(pos) > uint64_t(maxF)) pos = pos < 0 ? 0 : maxF;   // never outside the sample (a tiny window)
  return true;
}

void MelodicSampler::startNote(Note& n, uint8_t note, uint8_t root, float gain, uint32_t group) {
  SampleRef r;
  resolve(pool_, slot_, &r);
  n.data = r.data;
  n.len = r.length;
  n.gen = pool_ ? pool_->generation() : 0;
  n.builtin = r.builtin;
  n.slot = slot_;
  n.rateRatio = float(r.rate) * invFs_;
  n.gain = gain * r.gain * kLevel * kInv32768;
  n.loop = int(pf_[mel::kLoop] + 0.5f) == mel::kPingPong;
  n.reverse = pf_[mel::kReverse] >= 0.5f;
  n.rate = r.rate;
  const int32_t a = winStart(pf_[mel::kStart], n.len, r.rate), b = winEnd(a, pf_[mel::kLength], n.len, r.rate);
  n.startPos = a;
  window(n);
  n.pos = n.reverse ? ((int64_t(b - 1) << 32) - 1) : (int64_t(a) << 32);
  n.step = n.reverse ? -1 : 1;   // the speed comes at the next quantum (renderNote)
  n.incSemis = 1.0e9f;
  n.semis = n.target = float(int(note) - mel::kRootNote);
  n.env = 0.0f;
  n.last = 0.0f;
  n.stage = sAttack;
  n.gate = true;
  n.note = note;
  n.root = root;
  n.group = group;
  n.age = ageCounter_;
}

// A free note; else one already playing this pitch; else the quietest released;
// else the oldest. Never one in `claimed` (bit k) unless all are.
int MelodicSampler::allocNote(uint8_t note, uint32_t claimed) const {
  int best = -1, bestCls = 99;
  float bestVal = 0.0f;
  for (int k = 0; k < mel::kMaxNotes; ++k) {
    if ((claimed >> k) & 1u) continue;
    const Note& n = n_[k];
    int cls;
    float val;
    if (n.stage == sOff) { cls = 0; val = 0.0f; }
    else if (n.note == note) { cls = 1; val = 0.0f; }
    else if (!n.gate) { cls = 2; val = n.env; }
    else { cls = 3; val = float(n.age); }
    if (cls < bestCls || (cls == bestCls && val < bestVal)) {
      best = k;
      bestCls = cls;
      bestVal = val;
    }
  }
  return best < 0 ? 0 : best;
}

void MelodicSampler::releaseNote(Note& n) {
  n.gate = false;
  if (n.stage == sAttack || n.stage == sDecay) n.stage = sRelease;
}

void MelodicSampler::killNote(Note& n) {
  if (n.stage == sOff || n.stage == sKill) return;
  n.stage = sKill;
  n.gate = false;
  n.killStep = n.env / float(killQuanta_);
}

void MelodicSampler::noteOn(const NoteEvent& e) {
  int8_t iv[lead::kMaxChordNotes];
  int cnt = lead::LeadVoice::chordIntervals(e.chord, iv);
  if (cnt > mel::kMaxNotes) cnt = mel::kMaxNotes;   // the cap: the highest notes are dropped
  const uint32_t group = ++groupCounter_;
  ++ageCounter_;
  const float vel = drums::velocityGain(e.vel) * (e.accent ? 1.2f : 1.0f);
  const bool silent = int(slot_) == silentSlot();   // being resampled: starts nothing

  // Legato: the notes of the previous note / chord glide to the new pitches.
  int legato[mel::kMaxNotes] = {-1, -1, -1, -1};
  bool isLegato = false;
  if (e.slide) {
    int idx = 0;
    for (int k = 0; k < mel::kMaxNotes; ++k) {
      const Note& n = n_[k];
      if (n.stage != sOff && n.stage != sKill && n.group == lastGroup_ && idx < mel::kMaxNotes) {
        legato[idx++] = k;
        isLegato = true;
      }
    }
    // in pitch order, lowest first, so a chord's notes glide to their counterparts
    for (int i = 1; i < idx; ++i)
      for (int j = i; j > 0 && n_[legato[j]].target < n_[legato[j - 1]].target; --j) {
        const int s = legato[j];
        legato[j] = legato[j - 1];
        legato[j - 1] = s;
      }
  }
  uint32_t claimed = 0;
  for (int i = 0; i < mel::kMaxNotes; ++i)
    if (legato[i] >= 0) claimed |= 1u << legato[i];
  bool fresh = false;
  for (int i = 0; i < cnt; ++i) {
    int nn = int(e.note) + iv[i];
    const uint8_t note = uint8_t(nn < 0 ? 0 : (nn > 127 ? 127 : nn));
    if (isLegato && legato[i] >= 0) {
      Note& n = n_[legato[i]];
      n.target = float(int(note) - mel::kRootNote);
      n.note = note;
      n.root = e.note;
      n.group = group;
      n.age = ageCounter_;
      if (!n.gate) {   // its gate had ended: gated again from where its envelope is
        n.gate = true;
        n.stage = sAttack;
      }
      continue;
    }
    if (silent) continue;
    const int k = allocNote(note, claimed);
    claimed |= 1u << k;
    Note& n = n_[k];
    if (n.stage != sOff) resid_ += n.last;   // stolen: its last output decays as an offset
    startNote(n, note, e.note, vel * kChordGain[cnt], group);
    fresh = true;
  }
  if (isLegato)
    for (int i = cnt; i < mel::kMaxNotes; ++i)
      if (legato[i] >= 0) releaseNote(n_[legato[i]]);
  if (fresh && !isLegato) fenv_ = 1.0f;   // the filter envelope: the newest (non-legato) note
  lastGroup_ = group;
  if (fresh) active_ = true;
}

void MelodicSampler::noteOff(uint8_t note) {
  for (int k = 0; k < mel::kMaxNotes; ++k) {
    Note& n = n_[k];
    if (n.gate && n.stage != sOff && n.root == note) releaseNote(n);
  }
}

void MelodicSampler::choke() {
  for (int k = 0; k < mel::kMaxNotes; ++k) killNote(n_[k]);
}

// One note over one quantum, added into acc. Control first (pitch, window,
// envelope), then the per-sample loop: read, interpolate, the envelope's ramp,
// advance, the window's edges.
void MelodicSampler::renderNote(Note& n, float* acc) {
  // ---- control ----
  n.semis += (n.target - n.semis) * glideCoef_;
  if (absf(n.target - n.semis) < 1.0e-4f) n.semis = n.target;
  const float semis = clampf(n.semis + tune_ + drift_, -48.0f, 48.0f);
  if (semis != n.incSemis) {   // the speed: only when the pitch moved
    n.incSemis = semis;
    const int64_t inc = int64_t(n.rateRatio * exp2Fast(semis * (1.0f / 12.0f)) * 4294967296.0f);
    n.step = n.step < 0 ? -inc : inc;
  }
  if (n.winGen != winGen_) window(n);
  // this quantum's bounds: the window's, widened to the playhead while it travels toward a
  // window that moved away (PONG turns toward it first: the playhead never jumps)
  int64_t loF = n.loF, hiF = n.hiF;
  if (n.loop) {
    if (n.pos > hiF && n.step > 0) n.step = -n.step;
    else if (n.pos < loF && n.step < 0) n.step = -n.step;
    if (n.pos > hiF) hiF = n.pos;
    if (n.pos < loF) loF = n.pos;
  }
  float env1 = n.env;
  bool ends = false;
  switch (n.stage) {
    case sAttack:
      env1 += atkStepQ_;
      if (env1 >= 1.0f) {
        env1 = 1.0f;
        n.stage = n.gate ? sDecay : sRelease;
      }
      break;
    case sDecay:
      env1 = sus_ + (env1 - sus_) * decCoefQ_;
      if (env1 < kSilentEnv && sus_ < kSilentEnv) { env1 = 0.0f; ends = true; }   // decayed away while held
      break;
    case sRelease:
      env1 *= relCoefQ_;
      if (env1 < kSilentEnv) { env1 = 0.0f; ends = true; }
      break;
    case sKill:
      env1 -= n.killStep;
      if (env1 <= 0.0f) { env1 = 0.0f; ends = true; }
      break;
    default:
      break;
  }
  float amp = n.env * n.gain;
  const float ampStep = (env1 - n.env) * n.gain * (1.0f / kQ);
  n.env = env1;

  // ---- per sample ----
  const int16_t* d = n.data;
  int64_t pos = n.pos, step = n.step;
  float x = 0.0f;
  int i = 0;
  for (; i < kQuantum; ++i) {
    const int32_t ix = int32_t(pos >> 32);
    const float fr = float(uint32_t(pos) >> 8) * (1.0f / 16777216.0f);
    const float s0 = float(d[ix]), s1 = float(d[ix + 1]);
    amp += ampStep;
    x = (s0 + (s1 - s0) * fr) * amp;
    acc[i] += x;
    pos += step;
    if ((pos > hiF || pos < loF) && !edge(n, pos, step)) {   // a turn; or the one-shot's end
      ++i;
      ends = true;
      break;
    }
  }
  if (i < kQuantum) {   // ended inside the quantum: its last value decays away (de-click)
    float r = x;
    for (; i < kQuantum; ++i) {
      r *= residK_;
      acc[i] += r;
    }
    x = r;
  }
  n.pos = pos;
  n.step = step;
  n.last = x;
  if (ends) {   // what is left of its last value decays from the next quantum on
    residNext_ += x;
    n.stage = sOff;
    n.gate = false;
    n.last = 0.0f;
  }
}

// What moves the whole chain once per quantum: the drift (a slow wave and a random
// wander, shared by every note) and the filter's corner while its envelope decays.
// Only while one of them moves (out of the per-sample path: flash on the device).
__attribute__((noinline)) void MelodicSampler::moving() {
  if (driftDepth_ > 0.0f) {
    driftPh_ += driftInc_;
    if (driftPh_ >= 1.0f) {
      driftPh_ -= 1.0f;
      rng_ = rng_ * 1664525u + 1013904223u;
      driftTarget_ = float(int32_t(rng_)) * (1.0f / 2147483648.0f);
    }
    driftWander_ += (driftTarget_ - driftWander_) * (4.0f * driftInc_);
    drift_ = driftDepth_ * (0.6f * sinTurn(driftPh_) + 0.4f * driftWander_);
  } else {
    drift_ = 0.0f;
  }
  const bool envMoving = envOct_ > 0.0f && fenv_ > 1.0e-4f;
  if (filterOn_ && (envMoving || toneDirty_)) {
    float hz = 60.0f * exp2Fast(cutOct_ + envOct_ * fenv_);
    if (hz > 0.45f * fs_) hz = 0.45f * fs_;
    const float x = kPiF * hz * invFs_;
    g_ = drums::sinSmall(x) / drums::cosSmall(x);
    gk_ = g_ + k_;
    d_ = 1.0f / (1.0f + g_ * gk_);
  }
  toneDirty_ = false;
  if (fenv_ > 0.0f) {
    fenv_ *= fdecCoefQ_;
    if (fenv_ < 1.0e-5f) fenv_ = 0.0f;
  }
}

void MelodicSampler::render(float* outL, float* outR, int frames) {
  // ---- control (once per quantum) ----
  const int silent = silentSlot();
  const uint32_t gen = pool_ ? pool_->generation() : 0;
  bool any = false;
  for (int k = 0; k < mel::kMaxNotes; ++k) {
    Note& n = n_[k];
    if (n.stage == sOff) continue;
    if ((!n.builtin && n.gen != gen) || int(n.slot) == silent) killNote(n);   // the slot changed / is being recorded
    any = true;
  }
  if (driftDepth_ > 0.0f || fenv_ > 0.0f || toneDirty_) moving();
  else drift_ = 0.0f;

  // ---- the notes into one sum ----
  float acc[kQuantum];
  for (int i = 0; i < kQuantum; ++i) acc[i] = 0.0f;
  if (any)
    for (int k = 0; k < mel::kMaxNotes; ++k)
      if (n_[k].stage != sOff) renderNote(n_[k], acc);

  // ---- the shared tone chain ----
  float peak = 0.0f;
  float r = resid_;
  const bool resid = r != 0.0f;
  for (int i = 0; i < frames; ++i) {
    float x = acc[i];
    if (resid) {
      r *= residK_;
      x += r;
    }
    if (down_) {
      holdPh_ += holdInc_;
      if (holdPh_ >= 1.0f) {
        holdPh_ -= 1.0f;
        holdVal_ = x;
      }
      x = holdVal_;
    }
    if (crushQ_ > 0.0f) x = (float(int(x * crushInv_ + 65536.5f)) - 65536.0f) * crushQ_;
    if (driveOn_) x = drums::saturate(x * drive_) * driveNorm_;
    if (filterOn_) {
      float hp, bp, lp;
      drums::svfTick(x, g_, gk_, d_, s1_, s2_, hp, bp, lp);
      x = ftype_ == kLP ? lp : (ftype_ == kBP ? bp : hp);
    }
    const float ax = absf(x);
    if (ax > peak) peak = ax;
    outL[i] = x;
  }
  r += residNext_;
  residNext_ = 0.0f;
  // a filter ringing on a crushed (zero) input decays into the subnormal range, which
  // the ESP32-S3's FPU is slow on: flush what is far below hearing
  if (absf(s1_) < 1.0e-15f) s1_ = 0.0f;
  if (absf(s2_) < 1.0e-15f) s2_ = 0.0f;
  resid_ = absf(r) < 1.0e-7f ? 0.0f : r;
  for (int i = 0; i < frames; ++i) outR[i] = outL[i];
  bool sounding = false;
  for (int k = 0; k < mel::kMaxNotes; ++k) sounding |= n_[k].stage != sOff;
  if (!sounding && resid_ == 0.0f && peak < kSilentEnv) {
    active_ = false;
    s1_ = s2_ = 0.0f;
    holdVal_ = 0.0f;
  }
  publish();
}

void MelodicSampler::publish() {
  for (int k = 0; k < mel::kMaxNotes; ++k) {
    const Note& n = n_[k];
    const int32_t v = n.stage != sOff ? int32_t((uint32_t(n.slot) << 24) | (uint32_t(n.pos >> 32) & 0xFFFFFFu)) : -1;
    playhead_[k].store(v, std::memory_order_relaxed);
  }
}

MelodicSampler::NoteView MelodicSampler::view(int k) const {
  NoteView v{};
  if (k < 0 || k >= mel::kMaxNotes) return v;
  const Note& n = n_[k];
  v.on = n.stage != sOff;
  v.gate = n.gate;
  v.note = n.note;
  v.slot = n.slot;
  v.semis = n.semis;
  v.env = n.env;
  v.pos = int32_t(n.pos >> 32);
  v.dir = n.step < 0 ? -1 : 1;
  return v;
}

int MelodicSampler::soundingNotes() const {
  int c = 0;
  for (int k = 0; k < mel::kMaxNotes; ++k) c += n_[k].stage != sOff ? 1 : 0;
  return c;
}

}  // namespace sampler
}  // namespace pg
