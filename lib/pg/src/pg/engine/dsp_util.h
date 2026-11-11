// Engine-internal DSP helpers. Header-only, namespace pg::engine_detail.
// float only; nothing here calls libm.
#pragma once
#include <cstdint>
#include <cstring>

namespace pg {
namespace engine_detail {

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float absf(float x) { return x < 0.0f ? -x : x; }

// 2^x for x in about [-60, 60]; relative error < 2e-4. Control-rate use
// (cutoff / time / decay mappings) instead of powf/expf.
inline float fastPow2(float x) {
  x = clampf(x, -60.0f, 60.0f);
  const float xs = x + 64.0f;
  const int xi = int(xs);
  const float f = xs - float(xi);
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
  const uint32_t bits = uint32_t(xi - 64 + 127) << 23;
  float scale;
  std::memcpy(&scale, &bits, sizeof(scale));
  return p * scale;
}
// e^x via fastPow2.
inline float fastExp(float x) { return fastPow2(x * 1.4426950f); }

// tan(x) for x in [0, 1.26] (cutoff up to 0.4 * fs), error < 0.6 %.
inline float fastTan(float x) {
  const float x2 = x * x;
  return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

// One-pole coefficient 1 - exp(-2*pi*fc/fs), clamped to a stable range.
inline float onePoleCoef(float fc, float fs) {
  const float k = 1.0f - fastExp(-6.2831853f * fc / fs);
  return clampf(k, 0.0f, 1.0f);
}

// ---- 2x polyphase-allpass half-band resamplers (3 coefficients, 53 dB
// stop-band from 0.3*fs, pass-band flat to 0.2*fs). The send effects run at
// half the sample rate; these cost three one-multiply allpasses per pair.
constexpr float kHbA0 = 0.1284563494f;   // path A, first section
constexpr float kHbB0 = 0.4295667407f;   // path B
constexpr float kHbA1 = 0.7906755036f;   // path A, second section

struct HalfbandDown {
  float xa0, ya0, xa1, ya1, xb0, yb0;   // no initialisers: lives in the engine's .bss block, call reset()
  void reset() { xa0 = ya0 = xa1 = ya1 = xb0 = yb0 = 0.0f; }
  // in: 2*n samples, out: n samples.
  void process(const float* in, float* out, int n) {
    float x0 = xa0, y0 = ya0, x1 = xa1, y1 = ya1, xb = xb0, yb = yb0;
    for (int i = 0; i < n; ++i) {
      const float a = in[2 * i + 1];
      const float b = in[2 * i];
      const float t0 = (a - y0) * kHbA0 + x0; x0 = a; y0 = t0;
      const float t1 = (t0 - y1) * kHbA1 + x1; x1 = t0; y1 = t1;
      const float u0 = (b - yb) * kHbB0 + xb; xb = b; yb = u0;
      out[i] = 0.5f * (t1 + u0);
    }
    xa0 = x0; ya0 = y0; xa1 = x1; ya1 = y1; xb0 = xb; yb0 = yb;
  }
};

struct HalfbandUp {
  float xa0, ya0, xa1, ya1, xb0, yb0;   // no initialisers: lives in the engine's .bss block, call reset()
  void reset() { xa0 = ya0 = xa1 = ya1 = xb0 = yb0 = 0.0f; }
  // in: n samples, out: 2*n samples.
  void process(const float* in, float* out, int n) {
    float x0 = xa0, y0 = ya0, x1 = xa1, y1 = ya1, xb = xb0, yb = yb0;
    for (int i = 0; i < n; ++i) {
      const float a = in[i];
      const float t0 = (a - y0) * kHbA0 + x0; x0 = a; y0 = t0;
      const float t1 = (t0 - y1) * kHbA1 + x1; x1 = t0; y1 = t1;
      const float u0 = (a - yb) * kHbB0 + xb; xb = a; yb = u0;
      out[2 * i] = t1;
      out[2 * i + 1] = u0;
    }
    xa0 = x0; ya0 = y0; xa1 = x1; ya1 = y1; xb0 = xb; yb0 = yb;
  }
};

// float -> int16 with saturation, truncating toward zero (so recirculating
// buffers always decay to exact silence, never to a +-1 LSB limit cycle).
inline int16_t toI16Trunc(float v) {
  if (!(v > -32767.0f)) v = -32767.0f;   // written this way so NaN saturates too
  if (!(v < 32767.0f)) v = 32767.0f;
  return int16_t(int32_t(v));
}

}  // namespace engine_detail
}  // namespace pg
