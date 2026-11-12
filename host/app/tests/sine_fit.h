// A sine's frequency, level and THD+N, for the Listen tests
// (test_desktop_listen, listen_analyze): least-squares fit of
// a sin + b cos + c at a frequency found by golden-section search near a
// guess; THD+N = what is left after removing the fitted fundamental, relative
// to it (noise, harmonics, clicks, dropouts, modulation: everything that is
// not the tone).
#pragma once
#include <cmath>
#include <cstddef>

namespace sinefit {

struct Fit {
  double freq = 0.0;     // Hz
  double amp = 0.0;      // peak amplitude, in the samples' units
  double thdnDb = 0.0;   // residual RMS / fundamental RMS, dB
};

// Residual energy of the best a sin + b cos + c at frequency f (rad/sample w).
inline double residual(const float* x, size_t n, double w, double* amp) {
  double ss = 0, sc = 0, cc = 0, s1 = 0, c1 = 0, xs = 0, xc = 0, x1 = 0, xx = 0;
  // sin/cos by rotation (double precision is plenty for a second of audio)
  const double cw = std::cos(w), sw = std::sin(w);
  double s = 0.0, c = 1.0;
  for (size_t i = 0; i < n; ++i) {
    const double v = x[i];
    ss += s * s; sc += s * c; cc += c * c; s1 += s; c1 += c;
    xs += v * s; xc += v * c; x1 += v; xx += v * v;
    const double s2 = s * cw + c * sw;
    c = c * cw - s * sw;
    s = s2;
    if ((i & 1023) == 1023) {   // renormalise
      const double r = std::sqrt(s * s + c * c);
      s /= r;
      c /= r;
    }
  }
  const double N = double(n);
  // Solve [ss sc s1; sc cc c1; s1 c1 N] [a b k] = [xs xc x1] (Cramer).
  const double m[3][3] = {{ss, sc, s1}, {sc, cc, c1}, {s1, c1, N}};
  const double r[3] = {xs, xc, x1};
  auto det3 = [](const double q[3][3]) {
    return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
           q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
  };
  const double d = det3(m);
  double sol[3];
  for (int k = 0; k < 3; ++k) {
    double q[3][3];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) q[i][j] = j == k ? r[i] : m[i][j];
    sol[k] = det3(q) / d;
  }
  *amp = std::sqrt(sol[0] * sol[0] + sol[1] * sol[1]);
  // residual = xx - sol . r  (least squares)
  const double res = xx - (sol[0] * xs + sol[1] * xc + sol[2] * x1);
  return res > 0.0 ? res : 0.0;
}

// Fits the tone near fGuess (within +-spread, relative).
inline Fit fit(const float* x, size_t n, double rate, double fGuess, double spread = 0.01) {
  double lo = fGuess * (1.0 - spread), hi = fGuess * (1.0 + spread);
  const double g = 0.5 * (std::sqrt(5.0) - 1.0);
  double amp = 0.0;
  auto f = [&](double hz) { return residual(x, n, 2.0 * M_PI * hz / rate, &amp); };
  // coarse scan first (the residual has side lobes), then golden section around the best
  double best = fGuess, bestR = 1e300;
  for (int i = 0; i <= 40; ++i) {
    const double hz = lo + (hi - lo) * i / 40.0;
    const double rr = f(hz);
    if (rr < bestR) { bestR = rr; best = hz; }
  }
  const double step = (hi - lo) / 40.0;
  lo = best - step;
  hi = best + step;
  double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
  double fa = f(a), fb = f(b);
  for (int i = 0; i < 60; ++i) {
    if (fa < fb) { hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = f(a); }
    else { lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = f(b); }
  }
  Fit out;
  out.freq = 0.5 * (lo + hi);
  const double res = f(out.freq);
  out.amp = amp;
  const double sig = amp * amp / 2.0 * double(n);
  out.thdnDb = sig > 0.0 ? 10.0 * std::log10((res > 1e-30 ? res : 1e-30) / sig) : 0.0;
  return out;
}

}  // namespace sinefit
