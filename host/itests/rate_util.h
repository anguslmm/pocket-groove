// Helpers for the sample-rate tests (host/itests/itest_rate48*.cpp): render a voice
// the way the engine drives it, spectra with sub-bin peak estimates, decay times.
// Host code (doubles, the standard library). Not a test itself.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "pg/config.h"
#include "pg/voice.h"

namespace rt {

inline double midiHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }
inline double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
inline double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-15)); }

// The voice's parameter defaults (24 bytes).
inline void defaults(const pg::Voice& v, uint8_t* p) {
  for (int i = 0; i < pg::kSoundParams; ++i) {
    const pg::ParamInfo* pi = v.paramInfo(i);
    p[i] = pi ? pi->def : 0;
  }
}

// One note, as the engine drives a voice: init at fs, tempo, then per quantum
// setParams -> (the note-on at quantum 0, the note-off at gateSec) -> render while
// active. Returns the mono sum (L + R) / 2, totalSec long.
inline std::vector<float> renderNote(pg::Voice& v, float fs, const uint8_t* p, pg::NoteEvent e, double gateSec,
                                     double totalSec, float bpm = 120.0f) {
  v.init(fs);
  v.setTempo(bpm);
  const long quanta = long(std::ceil(totalSec * double(fs) / pg::kQuantum));
  const long offQ = long(std::lround(gateSec * double(fs) / pg::kQuantum));
  std::vector<float> out;
  out.reserve(size_t(quanta) * pg::kQuantum);
  float l[pg::kQuantum], r[pg::kQuantum];
  for (long q = 0; q < quanta; ++q) {
    v.setParams(p);
    if (q == 0) v.noteOn(e);
    if (q == offQ) v.noteOff(e.note);
    if (v.active()) {
      v.render(l, r, pg::kQuantum);
      for (int i = 0; i < pg::kQuantum; ++i) out.push_back(0.5f * (l[i] + r[i]));
    } else {
      for (int i = 0; i < pg::kQuantum; ++i) out.push_back(0.0f);
    }
  }
  return out;
}

// In-place radix-2 FFT (n a power of two).
inline void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979323846 / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Magnitude spectrum of x[from, from + n) with a Hann window, zero-padded so a bin
// is at most resHz wide. Magnitudes normalised so a full-scale sine reads ~1.
struct Spectrum {
  std::vector<double> mag;
  double binHz = 1.0, fs = 1.0;
  double hzOf(double bin) const { return bin * binHz; }
};
inline Spectrum spectrum(const std::vector<float>& x, double fs, size_t from, size_t n, double resHz = 0.5) {
  if (from > x.size()) from = x.size();
  if (from + n > x.size()) n = x.size() - from;
  size_t N = 1;
  while (N < n || double(fs) / double(N) > resHz) N <<= 1;
  std::vector<std::complex<double>> a(N);
  double wsum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double(i) + 0.5) / double(n));
    a[i] = double(x[from + i]) * w;
    wsum += w;
  }
  fft(a);
  Spectrum s;
  s.fs = fs;
  s.binHz = fs / double(N);
  s.mag.resize(N / 2);
  for (size_t k = 0; k < N / 2; ++k) s.mag[k] = 2.0 * std::abs(a[k]) / std::max(wsum, 1e-9);
  return s;
}

// The strongest local maximum within [lo, hi] Hz, refined by a parabola through the
// log magnitudes of it and its neighbours. 0 if none.
inline double peakHz(const Spectrum& s, double lo, double hi, double* magOut = nullptr) {
  const long a = std::max<long>(1, long(lo / s.binHz)), b = std::min<long>(long(s.mag.size()) - 2, long(hi / s.binHz) + 1);
  long best = -1;
  for (long k = a; k <= b; ++k)
    if (s.mag[size_t(k)] >= s.mag[size_t(k - 1)] && s.mag[size_t(k)] >= s.mag[size_t(k + 1)] &&
        (best < 0 || s.mag[size_t(k)] > s.mag[size_t(best)]))
      best = k;
  if (best < 0) return 0.0;
  const double y0 = std::log(s.mag[size_t(best - 1)] + 1e-30), y1 = std::log(s.mag[size_t(best)] + 1e-30),
               y2 = std::log(s.mag[size_t(best + 1)] + 1e-30);
  const double den = y0 - 2.0 * y1 + y2;
  const double d = den < 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
  if (magOut) *magOut = s.mag[size_t(best)];
  return s.hzOf(double(best) + d);
}

