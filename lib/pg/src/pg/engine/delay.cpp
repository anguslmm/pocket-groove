#include "delay.h"

namespace pg {
namespace engine_detail {

void Delay::init(float halfRate) {
  fadeInc_ = 1.0f / (0.025f * halfRate);
  mono_ = false;
  reset();
  d_ = dNew_ = dPending_ = int(0.125f * halfRate);
  pp_ = ppNew_ = fitsPingPong(d_);
  fb_ = 0.4f;
  lpK_ = 0.5f;
  hpK_ = 0.02f;
  width_ = 1.0f;
}

void Delay::reset() {
  std::memset(buf_, 0, sizeof(buf_));
  w_ = 0;
  fading_ = false;
  fade_ = 0.0f;
  lp_ = hp_ = lp2_ = hp2_ = 0.0f;
  zeroRun_ = kBufLen;
}

int Delay::setTime(float halfRateSamples) {
  const float maxD = float(kBufLen - kMaxBlock - 4);
  float d = halfRateSamples;
  int guard = 0;
  while (d > maxD && guard++ < 16) d *= 0.5f;
  int di = int(d + 0.5f);
  if (di < kMaxBlock + 1) di = kMaxBlock + 1;
  dPending_ = di;
  const bool pp = fitsPingPong(di);
  if (!fading_ && (di != d_ || pp != pp_)) {
    if (idle()) {
      d_ = dNew_ = di;      // nothing in the line: no need to fade
      pp_ = ppNew_ = pp;
    } else {
      dNew_ = di;
      ppNew_ = pp;
      fading_ = true;
      fade_ = 0.0f;
    }
  }
  return di;
}

void Delay::setParams(float fb, float lpCoef, float hpCoef, float width) {
  fb_ = clampf(fb, 0.0f, 0.95f);
  lpK_ = clampf(lpCoef, 0.001f, 1.0f);
  hpK_ = clampf(hpCoef, 0.0f, 0.5f);
  width_ = clampf(width, 0.0f, 1.0f);
}

void Delay::readTap(float* dst, int delay, int n) const {
  int r = w_ - delay;
  if (r < 0) r += kBufLen;
  if (r + n <= kBufLen) {
    const int16_t* b = buf_ + r;
    for (int i = 0; i < n; ++i) dst[i] = float(b[i]);
  } else {
    for (int i = 0; i < n; ++i) {
      dst[i] = float(buf_[r]);
      if (++r == kBufLen) r = 0;
    }
  }
}

template <bool kMono>
void Delay::run(const float* in, float* outL, float* outR, int n) {
  // Width law (ping-pong): near = 1 + w, far = 1 - w, near + far = 2, so the
  // mono sum (L+R)/2 is O + E at ANY width: the plain delay, nothing cancels.
  const float fb = fb_, lpK = lpK_, hpK = hpK_;
  const float near = 1.0f + width_, far = 1.0f - width_;
  const float inv = 1.0f / kScale;
  float lp = lp_, hp = hp_, lp2 = lp2_, hp2 = hp2_;
  float x[kMaxBlock];
  float t1[kMaxBlock], t2[kMaxBlock];
  readTap(t1, d_, n);

  if (!fading_ && !pp_) {
    // PLAIN (always in mono device mode): the cheap path.
    for (int i = 0; i < n; ++i) {
      x[i] = in[i] * kScale + fb * t1[i];
      if (kMono) {
        outL[i] += t1[i] * inv;
      } else {
        const float o = t1[i] * inv;
        outL[i] += o;
        outR[i] += o;
      }
    }
    lp2 = hp2 = 0.0f;   // F2 unused: start clean if ping-pong resumes
  } else if (!fading_) {
    // PING-PONG, steady.
    readTap(t2, 2 * d_, n);
    for (int i = 0; i < n; ++i) {
      lp2 += lpK * (t2[i] - lp2);
      hp2 += hpK * (lp2 - hp2);
      const float e = fb * (lp2 - hp2);
      x[i] = in[i] * kScale + fb * e;
      const float o1 = t1[i] * inv, e1 = e * inv;
      if (kMono) {
        outL[i] += o1 + e1;
      } else {
        outL[i] += near * o1 + far * e1;
        outR[i] += far * o1 + near * e1;
      }
    }
  } else {
    // Cross-fade from (d_, pp_) to (dNew_, ppNew_): taps, outputs and the
    // feedback into the line all move together. 25 ms; rare.
    float u1[kMaxBlock], u2[kMaxBlock];
    readTap(u1, dNew_, n);
    if (pp_) readTap(t2, 2 * d_, n);
    if (ppNew_) readTap(u2, 2 * dNew_, n);
    const bool ppA = pp_, ppB = ppNew_;
    float f = fade_;
    for (int i = 0; i < n; ++i) {
      f += fadeInc_;
      if (f > 1.0f) f = 1.0f;
      // F2 runs on the 2D tap of whichever configuration is ping-pong
      // (cross-faded when both are).
      float e = 0.0f;
      if (ppA || ppB) {
        const float t = ppA && ppB ? t2[i] + f * (u2[i] - t2[i]) : (ppA ? t2[i] : u2[i]);
        lp2 += lpK * (t - lp2);
        hp2 += hpK * (lp2 - hp2);
        e = fb * (lp2 - hp2);
      }
      const float eA = ppA ? e : 0.0f, eB = ppB ? e : 0.0f;
      // feedback: PLAIN feeds back the whole tap, PING-PONG only the even echoes
      const float fbA = fb * (ppA ? eA : t1[i]), fbB = fb * (ppB ? eB : u1[i]);
      x[i] = in[i] * kScale + fbA + f * (fbB - fbA);
      if (kMono) {
        const float mA = t1[i] + eA, mB = u1[i] + eB;
        outL[i] += (mA + f * (mB - mA)) * inv;
      } else {
        const float nA = ppA ? near : 1.0f, fA = ppA ? far : 1.0f;
        const float nB = ppB ? near : 1.0f, fB = ppB ? far : 1.0f;
        const float lA = nA * t1[i] + fA * eA, rA = fA * t1[i] + nA * eA;
        const float lB = nB * u1[i] + fB * eB, rB = fB * u1[i] + nB * eB;
        outL[i] += (lA + f * (lB - lA)) * inv;
        outR[i] += (rA + f * (rB - rA)) * inv;
      }
    }
    fade_ = f;
    if (f >= 1.0f) {
      d_ = dNew_;
      pp_ = ppNew_;
      fading_ = false;
      const bool ppPend = fitsPingPong(dPending_);
      if (dPending_ != d_ || ppPend != pp_) {
        dNew_ = dPending_;
        ppNew_ = ppPend;
        fading_ = true;
        fade_ = 0.0f;
      }
    }
  }

  // Write: x through the tone filter F into the line.
  int w = w_;
  int nonZero = 0;
  for (int i = 0; i < n; ++i) {
    lp += lpK * (x[i] - lp);
    hp += hpK * (lp - hp);
    // Round to nearest above 1 LSB (no truncation bias building up over the
    // echoes), toward zero at or below it so the line still decays to exact 0.
    float v = lp - hp;
    if (!(v > -32767.0f)) v = -32767.0f;   // NaN saturates too
    if (!(v < 32767.0f)) v = 32767.0f;
    const int16_t q = int16_t((v > 1.0f || v < -1.0f) ? int32_t(v + 32768.5f) - 32768 : int32_t(v));
    buf_[w] = q;
    nonZero |= q;
    if (++w == kBufLen) w = 0;
  }
  w_ = w;
  if (nonZero) {
    zeroRun_ = 0;
  } else if (zeroRun_ < kBufLen) {
    zeroRun_ += n;
    if (zeroRun_ >= kBufLen) { lp = hp = lp2 = hp2 = 0.0f; }
  }
  lp_ = lp;
  hp_ = hp;
  lp2_ = lp2;
  hp2_ = hp2;
}

template void Delay::run<false>(const float*, float*, float*, int);
template void Delay::run<true>(const float*, float*, float*, int);

}  // namespace engine_detail
}  // namespace pg
