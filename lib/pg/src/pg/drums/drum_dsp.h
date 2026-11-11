// Small DSP helpers private to the drum voices. Everything lives in
// pg::drums so it cannot collide with other components' inline helpers.
// libm calls here (powf/expf/sinf/tanf) are for CONTROL RATE use only:
// parameter changes and note-ons, never inside a per-sample loop.
#pragma once
#include <cmath>
#include <cstdint>

namespace pg {
namespace drums {

constexpr float kPi      = 3.14159265f;
constexpr float kSilence = 1.0e-4f;  // -80 dBFS: below this a tail is dropped

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Saturator used by every drum's drive stage. Small-signal gain 1.5, output
// strictly inside (-1, 1), never flat:
//   |x| <= 0.8   the cubic 1.5x - 0.5x^3 (bit-identical to the old clipper)
//   |x| >  0.8   f0 + h * y / (1 + y), y = s0 * (|x| - 0.8) / h, which meets
//                the cubic with the same value (f0 = 0.944) and slope
//                (s0 = 0.54) and approaches 1 asymptotically.
// The old version clamped x to +-1 first, so everything driven past 1 came out
// as an exactly flat plateau (the "flat tops" the analysis tool reported in
// kicks and snares). Here the top keeps rounding off instead. The divide is
// only paid while the signal is past the knee (the loud part of a driven hit).
inline float saturate(float x) {
  const float ax = x < 0.0f ? -x : x;
  if (!(ax > 0.8f)) return x * (1.5f - 0.5f * x * x);
  constexpr float kF0 = 0.944f, kH = 0.056f, kK = 0.54f / 0.056f;
  const float y = (ax - 0.8f) * kK;
  const float v = y < 1.0e30f ? kF0 + kH * y / (1.0f + y) : 1.0f;
  return x < 0.0f ? -v : v;
}

inline float unit(uint8_t v) { return float(v) * (1.0f / 127.0f); }    // 0..127 -> 0..1
inline float centred(uint8_t v) { return float(v) * (1.0f / 128.0f); } // 64 -> exactly 0.5

// Exponential map of t in 0..1 onto lo..hi.
inline float expMap(float t, float lo, float hi) { return lo * powf(hi / lo, t); }
// Per-sample multiplier for an exponential decay with time constant tau.
inline float decayCoef(float tauSec, float fs) { return expf(-1.0f / (tauSec * fs)); }
// One-pole low-pass coefficient (y += g * (x - y)); always in (0, 1).
inline float onePoleCoef(float hz, float fs) { return 1.0f - expf(-2.0f * kPi * hz / fs); }
// "Magic circle" sine oscillator coefficient: x += e*y; y -= e*x;
inline float oscEps(float hz, float fs) {
  return 2.0f * sinf(kPi * clampf(hz, 1.0f, 0.24f * fs) / fs);
}
// 32-bit phase increment for the integer square oscillators.
inline uint32_t phaseInc(float hz, float fs) {
  return uint32_t(clampf(hz / fs, 0.0f, 0.49f) * 4294967296.0f);
}

// 2^((note-60)/12), transposition clamped to +-maxSemis.
inline float noteRatio(int note, int maxSemis) {
  static constexpr float kSemi[12] = {1.0f,       1.0594631f, 1.1224620f, 1.1892071f,
                                      1.2599210f, 1.3348399f, 1.4142136f, 1.4983071f,
                                      1.5874011f, 1.6817928f, 1.7817974f, 1.8877486f};
  int s = note - 60;
  if (s > maxSemis) s = maxSemis;
  if (s < -maxSemis) s = -maxSemis;
  int oct = 0;
  while (s < 0) { s += 12; --oct; }
  while (s >= 12) { s -= 12; ++oct; }
  float r = kSemi[s];
  for (; oct > 0; --oct) r *= 2.0f;
  for (; oct < 0; ++oct) r *= 0.5f;
  return r;
}

// Velocity 1..127 -> gain. Between linear and square law: 127 -> 0 dB,
// 100 -> -3.6 dB, 64 -> -10 dB, 32 -> -18.6 dB.
inline float velocityGain(uint8_t vel) {
  const float v = float(vel < 1 ? 1 : (vel > 127 ? 127 : vel)) * (1.0f / 127.0f);
  return v * (0.25f + 0.75f * v);
}

// Smooth ceiling: the identity up to 85 % of `c`, then a C1 asymptotic
// approach to c (never reached, never flat). invC = 1/c, precomputed. Used
// where the voices used to hard-clamp (retrigger overlap, hat edge pile-ups);
// costs one compare per sample unless the signal is actually near c.
inline float softLimit(float x, float c, float invC) {
  const float ax = x < 0.0f ? -x : x;
  constexpr float kKnee = 0.85f, kRange = 1.0f - kKnee, kInvRange = 1.0f / kRange;
  if (!(ax > kKnee * c)) return x;
  const float y = (ax * invC - kKnee) * kInvRange;
  const float v = y < 1.0e30f ? c * (kKnee + kRange * y / (1.0f + y)) : c;
  return x < 0.0f ? -v : v;
}

// Make-up gain for saturate(x * g) so a signal of nominal peak `peak` comes
// out at about 1.0 for every drive setting.
inline float driveNorm(float g, float peak) {
  const float in = g * peak;
  return 1.0f / saturate(in > 1.0e-6f ? in : 1.0e-6f);
}

// White noise, uniform in [-1, 1).
struct Noise {
  uint32_t s = 0x2545F491u;
  inline float next() {
    s = s * 1664525u + 1013904223u;
    return float(int32_t(s)) * (1.0f / 2147483648.0f);
  }
};

// Trapezoidal (zero-delay) state-variable filter: unconditionally stable for
// any cutoff below Nyquist and any positive damping.
struct Svf {
  float g = 0.1f, gk = 1.1f, d = 0.9f;  // tan(pi fc/fs), g + 1/Q, 1 / (1 + g*(g + 1/Q))
  float s1 = 0.0f, s2 = 0.0f;
  void set(float hz, float q, float fs) {
    g = tanf(kPi * clampf(hz, 20.0f, 0.45f * fs) / fs);
    const float k = 1.0f / clampf(q, 0.3f, 20.0f);
    gk = g + k;
    d = 1.0f / (1.0f + g * gk);
  }
  void reset() { s1 = s2 = 0.0f; }
};

// One SVF sample. 12 flops for all three outputs.
inline void svfTick(float in, float g, float gk, float d, float& s1, float& s2,
                    float& hp, float& bp, float& lp) {
  hp = (in - gk * s1 - s2) * d;
  const float v1 = g * hp;
  bp = v1 + s1;
  s1 = bp + v1;
  const float v2 = g * bp;
  lp = v2 + s2;
  s2 = lp + v2;
}

}  // namespace drums
}  // namespace pg
