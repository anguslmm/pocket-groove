// Shared helpers for the lead-voice host tests (header only, host code: doubles are fine here).
#pragma once
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/wav.h"
#include "pg/lead/lead.h"

namespace lt {

using pg::lead::LeadVoice;

static int gFailures = 0;
#define LT_CHECK(cond, ...)                                  \
  do {                                                       \
    if (!(cond)) {                                           \
      ++lt::gFailures;                                       \
      std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                              \
      std::printf("\n");                                     \
    }                                                        \
  } while (0)

inline int finish(const char* name) {
  std::printf("%s: %s (%d failure%s)\n", name, gFailures ? "FAILED" : "ok", gFailures, gFailures == 1 ? "" : "s");
  return gFailures ? 1 : 0;
}

inline double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// A LeadVoice driven the way the engine drives it: setParams every quantum,
// render only while active(), events on quantum edges.
struct Synth {
  LeadVoice v;
  uint8_t p[pg::kSoundParams];
  float fs;
  std::vector<float> L, R;
  long quanta = 0, renderedQuanta = 0;

  explicit Synth(float sampleRate = 44100.0f) : fs(sampleRate) {
    v.init(fs);
    defaults();
  }
  void defaults() {
    for (int i = 0; i < pg::kSoundParams; ++i) {
      const pg::ParamInfo* pi = v.paramInfo(i);
      p[i] = pi ? pi->def : 0;
    }
  }
  void set(int idx, int val) { p[idx] = uint8_t(val); }
  void on(int note, int vel = 127, int chord = 0, bool accent = false, bool slide = false) {
    v.setParams(p);
    pg::NoteEvent e;
    e.note = uint8_t(note);
    e.vel = uint8_t(vel);
    e.accent = accent;
    e.slide = slide;
    e.chord = uint8_t(chord);
    v.noteOn(e);
  }
  void off(int note) { v.noteOff(uint8_t(note)); }
  void runQuanta(long n) {
    float l[pg::kQuantum], r[pg::kQuantum];
    for (long q = 0; q < n; ++q) {
      v.setParams(p);
      if (v.active()) {
        v.render(l, r, pg::kQuantum);
        ++renderedQuanta;
      } else {
        for (int i = 0; i < pg::kQuantum; ++i) l[i] = r[i] = 0.0f;
      }
      L.insert(L.end(), l, l + pg::kQuantum);
      R.insert(R.end(), r, r + pg::kQuantum);
      ++quanta;
    }
  }
  void run(double seconds) { runQuanta(long(seconds * double(fs) / pg::kQuantum + 0.5)); }
  // Run until absolute time t (seconds since construction / clear).
  void runTo(double t) {
    const long target = long(t * double(fs) / pg::kQuantum + 0.5);
    if (target > quanta) runQuanta(target - quanta);
  }
  double now() const { return double(quanta) * pg::kQuantum / double(fs); }
  void clear() { L.clear(); R.clear(); quanta = 0; }
  size_t size() const { return L.size(); }
  bool write(const std::string& path) const {
    std::vector<float> il(L.size() * 2);
    for (size_t i = 0; i < L.size(); ++i) { il[2 * i] = L[i]; il[2 * i + 1] = R[i]; }
    return wav::writeFloat(path.c_str(), il, int(fs), 2);
  }
};

inline void mkdirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty()) ::mkdir(cur.c_str(), 0777);
    }
    if (i < path.size()) cur += path[i];
  }
}

inline double peak(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  double m = 0;
  b = std::min(b, x.size());
  for (size_t i = a; i < b; ++i) m = std::max(m, double(std::fabs(x[i])));
  return m;
}
inline double rms(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  double s = 0;
  b = std::min(b, x.size());
  if (b <= a) return 0;
  for (size_t i = a; i < b; ++i) s += double(x[i]) * double(x[i]);
  return std::sqrt(s / double(b - a));
}
inline double mean(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  double s = 0;
  b = std::min(b, x.size());
  if (b <= a) return 0;
  for (size_t i = a; i < b; ++i) s += double(x[i]);
  return s / double(b - a);
}
inline bool allFinite(const std::vector<float>& x) {
  for (float f : x)
    if (!std::isfinite(f)) return false;
  return true;
}
inline double maxStep(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  double m = 0;
  b = std::min(b, x.size());
  for (size_t i = a + 1; i < b; ++i) m = std::max(m, double(std::fabs(x[i] - x[i - 1])));
  return m;
}

// In-place radix-2 FFT.
inline void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * M_PI / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<double> u = a[i + j], t = a[i + j + len / 2] * w;
        a[i + j] = u + t;
        a[i + j + len / 2] = u - t;
        w *= wl;
      }
    }
  }
}

