// SamplerVoice (sampler.h). No libm anywhere: every mapping uses the drums'
// polynomial exp2 / sin / cos (drum_dsp.h), so the byte path and the fractional
// path compute the same way and the control-rate updates are cheap.
#include "sampler.h"

#include <cstring>

#include "../drums/drum_dsp.h"

namespace pg {
namespace sampler {

namespace {

using drums::clampf;
using drums::exp2Fast;

constexpr float kInv32768 = 1.0f / 32768.0f;
constexpr float kSilent = 1.0e-4f;
constexpr float kPiF = 3.14159265f;

inline float absf(float x) { return x < 0.0f ? -x : x; }
inline int roundi(float x) { return int(x + 0.5f); }

int32_t minRegion(uint32_t len) {
  return int32_t(len < uint32_t(SamplerVoice::kMinRegion) ? len : uint32_t(SamplerVoice::kMinRegion));
}

// The region's end for LENGTH: from the chop's start sp toward the slice's end sb
// (0 = 10 ms, 127 = all of it, a square law between), at least minRegion after
// the region's start a, never past the sample's end.
int32_t endOf(int32_t sp, int32_t sb, int32_t a, float length, uint32_t len, uint32_t rate) {
  const float rest = float(sb - sp);
  const float minLen = 0.010f * float(rate);
  const float f = clampf(length, 0.0f, 127.0f) * (1.0f / 127.0f);
  const float l = rest > minLen ? minLen + (rest - minLen) * f * f : rest;
  int32_t b = sp + int32_t(l);
  if (b - a < minRegion(len)) b = a + minRegion(len);
  if (b > int32_t(len)) b = int32_t(len);
  return b;
}

// The region from the latched values (floats within their ranges).
void regionOf(float slices, float slice, float start, float length, uint32_t len, uint32_t rate, int32_t* sliceA,
              int32_t* sliceB, int32_t* startPos, int32_t* a, int32_t* b) {
  const int64_t L = int64_t(len);
  const int n = roundi(clampf(slices, 1.0f, 16.0f));
  const int i = roundi(clampf(slice, 0.0f, 15.0f)) % n;
  const int32_t sa = int32_t(L * i / n), sb = int32_t(L * (i + 1) / n);
  const int32_t sp = sa + int32_t(float(sb - sa) * clampf(start, 0.0f, 127.0f) * (1.0f / 128.0f));
  int32_t ra = sp;
  if (int32_t(L) - ra < minRegion(len)) ra = int32_t(L) - minRegion(len);   // too near the end: start earlier
  if (ra < 0) ra = 0;
  if (sliceA) *sliceA = sa;
  if (sliceB) *sliceB = sb;
  if (startPos) *startPos = sp;
  *a = ra;
  *b = endOf(sp, sb, ra, length, len, rate);
}

}  // namespace

SamplerVoice::SamplerVoice() {
  for (int i = 0; i < kNumParams; ++i) {
    raw_[i] = paramInfo(i)->def;
    pf_[i] = float(raw_[i]);
  }
}

void SamplerVoice::init(float sampleRate) {
  fs_ = clampf(sampleRate, 8000.0f, 192000.0f);
  chokeStep_ = 1.0f / (0.002f * fs_);
  residStep_ = 1.0f / (kCutMs * 0.001f * fs_);
  stepSamples_ = fs_ * 15.0f / 120.0f;
  active_ = false;
  stage_ = sOff;
  choking_ = false;
  chokeGain_ = 1.0f;
  resid_ = residRamp_ = lastOut_ = 0.0f;
  s1_ = s2_ = 0.0f;
  holdPh_ = holdVal_ = 0.0f;
  fine_ = false;
  resolve(pool_, slot_, &ref_);
  updateTone();
  playhead_.store(-1, std::memory_order_relaxed);
}

void SamplerVoice::region(const uint8_t* p, uint32_t length, uint32_t rate, int32_t* a, int32_t* b) {
  regionOf(float(p[kSlices]), float(p[kSlice]), float(p[kStart]), float(p[kLength]), length, rate, nullptr, nullptr,
           nullptr, a, b);
}

void SamplerVoice::setParams(const uint8_t* p) {
  if (!fine_ && std::memcmp(p, raw_, kNumParams) == 0) return;
  fine_ = false;
  for (int i = 0; i < kNumParams; ++i) {
    const ParamInfo* pi = paramInfo(i);
    raw_[i] = p[i];
    pf_[i] = float(p[i] < pi->min ? pi->min : (p[i] > pi->max ? pi->max : p[i]));
  }
  updateTone();
}

void SamplerVoice::setParamsFine(const float* v) {
  fine_ = true;
  bool changed = false;
  for (int i = 0; i < kNumParams; ++i) {
    const ParamInfo* pi = paramInfo(i);
    const float x = clampf(v[i], float(pi->min), float(pi->max));
    if (x != pf_[i]) {
      pf_[i] = x;
      raw_[i] = uint8_t(roundi(x));
      changed = true;
    }
  }
  if (changed) updateTone();
}

void SamplerVoice::setTempo(float bpm) {
  stepSamples_ = fs_ * 15.0f / clampf(bpm, 20.0f, 400.0f);
}

// The filter, the crush, the hold, the drive, the release: from pf_ (control rate).
void SamplerVoice::updateTone() {
  const float c = pf_[kCutoff], r = pf_[kReso];
  ftype_ = uint8_t(clampf(float(roundi(pf_[kFilter])), 0.0f, float(kNumFilters - 1)));
  filterOn_ = !(ftype_ == kLP && c >= 126.5f);
  const float hz = clampf(60.0f * exp2Fast(7.45f * c * (1.0f / 127.0f)), 20.0f, 0.45f * fs_);
  const float x = kPiF * hz / fs_;
  g_ = drums::sinSmall(x) / drums::cosSmall(x);
  const float rr = r * (1.0f / 127.0f);
  const float k = 1.0f / (0.6f + 11.0f * rr * rr);
  gk_ = g_ + k;
  d_ = 1.0f / (1.0f + g_ * gk_);

  const float bits = pf_[kBits];
  if (bits > 0.5f) {
    const float nb = 16.0f - 14.0f * bits * (1.0f / 127.0f);
    crushQ_ = exp2Fast(1.0f - nb);
    crushInv_ = 1.0f / crushQ_;
  } else {
    crushQ_ = 0.0f;
  }
  const float dn = pf_[kDown];
  holdInc_ = 22050.0f * exp2Fast(-5.0f * dn * (1.0f / 127.0f)) / fs_;
  down_ = dn > 0.5f && holdInc_ < 1.0f;
  const float dv = pf_[kDrive] * (1.0f / 127.0f);
  driveOn_ = dv > 0.002f;
  drive_ = 1.0f + 15.0f * dv * dv;
  driveNorm_ = 0.9f / drums::saturate(0.9f * drive_);

  const float tau = 0.005f * exp2Fast(8.644f * pf_[kRelease] * (1.0f / 127.0f));   // 5 ms .. 2 s
  relCoef_ = exp2Fast(-1.44269504f / (tau * fs_));
}

// The playback increment from the pitch, the note, the repeats and the tape speed.
void SamplerVoice::updateRate() {
  float semis = note_ + (pf_[kPitch] - 64.0f) + (pf_[kFine] - 64.0f) * (1.0f / 64.0f) + trackTune_ +
                float(repeats_) * (pf_[kRepPitch] - 64.0f) * (12.0f / 64.0f);
  semis = clampf(semis, -48.0f, 48.0f);
  const float speed = (pf_[kSpeed] - 64.0f) * (1.0f / 32.0f);   // -2 .. ~+2
  tapeGain_ = absf(speed) * 4.0f;
  if (tapeGain_ > 1.0f) tapeGain_ = 1.0f;
  const float incf = float(ref_.rate) / fs_ * exp2Fast(semis * (1.0f / 12.0f)) * speed * float(dir_);
  inc_ = int64_t(incf * 4294967296.0f);
}

void SamplerVoice::updateRegion() {
  // LENGTH is continuous: the region's end follows it (the start and the slice stay).
  b_ = endOf(startPos_, sliceB_, a_, pf_[kLength], ref_.length, ref_.rate);
}

void SamplerVoice::startChop(bool repeat) {
  if (!repeat) {
    resolve(pool_, slot_, &ref_);
    gen_ = pool_ ? pool_->generation() : 0;
    chopSlot_ = slot_;
    regionOf(pf_[kSlices], pf_[kSlice], pf_[kStart], pf_[kLength], ref_.length, ref_.rate, &sliceA_, &sliceB_,
             &startPos_, &a_, &b_);
    loop_ = pf_[kLoop] >= 0.5f;
    dir_ = pf_[kReverse] >= 0.5f ? -1 : 1;
    const float atk = 0.001f * exp2Fast(8.644f * pf_[kAttack] * (1.0f / 127.0f));   // 1 ms .. 400 ms
    attackInc_ = 1.0f / (atk * fs_);
    repeats_ = 0;
    repGain_ = 1.0f;
  } else {
    ++repeats_;
    repGain_ *= exp2Fast(-pf_[kRepFade] * (1.0f / 127.0f));   // -6 dB per repeat at 127
  }
  // The chop runs the way inc_ will (REVERSE x the tape's sign): from the region's
  // start forward, or from its end backward (a reversed chop on a backwards tape
  // runs forward: both negatives cancel).
  const int run = (pf_[kSpeed] < 64.0f) ? -dir_ : dir_;
  pos_ = run > 0 ? (int64_t(a_) << 32) : ((int64_t(b_ - 1) << 32) - 1);
  const int rt = roundi(clampf(pf_[kRetrig], 0.0f, float(kNumRetrig - 1)));
  if (rt > 0) {
    float per = kRetrigSteps[rt] * stepSamples_;
    if (per < float(kMinRegion)) per = float(kMinRegion);
    rtLeft_ = int32_t(per);
  } else {
    rtLeft_ = 0;
  }
  if (repeat && !gate_) {
    stage_ = sRelease;   // a repeat after the gate keeps the release going: the repeats die away with it
  } else {
    stage_ = sAttack;
    env_ = 0.0f;
  }
  choking_ = false;
  chokeGain_ = 1.0f;
  residRamp_ = -1.0f;   // flag: the first sample takes the jump (resid = lastOut - y)
  active_ = true;
  updateRate();
}

void SamplerVoice::noteOn(const NoteEvent& e) {
  note_ = float(int(e.note) - 60);
  velGain_ = drums::velocityGain(e.vel) * (e.accent ? 1.2f : 1.0f);
  gate_ = true;
  if (int(slot_) == silent_.load(std::memory_order_relaxed)) {   // being resampled: silent
    rtLeft_ = 0;
    choke();
    return;
  }
  startChop(false);
}

void SamplerVoice::noteOff(uint8_t) {
  gate_ = false;
  if (stage_ == sAttack || stage_ == sHold) stage_ = sRelease;
}

void SamplerVoice::choke() {
  if (active_ && !choking_) {
    choking_ = true;
    chokeGain_ = 1.0f;
  }
  gate_ = false;
}

// The chop's region ended (or its release faded: final). With repeats running it
// waits, silent, for the next one (its envelope keeps releasing meanwhile).
void SamplerVoice::endChop(bool final) {
  stage_ = (!final && rtLeft_ > 0) ? sWait : sOff;
  residRamp_ = -1.0f;   // the end is de-clicked like a cut
}

// The read index left [a_, b_ - 2]: wrap inside the region (LOOP) or end.
bool SamplerVoice::edge() {
  const int32_t span = b_ - 1 - a_;
  if (!loop_ || span < 1) {
    endChop(false);
    return false;
  }
  const int64_t spanF = int64_t(span) << 32, base = int64_t(a_) << 32;
  int64_t rel = (pos_ - base) % spanF;
  if (rel < 0) rel += spanF;
  pos_ = base + rel;
  residRamp_ = -1.0f;
  return true;
}

void SamplerVoice::render(float* outL, float* outR, int frames) {
  if (stage_ != sOff) {
    if (!ref_.builtin && pool_ && pool_->generation() != gen_) choke();   // the slot changed under the chop
    if (rtLeft_ > 0) {   // repeats: while the chop sounds or waits, gated or releasing
      rtLeft_ -= frames;
      if (rtLeft_ <= 0) startChop(true);
    }
    updateRegion();
    updateRate();
    if (int(chopSlot_) == silent_.load(std::memory_order_relaxed)) {   // its slot is being resampled
      rtLeft_ = 0;
      choke();
    }
  }
  const int16_t* data = ref_.data;
  const float gain = velGain_ * repGain_ * tapeGain_ * ref_.gain;   // x 1.0 (exact) but for a mic recording
  float peak = 0.0f;
  for (int n = 0; n < frames; ++n) {
    float x = 0.0f;
    if (stage_ == sWait) {
      if (!gate_) {
        env_ *= relCoef_;
        if (env_ < kSilent) stage_ = sOff;
      }
    } else if (stage_ != sOff) {
      int32_t i = int32_t(pos_ >> 32);
      bool ok = true;
      if (i < a_ || i > b_ - 2) {
        ok = edge();
        i = int32_t(pos_ >> 32);
      }
      if (ok && i >= a_ && i <= b_ - 2) {
        const float fr = float(uint32_t(pos_) >> 8) * (1.0f / 16777216.0f);
        const float s0 = float(data[i]), s1 = float(data[i + 1]);
        x = (s0 + (s1 - s0) * fr) * kInv32768;
        pos_ += inc_;
        if (stage_ == sAttack) {
          env_ += attackInc_;
          if (env_ >= 1.0f) {
            env_ = 1.0f;
            stage_ = gate_ ? sHold : sRelease;
          }
        } else if (stage_ == sRelease) {
          env_ *= relCoef_;
          if (env_ < kSilent) endChop(true);
        }
        x *= env_ * gain;
      } else if (stage_ != sOff && stage_ != sWait) {
        endChop(false);
      }
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
    float y = x * kLevel;
    if (residRamp_ < 0.0f) {   // a jump (cut, wrap, end): continue from the last output
      resid_ = lastOut_ - y;
      residRamp_ = 1.0f;
    }
    if (residRamp_ > 0.0f) {
      y += resid_ * residRamp_ * residRamp_;
      residRamp_ -= residStep_;
      if (residRamp_ < 0.0f) residRamp_ = 0.0f;
    }
    if (choking_) {
      chokeGain_ -= chokeStep_;
      if (chokeGain_ < 0.0f) chokeGain_ = 0.0f;
      y *= chokeGain_;
    }
    lastOut_ = y;
    const float ay = absf(y);
    if (ay > peak) peak = ay;
    outL[n] = y;
  }
  for (int n = 0; n < frames; ++n) outR[n] = outL[n];
  if (choking_ && chokeGain_ <= 0.0f) {
    choking_ = false;
    stage_ = sOff;
    residRamp_ = 0.0f;
    peak = 0.0f;
  }
  if (stage_ == sOff && residRamp_ <= 0.0f && peak < kSilent) {
    active_ = false;
    lastOut_ = 0.0f;
    s1_ = s2_ = 0.0f;
    holdVal_ = 0.0f;
  }
  playhead_.store(stage_ != sOff && stage_ != sWait ? int32_t(pos_ >> 32) : -1, std::memory_order_relaxed);
}

}  // namespace sampler
}  // namespace pg
