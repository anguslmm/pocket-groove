// Pocket Groove hardware spike -- small DSP pieces. Portable, header-only,
// unit-tested on the host (host/hwspike).
//
//  * Voice / voiceRender: linear-interpolating sample player, integer only
//    (16-bit samples, 32.32 fixed-point position), reading the sample data
//    through a plain pointer -- RAM or memory-mapped flash alike, which is the
//    point of the playback-cost benchmark.
//  * Svf: Zavalishin TPT state-variable filter, low-pass output (the tilt page's
//    resonant filter).
//  * LevelAcc: peak / RMS / DC / clip / stuck statistics of 16-bit input.
#pragma once
#include <cmath>
#include <cstdint>

#ifndef HWS_INLINE
#define HWS_INLINE inline __attribute__((always_inline))
#endif

namespace hws {

// ---------------------------------------------------------------------------
// Sample voice
// ---------------------------------------------------------------------------
struct Voice {
  const int16_t* data = nullptr;
  uint32_t len = 0;        // samples
  uint32_t idx = 0;        // integer position
  uint32_t frac = 0;       // fractional position, Q0.32
  uint32_t stepInt = 1;
  uint32_t stepFrac = 0;
  int32_t  gain = 0;       // Q15 (32767 = 1.0)
  bool     active = false;
  bool     loop = false;   // wrap to the start at the end (benchmark), else stop
};

// Playback ratio (1.0 = original pitch) as a 32.32 step. Ratios are clamped to [1/256, 16].
inline void ratioToStep(float ratio, uint32_t* stepInt, uint32_t* stepFrac) {
  if (!(ratio >= 0.00390625f)) ratio = 0.00390625f;
  if (ratio > 16.0f) ratio = 16.0f;
  const uint32_t i = static_cast<uint32_t>(ratio);
  float f = (ratio - static_cast<float>(i)) * 4294967296.0f;
  if (f > 4294967040.0f) f = 4294967040.0f;   // largest float below 2^32
  if (f < 0.0f) f = 0.0f;
  *stepInt = i;
  *stepFrac = static_cast<uint32_t>(f);
}

inline float semitoneRatio(float semis) { return exp2f(semis * (1.0f / 12.0f)); }

inline void voiceStart(Voice& v, const int16_t* data, uint32_t len, uint32_t startIdx, float ratio, float gain,
                       bool loop) {
  v.data = data;
  v.len = len;
  v.idx = (len > 1 && startIdx < len - 1) ? startIdx : 0;
  v.frac = 0;
  ratioToStep(ratio, &v.stepInt, &v.stepFrac);
  float g = gain * 32767.0f;
  if (g > 32767.0f) g = 32767.0f;
  if (g < 0.0f) g = 0.0f;
  v.gain = static_cast<int32_t>(g);
  v.loop = loop;
  v.active = (data != nullptr && len >= 2);
}

// Adds n output samples of v into mix (int32, not saturated). Returns the
// number of samples produced (< n when a one-shot voice ran out; it is then
// inactive). Interpolation: y = a + (b - a) * f with f the top 15 bits of frac.
// Overflow check: |b - a| <= 65535 and f <= 32767, product <= 2147385345 < 2^31.
HWS_INLINE int voiceRender(Voice& v, int32_t* mix, int n) {
  if (!v.active) return 0;
  const int16_t* d = v.data;
  const uint32_t last = v.len - 1;
  const uint32_t si = v.stepInt, sf = v.stepFrac;
  const int32_t g = v.gain;
  uint32_t idx = v.idx, frac = v.frac;
  int i = 0;
  for (; i < n; ++i) {
    if (idx >= last) {
      if (!v.loop) {
        v.active = false;
        break;
      }
      idx -= last;
      if (idx >= last) idx = 0;
    }
    const int32_t a = d[idx];
    const int32_t b = d[idx + 1];
    const int32_t f = static_cast<int32_t>(frac >> 17);
    const int32_t y = a + (((b - a) * f) >> 15);
    mix[i] += (y * g) >> 15;
    const uint32_t nf = frac + sf;
    idx += si + (nf < frac ? 1u : 0u);
    frac = nf;
  }
  v.idx = idx;
  v.frac = frac;
  return i;
}

inline int16_t sat16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return static_cast<int16_t>(x);
}