// The k strongest local maxima in [lo, hi] Hz within rangeDb of the strongest one.
inline std::vector<double> peaks(const Spectrum& s, double lo, double hi, int k, double rangeDb = 30.0) {
  const long a = std::max<long>(1, long(lo / s.binHz)), b = std::min<long>(long(s.mag.size()) - 2, long(hi / s.binHz));
  double top = 0.0;
  for (long i = a; i <= b; ++i) top = std::max(top, s.mag[size_t(i)]);
  std::vector<std::pair<double, long>> cand;
  for (long i = a; i <= b; ++i) {
    const double m = s.mag[size_t(i)];
    if (m >= s.mag[size_t(i - 1)] && m > s.mag[size_t(i + 1)] && dB(m) >= dB(top) - rangeDb) cand.push_back({m, i});
  }
  std::sort(cand.begin(), cand.end(), [](const std::pair<double, long>& x, const std::pair<double, long>& y) {
    return x.first > y.first;
  });
  std::vector<double> out;
  for (const auto& c : cand) {
    // one per 3 % (a peak's own skirt is not another peak)
    const double f = s.hzOf(double(c.second));
    bool near = false;
    for (double g : out) near |= std::fabs(f / g - 1.0) < 0.03;
    if (near) continue;
    out.push_back(peakHz(s, f * 0.995, f * 1.005));
    if (int(out.size()) >= k) break;
  }
  return out;
}

// Power-weighted spectral centroid within [lo, hi] Hz.
inline double centroid(const Spectrum& s, double lo = 20.0, double hi = 20000.0) {
  double num = 0.0, den = 0.0;
  for (size_t k = 1; k < s.mag.size(); ++k) {
    const double f = s.hzOf(double(k));
    if (f < lo || f > hi) continue;
    const double p = s.mag[k] * s.mag[k];
    num += p * f;
    den += p;
  }
  return den > 0.0 ? num / den : 0.0;
}

// The envelope (RMS over 5 ms windows, hop 1 ms) and the time in ms from its peak to
// the first window `dropDb` below it; -1 if it never gets there.
inline double decayMs(const std::vector<float>& x, double fs, double dropDb, size_t from = 0) {
  const size_t w = size_t(0.005 * fs), hop = size_t(0.001 * fs);
  std::vector<double> env;
  for (size_t i = from; i + w <= x.size(); i += hop) {
    double s = 0.0;
    for (size_t j = 0; j < w; ++j) s += double(x[i + j]) * double(x[i + j]);
    env.push_back(std::sqrt(s / double(w)));
  }
  size_t pk = 0;
  for (size_t i = 0; i < env.size(); ++i)
    if (env[i] > env[pk]) pk = i;
  const double thr = env.empty() ? 0.0 : env[pk] * std::pow(10.0, -dropDb / 20.0);
  for (size_t i = pk; i < env.size(); ++i)
    if (env[i] < thr) return double(i - pk) * double(hop) * 1000.0 / fs;
  return -1.0;
}

// Time in ms of the first |x| >= thr from `from`, -1 if none.
inline double onsetMs(const std::vector<float>& x, double fs, float thr, size_t from = 0) {
  for (size_t i = from; i < x.size(); ++i)
    if (std::fabs(x[i]) >= thr) return double(i) * 1000.0 / fs;
  return -1.0;
}

inline double rmsDb(const std::vector<float>& x, size_t from = 0, size_t to = size_t(-1)) {
  to = std::min(to, x.size());
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += double(x[i]) * double(x[i]);
  return to > from ? 10.0 * std::log10(s / double(to - from) + 1e-30) : -300.0;
}

}  // namespace rt
