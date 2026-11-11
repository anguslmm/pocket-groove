// Reference formula for the bass voice's sine and BLEP tables. NOT used at run
// time: the voice reads the constant data in bass_tables_data.cpp, generated
// by tools/gen/gen_tables.cpp from buildTablesReference() below.
// host/tests/test_bass_tables.cpp checks that the data still equals it.
#include <cmath>

#include "bass_math.h"

namespace pg {
namespace bass {

namespace {

// Zeroth-order modified Bessel function (Kaiser window), float series.
float besselI0(float x) {
  float sum = 1.0f, term = 1.0f, h = 0.5f * x;
  for (int k = 1; k < 32; ++k) {
    float t = h / float(k);
    term *= t * t;
    sum += term;
    if (term < 1e-9f * sum) break;
  }
  return sum;
}

// Band-limiting kernel for the BLEP: Kaiser-windowed sinc, 16 samples long.
// kBlepCut is the -6 dB point in cycles/sample. It is deliberately below
// Nyquist: with only 16 taps the transition band is wide, and for a bass
// instrument trading a little "air" above 12 kHz for aliases that are >60 dB
// down is the right deal.
constexpr float kBlepCut = 0.40f;
constexpr float kBlepBeta = 8.0f;
constexpr int kBlepSub = 8;   // integration sub-steps per table phase

float blepKernel(float t) {   // t in samples, |t| <= 8
  float half = 0.5f * float(kBlepTaps);
  float u = t / half;
  float w2 = 1.0f - u * u;
  if (w2 <= 0.0f) return 0.0f;
  float win = besselI0(kBlepBeta * sqrtf(w2));
  float a = kTwoPi * kBlepCut * t;
  float sinc = (a > -1e-4f && a < 1e-4f) ? 1.0f : sinf(a) / a;
  return win * sinc;
}

}  // namespace

void buildTablesReference(float* gSine, float* gBlep) {
  for (int i = 0; i <= kSineSize; ++i) gSine[i] = sinf(kTwoPi * float(i) / float(kSineSize));
  gSine[kSineSize] = 0.0f;

  // Integrate the kernel on a fine grid (trapezoid), twice: once for the
  // normalisation, once to fill the table. No big temporaries.
  const int fine = kBlepPhases * kBlepSub;              // steps per sample
  const int total = kBlepTaps * fine;
  const float dt = 1.0f / float(fine);
  const float t0 = -0.5f * float(kBlepTaps);
  float norm = 0.0f;
  {
    float prev = blepKernel(t0);
    for (int n = 1; n <= total; ++n) {
      float cur = blepKernel(t0 + float(n) * dt);
      norm += 0.5f * (prev + cur);
      prev = cur;
    }
  }
  float acc = 0.0f, prev = blepKernel(t0);
  for (int n = 0; n <= total; ++n) {
    if (n > 0) {
      float cur = blepKernel(t0 + float(n) * dt);
      acc += 0.5f * (prev + cur);
      prev = cur;
    }
    if (n % kBlepSub == 0) {
      int m = n / kBlepSub;                 // = j * kBlepPhases + p
      float blep = acc / norm;              // 0 .. 1
      // Entry (p, j) holds residual(t = j - 8 + p/phases). A fine index maps
      // to (p, j) and, at p == 0, also to (phases, j - 1): the latter is the
      // left limit, which matters at the step itself (t = 0).
      int j = m / kBlepPhases, p = m % kBlepPhases;
      if (j < kBlepTaps) gBlep[p * kBlepTaps + j] = blep - (j >= kBlepTaps / 2 ? 1.0f : 0.0f);
      if (p == 0 && j > 0) gBlep[kBlepPhases * kBlepTaps + (j - 1)] = blep - (j - 1 >= kBlepTaps / 2 ? 1.0f : 0.0f);
    }
  }
  // Guard row so a rounding t == 1.0 can never read past the table.
  for (int j = 0; j < kBlepTaps; ++j) gBlep[(kBlepPhases + 1) * kBlepTaps + j] = gBlep[kBlepPhases * kBlepTaps + j];
}

}  // namespace bass
}  // namespace pg