// ---------------------------------------------------------------------------
// TPT state-variable filter, low-pass
// ---------------------------------------------------------------------------
struct Svf {
  float ic1 = 0, ic2 = 0;
  float a1 = 1, a2 = 0, a3 = 0;
  // res 0..1 (clamped to 0.98: k = 2 - 2 res >= 0.04 keeps it stable).
  void set(float cutoffHz, float res, float fs) {
    float fc = cutoffHz;
    const float maxFc = 0.45f * fs;
    if (!(fc >= 10.0f)) fc = 10.0f;
    if (fc > maxFc) fc = maxFc;
    float r = res;
    if (!(r >= 0.0f)) r = 0.0f;
    if (r > 0.98f) r = 0.98f;
    const float g = tanf(3.14159265f * fc / fs);
    const float k = 2.0f - 2.0f * r;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  HWS_INLINE float lp(float x) {
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return v2;
  }
  void reset() { ic1 = ic2 = 0; }
};

// ---------------------------------------------------------------------------
// Input level statistics
// ---------------------------------------------------------------------------
constexpr int16_t kClipLevel = 32700;   // |x| >= this counts as clipped

struct LevelAcc {
  int64_t  sum = 0;
  uint64_t sumsq = 0;
  int32_t  peak = 0;          // max |x|
  int16_t  mn = 32767, mx = -32768;
  uint32_t n = 0, clips = 0;
  HWS_INLINE void add(int16_t s) {
    const int32_t x = s;
    sum += x;
    sumsq += static_cast<uint64_t>(static_cast<int64_t>(x) * x);
    const int32_t ax = x < 0 ? -x : x;
    if (ax > peak) peak = ax;
    if (s < mn) mn = s;
    if (s > mx) mx = s;
    if (ax >= kClipLevel) ++clips;
    ++n;
  }
  void merge(const LevelAcc& o) {
    sum += o.sum;
    sumsq += o.sumsq;
    if (o.peak > peak) peak = o.peak;
    if (o.mn < mn) mn = o.mn;
    if (o.mx > mx) mx = o.mx;
    n += o.n;
    clips += o.clips;
  }
  void clear() { *this = LevelAcc(); }
};

// dBFS of a level in LSB (32768 = 0 dBFS), floored at -120.
inline float dbfs(float lsb) {
  if (!(lsb > 0.0f)) return -120.0f;
  const float d = 20.0f * log10f(lsb / 32768.0f);
  return d < -120.0f ? -120.0f : d;
}

struct LevelResult {
  float    rmsDb;      // RMS including DC
  float    acRmsDb;    // RMS with the DC removed (noise floor)
  float    peakDb;
  float    dcLsb;      // mean
  float    dcDb;
  uint32_t clips;
  uint32_t n;
  bool     stuck;      // every sample identical (n > 1)
  int16_t  mn, mx;
};

inline LevelResult levelResult(const LevelAcc& a) {
  LevelResult r{};
  r.n = a.n;
  r.clips = a.clips;
  r.mn = a.mn;
  r.mx = a.mx;
  if (a.n == 0) {
    r.rmsDb = r.acRmsDb = r.peakDb = r.dcDb = -120.0f;
    r.dcLsb = 0;
    r.stuck = false;
    return r;
  }
  // Double on purpose (control rate only): mean^2 nearly cancels mean-square
  // when a small noise floor sits on a DC offset.
  const double n = static_cast<double>(a.n);
  const double meanD = static_cast<double>(a.sum) / n;
  const double msD = static_cast<double>(a.sumsq) / n;
  double varD = msD - meanD * meanD;
  if (varD < 0.0) varD = 0.0;
  const float mean = static_cast<float>(meanD);
  r.rmsDb = dbfs(static_cast<float>(sqrt(msD)));
  r.acRmsDb = dbfs(static_cast<float>(sqrt(varD)));
  r.peakDb = dbfs(static_cast<float>(a.peak));
  r.dcLsb = mean;
  r.dcDb = dbfs(fabsf(mean));
  r.stuck = (a.n > 1 && a.mn == a.mx);
  return r;
}

}  // namespace hws
