#include "reverb.h"

namespace pg {
namespace engine_detail {
namespace {

// Lengths in samples at 22.05 kHz (44.1 kHz engine rate); scaled for other rates.
const int kApLen[Reverb::kNumAp] = {37, 53, 89, 139, 211, 307};
const int kLineLen[Reverb::kNumLines] = {887, 1039, 1213, 1409, 1627, 1877, 2141, 2423};
// Output tap positions as a fraction of each line, and their signs.
const float kTapFracL[Reverb::kNumLines] = {0.11f, 0.78f, 0.37f, 0.29f, 0.21f, 0.93f, 0.55f, 0.07f};
const float kTapFracR[Reverb::kNumLines] = {0.71f, 0.13f, 0.31f, 0.43f, 0.89f, 0.19f, 0.09f, 0.61f};
const float kTapSignL[Reverb::kNumLines] = {1, 1, -1, -1, 1, 1, -1, -1};
const float kTapSignR[Reverb::kNumLines] = {1, 1, -1, -1, 1, 1, -1, -1};
const float kInSign[Reverb::kNumLines] = {1, -1, 1, -1, 1, -1, 1, -1};
const float kApGain = 0.62f;
const float kSideWidth = 0.75f;   // side tap set gain relative to mid

bool isPrime(int n) {
  if (n < 4) return n > 1;
  if ((n & 1) == 0) return false;
  for (int d = 3; d * d <= n; d += 2)
    if (n % d == 0) return false;
  return true;
}
int nextPrime(int n) {
  while (!isPrime(n)) ++n;
  return n;
}

}  // namespace

void Reverb::init(float halfRate) {
  halfRate_ = halfRate;
  float scale = halfRate / 22050.0f;
  if (scale > 1.0f) scale = 1.0f;          // pool is sized for 44.1 kHz
  if (scale < 0.25f) scale = 0.25f;
  int off = 0;
  for (int i = 0; i < kNumAp; ++i) {
    int len = nextPrime(int(float(kApLen[i]) * scale + 0.5f));
    if (len < kMaxBlock + 3) len = nextPrime(kMaxBlock + 3);
    ap_[i].buf = pool_ + off;
    ap_[i].len = len;
    ap_[i].pos = 0;
    off += len;
  }
  int longest = 0;
  for (int i = 0; i < kNumLines; ++i) {
    int len = nextPrime(int(float(kLineLen[i]) * scale + 0.5f));
    while (off + len > kPoolLen && len > 64) len = nextPrime(len / 2);
    dl_[i].buf = pool_ + off;
    dl_[i].len = len;
    dl_[i].pos = 0;
    off += len;
    if (len > longest) longest = len;
    tapL_[i] = clampi(int(kTapFracL[i] * float(len)), kMaxBlock + 1, len - 1);
    tapR_[i] = clampi(int(kTapFracR[i] * float(len)), kMaxBlock + 1, len - 1);
  }
  used_ = off;
  idleAfter_ = longest + 64;
  hpK_ = onePoleCoef(140.0f, halfRate);
  reset();
  setParams(1.2f, 0.6f);
}

void Reverb::reset() {
  std::memset(pool_, 0, sizeof(pool_));
  for (int i = 0; i < kNumLines; ++i) { lp_[i] = 0.0f; err_[i] = 0.0f; }
  hp_ = 0.0f;
  zeroRun_ = idleAfter_;
}

void Reverb::setParams(float rt60, float dampCoef) {
  rt60 = clampf(rt60, 0.1f, 20.0f);
  damp_ = clampf(dampCoef, 0.02f, 1.0f);
  // g = 10^(-3 * len / (rt60 * fs)) per line, so every mode decays at the same rate.
  const float k = -3.0f * 3.3219281f / (rt60 * halfRate_);
  for (int i = 0; i < kNumLines; ++i) g_[i] = fastPow2(k * float(dl_[i].len));
  // Longer tails integrate more energy: trim the return a little as RT grows
  // (fourth root) so the send level means roughly the same thing at any size.
  float r = 1.2f / rt60;                  // 1.0 at the default-ish 1.2 s
  // r^(1/4) via two Newton-free halvings: sqrt(sqrt(r)) through fastPow2/log is
  // overkill; a 2-step approximation is fine for a level trim.
  float s = 0.5f * (1.0f + r);            // crude sqrt seeds, refined below
  s = 0.5f * (s + r / s);
  s = 0.5f * (s + r / s);                 // ~sqrt(r)
  float q = 0.5f * (1.0f + s);
  q = 0.5f * (q + s / q);
  q = 0.5f * (q + s / q);                 // ~sqrt(sqrt(r))
  outGain_ = 0.42f * clampf(q, 0.5f, 1.6f) / kScale;
}

void Reverb::process(const float* in, float* outL, float* outR, int n) {
  float s[kMaxBlock];
  // Input: scale to int16 units, high-pass (keep kick/bass mud out of the tank).
  {
    float hp = hp_;
    for (int i = 0; i < n; ++i) {
      const float x = in[i] * kScale;
      hp += hpK_ * (x - hp);
      s[i] = x - hp;
    }
    hp_ = hp;
  }
  int nonZero = 0;
  // Series allpass diffusers.
  for (int a = 0; a < kNumAp; ++a) {
    Line& l = ap_[a];
    int p = l.pos;
    int16_t* buf = l.buf;
    const int len = l.len;
    if (p + n < len) {
      // No wrap inside this block (the common case): straight pointer walk.
      int16_t* b = buf + p;
      for (int i = 0; i < n; ++i) {
        const float d = float(b[i]);
        const float w = s[i] - kApGain * d;
        s[i] = d + kApGain * w;
        const int16_t q = toI16Trunc(w);
        b[i] = q;
        nonZero |= q;
      }
      p += n;
    } else {
      for (int i = 0; i < n; ++i) {
        const float d = float(buf[p]);
        const float w = s[i] - kApGain * d;
        s[i] = d + kApGain * w;
        const int16_t q = toI16Trunc(w);
        buf[p] = q;
        nonZero |= q;
        if (++p == len) p = 0;
      }
    }
    l.pos = p;
  }

  // FDN: read all lines (and the output taps) for the block.
  float f[kNumLines][kMaxBlock];
  float oL[kMaxBlock], oR[kMaxBlock];
  for (int i = 0; i < n; ++i) { oL[i] = 0.0f; oR[i] = 0.0f; }
  for (int j = 0; j < kNumLines; ++j) {
    const Line& l = dl_[j];
    const int16_t* buf = l.buf;
    const int len = l.len;
    int p = l.pos;
    int pl = p - tapL_[j]; if (pl < 0) pl += len;
    int pr = p - tapR_[j]; if (pr < 0) pr += len;
    float lp = lp_[j];
    const float g = g_[j], k = damp_;
    const float sl = kTapSignL[j], sr = kTapSignR[j];
    if (p + n <= len && pl + n <= len && pr + n <= len) {
      // No wrap inside this block for any of the three read heads (~98 %).
      const int16_t *bp = buf + p, *bl = buf + pl, *br = buf + pr;
      for (int i = 0; i < n; ++i) {
        lp += k * (float(bp[i]) - lp);
        f[j][i] = lp * g;
        oL[i] += sl * float(bl[i]);
        oR[i] += sr * float(br[i]);
      }
    } else {
      for (int i = 0; i < n; ++i) {
        lp += k * (float(buf[p]) - lp);
        f[j][i] = lp * g;
        oL[i] += sl * float(buf[pl]);
        oR[i] += sr * float(buf[pr]);
        if (++p == len) p = 0;
        if (++pl == len) pl = 0;
        if (++pr == len) pr = 0;
      }
    }
    lp_[j] = lp;
  }
  // Mid/side output from two independent tap sets: L = M + w*S, R = M - w*S.
  // The device plays (L+R)/2 = M exactly: a full, uncoloured tap sum at the
  // calibrated level, whatever the side does (two decorrelated taps summed
  // directly would lose ~3 dB and comb at low frequencies). In stereo the
  // side adds width (L/R correlation (1-w^2)/(1+w^2) = 0.28) and +1.9 dB.
  {
    const float gm = outGain_, gs = kSideWidth * outGain_;
    for (int i = 0; i < n; ++i) {
      const float m = oL[i] * gm, sd = oR[i] * gs;
      outL[i] += m + sd;
      outR[i] += m - sd;
    }
  }

  // 8x8 Hadamard (fast Walsh-Hadamard), scaled by 1/sqrt(8), plus input injection.
  const float norm = 0.35355339f;
  for (int i = 0; i < n; ++i) {
    const float a0 = f[0][i] + f[1][i], a1 = f[0][i] - f[1][i];
    const float a2 = f[2][i] + f[3][i], a3 = f[2][i] - f[3][i];
    const float a4 = f[4][i] + f[5][i], a5 = f[4][i] - f[5][i];
    const float a6 = f[6][i] + f[7][i], a7 = f[6][i] - f[7][i];
    const float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
    const float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
    const float x = s[i] * 0.5f;
    f[0][i] = (b0 + b4) * norm + x * kInSign[0];
    f[1][i] = (b1 + b5) * norm + x * kInSign[1];
    f[2][i] = (b2 + b6) * norm + x * kInSign[2];
    f[3][i] = (b3 + b7) * norm + x * kInSign[3];
    f[4][i] = (b0 - b4) * norm + x * kInSign[4];
    f[5][i] = (b1 - b5) * norm + x * kInSign[5];
    f[6][i] = (b2 - b6) * norm + x * kInSign[6];
    f[7][i] = (b3 - b7) * norm + x * kInSign[7];
  }

  // Write back with error-feedback quantisation.
  for (int j = 0; j < kNumLines; ++j) {
    Line& l = dl_[j];
    int16_t* buf = l.buf;
    const int len = l.len;
    int p = l.pos;
    float e = err_[j];
    for (int i = 0; i < n; ++i) {
      float v = f[j][i] + e;
      if (!(v > -32767.0f)) v = -32767.0f;
      if (!(v < 32767.0f)) v = 32767.0f;
      // Above 1 LSB: round with first-order error feedback, so the tail
      // decays smoothly instead of in steps. At or below: truncate toward
      // zero and drop the error, so the tank cannot sustain itself on its
      // own quantisation noise (with error feedback alone a long RT settles
      // into a few-LSB noise floor forever and the reverb is never idle).
      int32_t q;
      if (v > 1.0f || v < -1.0f) {
        q = int32_t(v + 32768.5f) - 32768;               // round to nearest
        e = v - float(q);
      } else {
        q = int32_t(v);                                  // toward zero
        e = 0.0f;
      }
      buf[p] = int16_t(q);
      nonZero |= q;
      if (++p == len) p = 0;
    }
    err_[j] = e;
    l.pos = p;
  }

  if (nonZero) {
    zeroRun_ = 0;
  } else if (zeroRun_ < idleAfter_) {
    zeroRun_ += n;
    if (zeroRun_ >= idleAfter_) {
      for (int j = 0; j < kNumLines; ++j) { lp_[j] = 0.0f; err_[j] = 0.0f; }
      hp_ = 0.0f;
    }
  }
}

}  // namespace engine_detail
}  // namespace pg
