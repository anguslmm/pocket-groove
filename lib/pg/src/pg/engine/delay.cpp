#include "delay.h"

namespace pg {
namespace engine_detail {

void Delay::init(float halfRate) {
  fadeInc_ = 1.0f / (0.025f * halfRate);
  reset();
  d_ = dNew_ = dPending_ = int(0.125f * halfRate);
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
  lp_ = hp_ = 0.0f;
  zeroRun_ = kBufLen;
}

int Delay::setTime(float halfRateSamples) {
  const float maxD = float((kBufLen - 2 * kMaxBlock - 4) / 2);
  float d = halfRateSamples;
  int guard = 0;
  while (d > maxD && guard++ < 16) d *= 0.5f;
  int di = int(d + 0.5f);
  if (di < kMaxBlock + 1) di = kMaxBlock + 1;
  dPending_ = di;
  if (!fading_ && di != d_) {
    if (idle()) {
      d_ = dNew_ = di;      // nothing in the line: no need to fade
    } else {
      dNew_ = di;
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
  for (int i = 0; i < n; ++i) {
    dst[i] = float(buf_[r]);
    if (++r == kBufLen) r = 0;
  }
}

template <bool kMono>
void Delay::run(const float* in, float* outL, float* outR, int n) {
  float t1[kMaxBlock], t2[kMaxBlock];
  readTap(t1, d_, n);
  readTap(t2, 2 * d_, n);
  if (fading_) {
    float u1[kMaxBlock], u2[kMaxBlock];
    readTap(u1, dNew_, n);
    readTap(u2, 2 * dNew_, n);
    float f = fade_;
    for (int i = 0; i < n; ++i) {
      f += fadeInc_;
      if (f > 1.0f) f = 1.0f;
      t1[i] += f * (u1[i] - t1[i]);
      t2[i] += f * (u2[i] - t2[i]);
    }
    fade_ = f;
    if (f >= 1.0f) {
      d_ = dNew_;
      fading_ = false;
      if (dPending_ != d_) {
        dNew_ = dPending_;
        fading_ = true;
        fade_ = 0.0f;
      }
    }
  }

  const float fb = fb_, fb2 = fb_ * fb_;
  // Width law: near = 1 + w, far = 1 - w, so near + far is constant. The
  // device's codec is mono and plays (L+R)/2: with this law the mono sum is
  // exactly the plain delay at ANY width (nothing cancels, no level is lost).
  // In stereo a full ping-pong echo is +3 dB in total power over width 0.
  const float near = 1.0f + width_, far = 1.0f - width_;
  const float inv = 1.0f / kScale;
  float lp = lp_, hp = hp_;
  int w = w_;
  int nonZero = 0;
  for (int i = 0; i < n; ++i) {
    const float x = in[i] * kScale + fb2 * t2[i];
    lp += lpK_ * (x - lp);
    hp += hpK_ * (lp - hp);
    const int16_t q = toI16Trunc(lp - hp);
    buf_[w] = q;
    nonZero |= q;
    if (++w == kBufLen) w = 0;
    const float e1 = t1[i] * inv;
    const float e2 = t2[i] * (fb * inv);
    if (kMono) {
      outL[i] += e1 + e2;      // (L + R) / 2 of the stereo taps, since near + far = 2
    } else {
      outL[i] += near * e1 + far * e2;
      outR[i] += far * e1 + near * e2;
    }
  }
  w_ = w;
  if (nonZero) {
    zeroRun_ = 0;
  } else if (zeroRun_ < kBufLen) {
    zeroRun_ += n;
    if (zeroRun_ >= kBufLen) { lp = 0.0f; hp = 0.0f; }
  }
  lp_ = lp;
  hp_ = hp;
}

template void Delay::run<false>(const float*, float*, float*, int);
template void Delay::run<true>(const float*, float*, float*, int);

}  // namespace engine_detail
}  // namespace pg
