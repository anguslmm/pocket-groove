#include "master.h"

#include <cmath>

namespace pg {
namespace engine_detail {

void MasterBus::init(float sampleRate, int quantum) {
  sr_ = sampleRate;
  invSr_ = 1.0f / sampleRate;
  const float quantaPerSec = sampleRate / float(quantum);
  smoothK_ = 1.0f - fastExp(-1.0f / (0.012f * quantaPerSec));   // ~12 ms knob smoothing
  mixStep_ = 1.0f / (0.004f * quantaPerSec);                    // 4 ms bypass cross-fade
  gainK_ = 1.0f - fastExp(-1.0f / (0.003f * quantaPerSec));     // ~3 ms gain smoothing
  // Filter range (init is not real-time, libm is fine here).
  top_ = 0.40f * sampleRate < 18000.0f ? 0.40f * sampleRate : 18000.0f;
  lpLog2_ = log2f(90.0f / top_);
  const float hpTop = top_ < 8000.0f ? top_ : 8000.0f;
  hpLog2_ = log2f(hpTop / 25.0f);
  // Rumble high-pass: 20 Hz, Q = 0.7071 (Butterworth).
  hpG_ = tanf(3.14159265f * 20.0f / sampleRate);
  hpGk_ = hpG_ + 1.41421356f;
  hpD_ = 1.0f / (1.0f + hpG_ * hpGk_);
  reset();
}

void MasterBus::reset() {
  mode_ = kBypass;
  pos_ = posTarget_ = 64.0f;
  mix_ = mixTarget_ = 0.0f;
  resoK_ = 1.4142f;
  a1_ = a2_ = a3_ = 0.0f;
  ic1_[0] = ic1_[1] = ic2_[0] = ic2_[1] = 0.0f;
  hp1_[0] = hp1_[1] = hp2_[0] = hp2_[1] = 0.0f;
  pre_ = preTarget_ = 1.0f;
  c3_ = c3Target_ = 0.0f;
  post_ = postTarget_ = 1.0f;
  driveGain_ = volGain_ = 1.0f;
  lastFilter_ = lastReso_ = lastDrive_ = lastVolume_ = -1;
  coefDirty_ = true;
  faults_ = 0;
}

void MasterBus::updateFilterCoefs() {
  // Knob position -> cutoff, exponential both ways.
  //   low-pass side:  63 -> top of band .. 0 -> 90 Hz
  //   high-pass side: 65 -> 25 Hz .. 127 -> 8 kHz
  float fc;
  if (mode_ == kLowPass) {
    const float t = clampf((63.0f - pos_) * (1.0f / 63.0f), 0.0f, 1.0f);
    fc = top_ * fastPow2(lpLog2_ * t);
  } else {
    const float t = clampf((pos_ - 65.0f) * (1.0f / 62.0f), 0.0f, 1.0f);
    fc = 25.0f * fastPow2(hpLog2_ * t);
  }
  const float g = fastTan(3.14159265f * clampf(fc * invSr_, 0.0002f, 0.40f));
  a1_ = 1.0f / (1.0f + g * (g + resoK_));
  a2_ = g * a1_;
  a3_ = g * a2_;
  coefDirty_ = false;
}

void MasterBus::setParams(int filter, int reso, int drive, int volume) {
  if (filter != lastFilter_) {
    lastFilter_ = filter;
    posTarget_ = float(clampi(filter, 0, 127));
  }
  if (reso != lastReso_) {
    lastReso_ = reso;
    // Q from 0.707 to 6 (exponential): 1/Q = 1.4142 * 2^(-3.085 * r)
    resoK_ = 1.4142f * fastPow2(-3.085f * clampf(float(reso) * (1.0f / 127.0f), 0.0f, 1.0f));
    coefDirty_ = true;
  }
  if (drive != lastDrive_) {
    lastDrive_ = drive;
    const float d = clampf(float(drive) * (1.0f / 127.0f), 0.0f, 1.0f);
    const float pre = 1.0f + 7.0f * d * d;
    // 1/sqrt(pre) by Newton (pre in 1..8), control rate only.
    float y = 1.0f / (0.5f * (1.0f + pre));
    y = y * (1.5f - 0.5f * pre * y * y);
    y = y * (1.5f - 0.5f * pre * y * y);
    y = y * (1.5f - 0.5f * pre * y * y);
    y = y * (1.5f - 0.5f * pre * y * y);
    driveGain_ = y;
    preTarget_ = pre;
    c3Target_ = (1.0f / 6.75f) * clampf(d * 8.0f, 0.0f, 1.0f);
    postTarget_ = driveGain_ * volGain_;
  }
  if (volume != lastVolume_) {
    lastVolume_ = volume;
    const float v = clampf(float(volume) / 100.0f, 0.0f, 1.27f);   // exact 1.0 at 100
    volGain_ = v * v * v;
    postTarget_ = driveGain_ * volGain_;
  }

  // ---- DJ filter state machine (one step per quantum) ----
  const Mode want = posTarget_ < 63.5f ? kLowPass : (posTarget_ > 64.5f ? kHighPass : kBypass);
  if (mode_ == kBypass) {
    if (want != kBypass && mix_ <= 0.0f) {
      mode_ = want;                       // start the new mode at its target, states clean
      pos_ = posTarget_;
      ic1_[0] = ic1_[1] = ic2_[0] = ic2_[1] = 0.0f;
      mixTarget_ = 1.0f;
      updateFilterCoefs();
    }
  } else if (want != mode_) {
    mixTarget_ = 0.0f;                    // fade to dry first; mode changes once dry
    if (mix_ <= 0.0f) mode_ = kBypass;
  } else {
    mixTarget_ = 1.0f;
    const float d = posTarget_ - pos_;
    if (d > 0.01f || d < -0.01f) {
      pos_ += d * smoothK_;
      updateFilterCoefs();
    } else if (pos_ != posTarget_) {
      pos_ = posTarget_;
      updateFilterCoefs();
    } else if (coefDirty_) {
      updateFilterCoefs();
    }
  }
}

void MasterBus::rumble(float* x, int c, int frames) {
  if (rumbleOff_) return;
  float s1 = hp1_[c], s2 = hp2_[c];
  const float g = hpG_, gk = hpGk_, d = hpD_;
  for (int i = 0; i < frames; ++i) {
    const float hp = (x[i] - gk * s1 - s2) * d;
    const float v1 = g * hp;
    const float bp = v1 + s1;
    s1 = bp + v1;
    const float v2 = g * bp;
    s2 += 2.0f * v2;              // lp = v2 + s2; s2 = lp + v2
    x[i] = hp;
  }
  // Flush tiny states (silence must not decay into denormals) and garbage.
  if (!(absf(s1) > 1.0e-20f) || !(absf(s1) < 1.0e6f)) s1 = 0.0f;
  if (!(absf(s2) > 1.0e-20f) || !(absf(s2) < 1.0e6f)) s2 = 0.0f;
  hp1_[c] = s1;
  hp2_[c] = s2;
}

template <bool kMono>
void MasterBus::run(float* l, float* r, int16_t* out, int frames, float* peakL, float* peakR) {
  // ---- rumble / DC high-pass (always on) ----
  rumble(l, 0, frames);
  if (!kMono) rumble(r, 1, frames);

  // ---- DJ filter (skipped entirely in bypass) ----
  if (mode_ != kBypass || mix_ > 0.0f) {
    float mixEnd = mix_;
    if (mixEnd < mixTarget_) { mixEnd += mixStep_; if (mixEnd > mixTarget_) mixEnd = mixTarget_; }
    else if (mixEnd > mixTarget_) { mixEnd -= mixStep_; if (mixEnd < mixTarget_) mixEnd = mixTarget_; }
    const float mixInc = (mixEnd - mix_) / float(frames);
    const bool hp = mode_ == kHighPass;
    float* ch[2] = {l, r};
    for (int c = 0; c < (kMono ? 1 : 2); ++c) {
      float ic1 = ic1_[c], ic2 = ic2_[c];
      float m = mix_;
      float* x = ch[c];
      const float a1 = a1_, a2 = a2_, a3 = a3_, k = resoK_;
      if (hp) {
        for (int i = 0; i < frames; ++i) {
          const float v0 = x[i];
          const float v3 = v0 - ic2;
          const float v1 = a1 * ic1 + a2 * v3;
          const float v2 = ic2 + a2 * ic1 + a3 * v3;
          ic1 = 2.0f * v1 - ic1;
          ic2 = 2.0f * v2 - ic2;
          m += mixInc;
          x[i] = v0 + m * ((v0 - k * v1 - v2) - v0);
        }
      } else {
        for (int i = 0; i < frames; ++i) {
          const float v0 = x[i];
          const float v3 = v0 - ic2;
          const float v1 = a1 * ic1 + a2 * v3;
          const float v2 = ic2 + a2 * ic1 + a3 * v3;
          ic1 = 2.0f * v1 - ic1;
          ic2 = 2.0f * v2 - ic2;
          m += mixInc;
          x[i] = v0 + m * (v2 - v0);
        }
      }
      // State hygiene: a resonant filter fed garbage must not stay broken.
      if (!(absf(ic1) < 1.0e6f) || !(absf(ic2) < 1.0e6f)) { ic1 = 0.0f; ic2 = 0.0f; ++faults_; }
      ic1_[c] = ic1;
      ic2_[c] = ic2;
    }
    mix_ = mixEnd;
  }

  // ---- drive, volume, limiter, int16 ----
  const float knee = kKnee;
  const float invRange = 1.0f / (1.0f - kKnee);
  const float range = 1.0f - kKnee;
  if (pre_ == 1.0f && preTarget_ == 1.0f && c3_ == 0.0f && c3Target_ == 0.0f && post_ == postTarget_) {
    // Fast path (drive off, volume settled: the usual case): clamp, gain,
    // and the limiter, which is the identity below the knee. The same maths
    // as the general path below with pre = 1, c3 = 0, bit for bit.
    const float post = post_;
    float pkL = *peakL, pkR = *peakR;
    if (kMono) {
      for (int i = 0; i < frames; ++i) {
        float ul = l[i];
        if (!(ul > -1.5f)) ul = -1.5f;
        if (!(ul < 1.5f)) ul = 1.5f;
        const float yl = ul * post;
        float al = absf(yl);
        if (al > knee) { float t = (al - knee) * invRange; t = t > 2.0f ? 2.0f : t; al = knee + range * (t - 0.25f * t * t); }
        if (al > pkL) pkL = al;
        const int32_t il = int32_t(al * 32767.0f + 0.5f);
        out[2 * i] = out[2 * i + 1] = int16_t(yl < 0.0f ? -il : il);
      }
      *peakL = pkL;
      return;
    }
    for (int i = 0; i < frames; ++i) {
      float ul = l[i], ur = r[i];
      if (!(ul > -1.5f)) ul = -1.5f;
      if (!(ul < 1.5f)) ul = 1.5f;
      if (!(ur > -1.5f)) ur = -1.5f;
      if (!(ur < 1.5f)) ur = 1.5f;
      const float yl = ul * post, yr = ur * post;
      float al = absf(yl), ar = absf(yr);
      if (al > knee) { float t = (al - knee) * invRange; t = t > 2.0f ? 2.0f : t; al = knee + range * (t - 0.25f * t * t); }
      if (ar > knee) { float t = (ar - knee) * invRange; t = t > 2.0f ? 2.0f : t; ar = knee + range * (t - 0.25f * t * t); }
      if (al > pkL) pkL = al;
      if (ar > pkR) pkR = ar;
      const int32_t il = int32_t(al * 32767.0f + 0.5f);
      const int32_t ir = int32_t(ar * 32767.0f + 0.5f);
      out[2 * i] = int16_t(yl < 0.0f ? -il : il);
      out[2 * i + 1] = int16_t(yr < 0.0f ? -ir : ir);
    }
    *peakL = pkL;
    *peakR = pkR;
    return;
  }
  if (kMono && pre_ == preTarget_ && c3_ == c3Target_ && post_ == postTarget_) {
    // Drive on but settled: the general path below with zero ramps, bit for bit.
    const float pre = pre_, c3 = c3_, post = post_;
    float pkL = *peakL;
    for (int i = 0; i < frames; ++i) {
      float ul = l[i] * pre;
      if (!(ul > -1.5f)) ul = -1.5f;
      if (!(ul < 1.5f)) ul = 1.5f;
      const float yl = post * (ul - c3 * ul * ul * ul);
      float al = absf(yl);
      float tl = (al - knee) * invRange;
      tl = tl < 0.0f ? 0.0f : (tl > 2.0f ? 2.0f : tl);
      al = (al < knee ? al : knee) + range * (tl - 0.25f * tl * tl);
      if (al > pkL) pkL = al;
      const int32_t il = int32_t(al * 32767.0f + 0.5f);
      out[2 * i] = out[2 * i + 1] = int16_t(yl < 0.0f ? -il : il);
    }
    *peakL = pkL;
    return;
  }
  const float invN = 1.0f / float(frames);
  // Gains are smoothed (one-pole per quantum) and then ramped linearly inside it.
  const float preEnd = pre_ + (preTarget_ - pre_) * gainK_;
  const float c3End = c3_ + (c3Target_ - c3_) * gainK_;
  const float postEnd = post_ + (postTarget_ - post_) * gainK_;
  const float preInc = (preEnd - pre_) * invN;
  const float c3Inc = (c3End - c3_) * invN;
  const float postInc = (postEnd - post_) * invN;
  float pre = pre_, c3 = c3_, post = post_;
  float pkL = *peakL, pkR = *peakR;
  if (kMono) {
    for (int i = 0; i < frames; ++i) {
      pre += preInc; c3 += c3Inc; post += postInc;
      float ul = l[i] * pre;
      if (!(ul > -1.5f)) ul = -1.5f;
      if (!(ul < 1.5f)) ul = 1.5f;
      const float yl = post * (ul - c3 * ul * ul * ul);
      float al = absf(yl);
      float tl = (al - knee) * invRange;
      tl = tl < 0.0f ? 0.0f : (tl > 2.0f ? 2.0f : tl);
      al = (al < knee ? al : knee) + range * (tl - 0.25f * tl * tl);
      if (al > pkL) pkL = al;
      const int32_t il = int32_t(al * 32767.0f + 0.5f);
      out[2 * i] = out[2 * i + 1] = int16_t(yl < 0.0f ? -il : il);
    }
  } else {
    for (int i = 0; i < frames; ++i) {
      pre += preInc; c3 += c3Inc; post += postInc;
      float ul = l[i] * pre, ur = r[i] * pre;
      if (!(ul > -1.5f)) ul = -1.5f;
      if (!(ul < 1.5f)) ul = 1.5f;
      if (!(ur > -1.5f)) ur = -1.5f;
      if (!(ur < 1.5f)) ur = 1.5f;
      const float yl = post * (ul - c3 * ul * ul * ul);
      const float yr = post * (ur - c3 * ur * ur * ur);
      // soft limiter: identity below the knee, quadratic knee, ceiling 1.0
      float al = absf(yl), ar = absf(yr);
      float tl = (al - knee) * invRange, tr = (ar - knee) * invRange;
      tl = tl < 0.0f ? 0.0f : (tl > 2.0f ? 2.0f : tl);
      tr = tr < 0.0f ? 0.0f : (tr > 2.0f ? 2.0f : tr);
      al = (al < knee ? al : knee) + range * (tl - 0.25f * tl * tl);
      ar = (ar < knee ? ar : knee) + range * (tr - 0.25f * tr * tr);
      if (al > pkL) pkL = al;
      if (ar > pkR) pkR = ar;
      const int32_t il = int32_t(al * 32767.0f + 0.5f);
      const int32_t ir = int32_t(ar * 32767.0f + 0.5f);
      out[2 * i] = int16_t(yl < 0.0f ? -il : il);
      out[2 * i + 1] = int16_t(yr < 0.0f ? -ir : ir);
    }
  }
  pre_ = absf(preTarget_ - preEnd) < 1.0e-5f ? preTarget_ : preEnd;
  c3_ = absf(c3Target_ - c3End) < 1.0e-6f ? c3Target_ : c3End;
  post_ = absf(postTarget_ - postEnd) < 1.0e-5f ? postTarget_ : postEnd;
  *peakL = pkL;
  *peakR = pkR;
}

template void MasterBus::run<false>(float*, float*, int16_t*, int, float*, float*);
template void MasterBus::run<true>(float*, float*, int16_t*, int, float*, float*);

}  // namespace engine_detail
}  // namespace pg