// Magnitude spectrum of n samples from x[start..] with a 4-term Blackman-Harris
// window (sidelobes -92 dB), scaled so a full-scale sine reads 1.0.
inline std::vector<double> spectrum(const std::vector<float>& x, size_t start, size_t n) {
  std::vector<std::complex<double>> a(n);
  double wsum = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = 2.0 * M_PI * double(i) / double(n);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    wsum += w;
    a[i] = (start + i < x.size() ? double(x[start + i]) : 0.0) * w;
  }
  fft(a);
  std::vector<double> m(n / 2);
  for (size_t i = 0; i < n / 2; ++i) m[i] = std::abs(a[i]) * 2.0 / wsum;
  return m;
}

// Frequency (in bins, fractional) of the local peak around bin k: parabolic fit on log magnitude.
inline double refinePeak(const std::vector<double>& m, size_t k) {
  if (k == 0 || k + 1 >= m.size()) return double(k);
  const double a = std::log(std::max(m[k - 1], 1e-30)), b = std::log(std::max(m[k], 1e-30)),
               c = std::log(std::max(m[k + 1], 1e-30));
  const double d = a - 2 * b + c;
  return double(k) + (d != 0.0 ? 0.5 * (a - c) / d : 0.0);
}

struct Peak {
  double hz, mag;
};
// Local maxima above `floorMag`, strongest first.
inline std::vector<Peak> findPeaks(const std::vector<double>& m, double binHz, double floorMag) {
  std::vector<Peak> out;
  for (size_t k = 2; k + 2 < m.size(); ++k) {
    if (m[k] > floorMag && m[k] >= m[k - 1] && m[k] > m[k + 1] && m[k] >= m[k - 2] && m[k] > m[k + 2])
      out.push_back({refinePeak(m, k) * binHz, m[k]});
  }
  std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.mag > b.mag; });
  return out;
}


// Amplitude envelope via the analytic signal (FFT Hilbert transform).
inline std::vector<double> envelope(const std::vector<float>& x) {
  size_t n = 1;
  while (n < x.size()) n <<= 1;
  std::vector<std::complex<double>> a(n);
  for (size_t i = 0; i < x.size(); ++i) a[i] = double(x[i]);
  fft(a);
  for (size_t i = 1; i < n / 2; ++i) a[i] *= 2.0;
  for (size_t i = n / 2 + 1; i < n; ++i) a[i] = 0.0;
  for (auto& c : a) c = std::conj(c);   // inverse via conjugation
  fft(a);
  std::vector<double> e(x.size());
  for (size_t i = 0; i < x.size(); ++i) e[i] = std::abs(a[i]) / double(n);
  return e;
}

// Fundamental frequency near `expectHz` (+-1 semitone) from a long windowed FFT.
inline double measureHz(const std::vector<float>& x, double fs, size_t start, size_t n, double expectHz) {
  const std::vector<double> m = spectrum(x, start, n);
  const double binHz = fs / double(n);
  const size_t lo = size_t(expectHz / 1.06 / binHz), hi = std::min(m.size() - 2, size_t(expectHz * 1.06 / binHz) + 1);
  size_t k = lo;
  for (size_t i = lo; i <= hi; ++i)
    if (m[i] > m[k]) k = i;
  return refinePeak(m, k) * binHz;
}

// Instantaneous frequency from positive-going zero crossings in [a, b) (mono-ish signals).
inline double zeroCrossHz(const std::vector<float>& x, double fs, size_t a, size_t b) {
  double first = -1, last = -1;
  int count = 0;
  b = std::min(b, x.size());
  for (size_t i = a + 1; i < b; ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]);
      if (first < 0) first = t;
      last = t;
      ++count;
    }
  }
  return count >= 2 ? double(count - 1) * fs / (last - first) : 0.0;
}

// Simple deterministic RNG for the sweep tests.
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed) {}
  uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
  int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }
};

// Patch helpers used by several tests.
inline void sinePatch(Synth& s) {   // one pure sine per voice, filter wide open, flat sustain
  using namespace pg::lead;
  s.set(kPWave, kWaveFm1); s.set(kPShape, 0); s.set(kPOscMix, 0); s.set(kPDetune, 0);
  s.set(kPCutoff, 127); s.set(kPReso, 0); s.set(kPEnvAmt, 0); s.set(kPKeyTrack, 0);
  s.set(kPSustain, 127); s.set(kPVibrato, 0); s.set(kPWidth, 0);
}
inline void openFilter(Synth& s) {
  using namespace pg::lead;
  s.set(kPCutoff, 127); s.set(kPReso, 0); s.set(kPEnvAmt, 0); s.set(kPKeyTrack, 0); s.set(kPSustain, 127);
}

inline double midiHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }
inline double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

}  // namespace lt
