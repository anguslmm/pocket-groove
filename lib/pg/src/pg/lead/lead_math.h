// Small control-rate math helpers for the lead voice. Everything lives in
// pg::lead so it cannot collide with other components' inline helpers.
#pragma once
#include <cstdint>
#include <cstring>

namespace pg {
namespace lead {

constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x for x in about [-60, 60]. Degree-5 minimax polynomial on the fraction,
// exponent built directly; relative error < 5e-7 (< 0.001 cent). ~12 flops.
inline float exp2Fast(float x) {
  const float xs = x + 64.0f;
  const int32_t i = int32_t(xs);
  const float f = xs - float(i);
  const float p = 1.0f + f * (0.693147600f + f * (0.240206867f + f * (0.0556586646f +
                  f * (0.00919680204f + f * 0.00178966508f))));
  const uint32_t bits = uint32_t(i - 64 + 127) << 23;
  float s;
  std::memcpy(&s, &bits, sizeof s);
  return p * s;
}

// sin and cos of a in [0, pi/2] by their Taylor series (to a^11 / a^12):
// absolute error below 1e-7. Control rate only (the oscillator mix in motion).
inline float sinQuarter(float a) {
  const float a2 = a * a;
  return a * (1.0f - a2 * (1.0f / 6.0f - a2 * (1.0f / 120.0f - a2 * (1.0f / 5040.0f - a2 * (1.0f / 362880.0f -
              a2 * (1.0f / 39916800.0f))))));
}
inline float cosQuarter(float a) {
  const float a2 = a * a;
  return 1.0f - a2 * (0.5f - a2 * (1.0f / 24.0f - a2 * (1.0f / 720.0f - a2 * (1.0f / 40320.0f - a2 * (1.0f / 3628800.0f -
                a2 * (1.0f / 479001600.0f))))));
}

// 1/sqrt(x), x > 0. Bit trick + two Newton steps, relative error < 5e-6.
inline float rsqrtFast(float x) {
  uint32_t i;
  std::memcpy(&i, &x, sizeof i);
  i = 0x5f375a86u - (i >> 1);
  float y;
  std::memcpy(&y, &i, sizeof y);
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  return y;
}

}  // namespace lead
}  // namespace pg
