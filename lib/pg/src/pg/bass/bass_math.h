// Small float-only math helpers for the bass voice. Everything is in
// pg::bass so it cannot collide with other components' helpers.
#pragma once
#include <cstdint>
#include <cstring>

namespace pg {
namespace bass {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x for |x| < 120, relative error < 3e-6 (0.005 cent). ~15 flops, no libm.
inline float fastExp2(float x) {
  float xr = x + 0.5f;
  int32_t i = int32_t(xr);
  if (float(i) > xr) --i;                       // floor(x + 0.5)
  if (i < -120) i = -120;
  if (i > 120) i = 120;
  float f = (x - float(i)) * 0.69314718f;       // |f| <= 0.347
  float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.041666667f + f * 0.0083333333f))));
  uint32_t bits = uint32_t(i + 127) << 23;
  float s;
  std::memcpy(&s, &bits, sizeof s);
  return p * s;
}

// exp(x) for x <= 0 (time-constant coefficients).
inline float fastExp(float x) { return fastExp2(x * 1.44269504f); }

// tan(x) for 0 <= x <= 1.2, Pade [5/4], relative error < 2e-5. One divide.
inline float fastTan(float x) {
  float x2 = x * x;
  return x * (945.0f + x2 * (x2 - 105.0f)) / (945.0f + x2 * (15.0f * x2 - 420.0f));
}

// Cubic soft clipper: unity gain at 0, reaches +-1 with zero slope at |x| = 1.5.
inline float softClip(float x) {
  float c = clampf(x, -1.5f, 1.5f);
  return c - 0.148148148f * c * c * c;
}
// Antiderivative of softClip (for ADAA).
inline float softClipInt(float x) {
  float a = x < 0.0f ? -x : x;
  float c = a < 1.5f ? a : 1.5f;
  float c2 = c * c;
  return c2 * (0.5f - 0.037037037f * c2) + (a - c);
}

// ---- shared constant tables (flash; generated into bass_tables_data.cpp by
// tools/gen/gen_tables.cpp from the formula in bass_tables_ref.cpp) -----------
constexpr int kSineBits = 9;
constexpr int kSineSize = 1 << kSineBits;        // 512 + 1 guard
constexpr int kBlepTaps = 16;                    // 8 before + 8 after the edge
constexpr int kBlepPhases = 32;                  // sub-sample positions (linear interp between)
extern const float gSine[kSineSize + 1];
extern const float gBlep[(kBlepPhases + 2) * kBlepTaps];
// Generator / test only: the formula behind the two tables above.
void buildTablesReference(float* sine, float* blep);

// sin(2*pi*phase / 2^32), linear interpolation. Error < -90 dB.
inline float sineLerp(uint32_t ph) {
  uint32_t i = ph >> (32 - kSineBits);
  float fr = float(int32_t(ph & ((1u << (32 - kSineBits)) - 1u))) * (1.0f / float(1u << (32 - kSineBits)));
  float a = gSine[i];
  return a + fr * (gSine[i + 1] - a);
}
// Nearest-entry version (modulator only).
inline float sineNear(uint32_t ph) { return gSine[ph >> (32 - kSineBits)]; }

}  // namespace bass
}  // namespace pg
