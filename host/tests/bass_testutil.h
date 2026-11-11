// Shared helpers for the bass voice tests: a tiny step sequencer that drives a
// BassVoice the way the engine will (events on quantum edges), plus the
// measurement tools the tests "listen" with (FFT, pitch tracking, levels).
#pragma once
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/wav.h"
#include "pg/bass/bass.h"

namespace bt {

using pg::bass::BassVoice;
constexpr double kPiD = 3.14159265358979323846;

// ---------------------------------------------------------------- voice driver
struct Rig {
  BassVoice v;
  float sr;
  uint8_t p[pg::kSoundParams];
  std::vector<float> out;      // mono
  std::vector<float> cut;      // debugCutoffHz per quantum
  std::vector<float> pitch;    // debugPitch per quantum

  explicit Rig(float sampleRate = 44100.0f, float bpm = 126.0f) : sr(sampleRate) {
    v.init(sr);
    defaults();
    v.setTempo(bpm);
  }
  void defaults() {
    for (int i = 0; i < pg::kSoundParams; ++i) {
      const pg::ParamInfo* pi = v.paramInfo(i);
      p[i] = pi ? pi->def : uint8_t(0);
    }
  }
  void set(int idx, int val) { p[idx] = uint8_t(val); }
  void on(int note, int vel = 127, bool accent = false, bool slide = false) {
    pg::NoteEvent e{uint8_t(note), uint8_t(vel), accent, slide, 0};
    v.setParams(p);
    v.noteOn(e);
  }
  void off(int note) { v.noteOff(uint8_t(note)); }
  // Render `quanta` control blocks (16 frames each).
  void run(int quanta) {
    float l[pg::kQuantum], r[pg::kQuantum];
    for (int q = 0; q < quanta; ++q) {
      v.setParams(p);
      if (v.active()) {
        v.render(l, r, pg::kQuantum);
      } else {
        for (int i = 0; i < pg::kQuantum; ++i) l[i] = r[i] = 0.0f;
      }
      for (int i = 0; i < pg::kQuantum; ++i) out.push_back(l[i]);
      cut.push_back(v.debugCutoffHz());
      pitch.push_back(v.debugPitch());
    }
  }
  void runSeconds(float s) { run(int(s * sr / float(pg::kQuantum) + 0.5f)); }
  int quantaFor(float seconds) const { return int(seconds * sr / float(pg::kQuantum) + 0.5f); }
  size_t pos() const { return out.size(); }
};

// One 16th-note step of a pattern.
struct StepEv {
  int note;        // 0 = rest
  bool accent;
  bool slide;      // glide INTO the next step (gate held through), 303 style
  int vel;
  float gate;      // fraction of the step (ignored when slide)
};

// Play `steps` as 16ths at `bpm`, calling `automate(stepIndex, fractionOfPattern)`
// once per quantum so tests/renders can sweep params.
template <class F>
inline void playPattern(Rig& rig, const std::vector<StepEv>& steps, float bpm, int repeats, F automate) {
  rig.v.setTempo(bpm);
  const double stepSamples = double(rig.sr) * 60.0 / double(bpm) / 4.0;
  const int total = int(steps.size()) * repeats;
  double t = 0.0;            // in samples
  long rendered = 0;         // quanta
  bool prevSlide = false;
  int prevNote = 0;
  for (int s = 0; s < total; ++s) {
    const StepEv& e = steps[size_t(s) % steps.size()];
    const long startQ = long(t / pg::kQuantum + 0.5);
    const long endQ = long((t + stepSamples) / pg::kQuantum + 0.5);
    long offQ = startQ + long(double(e.gate) * stepSamples / pg::kQuantum + 0.5);
    if (offQ >= endQ) offQ = endQ;       // full-length gate: release exactly at the next step
    for (long q = startQ; q < endQ; ++q) {
      automate(s, float(double(q) * pg::kQuantum / (stepSamples * total)));
      if (q == startQ) {
        if (e.note) {
          rig.on(e.note, e.vel, e.accent, prevSlide && prevNote != 0);
        } else if (prevNote && prevSlide) {
          rig.off(prevNote);
        }
      }
      if (q == offQ && e.note && !e.slide) rig.off(e.note);
      rig.run(1);
      ++rendered;
    }
    if (e.note && !e.slide && offQ >= endQ) rig.off(e.note);
    prevSlide = e.note && e.slide;
    prevNote = e.note;
    t += stepSamples;
  }
  if (prevNote) rig.off(prevNote);
  (void)rendered;
}

// ---------------------------------------------------------------- file output
inline void ensureDir(const char* path) {
  std::string s(path), cur;
  for (size_t i = 0; i < s.size(); ++i) {
    cur.push_back(s[i]);
    if (s[i] == '/' || i + 1 == s.size()) mkdir(cur.c_str(), 0755);
  }
}
inline bool writeMono(const std::string& path, const std::vector<float>& mono, float sr) {
  std::vector<float> st(mono.size() * 2);
  for (size_t i = 0; i < mono.size(); ++i) st[2 * i] = st[2 * i + 1] = mono[i];
  return wav::writeFloat(path.c_str(), st, int(sr), 2);
}

// ---------------------------------------------------------------- measurement
inline double dB(double x) { return 20.0 * std::log10(x > 1e-12 ? x : 1e-12); }

inline double peak(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  if (b > x.size()) b = x.size();
  double m = 0;
  for (size_t i = a; i < b; ++i) m = std::fmax(m, std::fabs(double(x[i])));
  return m;
}
inline double rms(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  if (b > x.size()) b = x.size();
  if (b <= a) return 0;
  double s = 0;
  for (size_t i = a; i < b; ++i) s += double(x[i]) * double(x[i]);
  return std::sqrt(s / double(b - a));
}
inline double mean(const std::vector<float>& x, size_t a = 0, size_t b = size_t(-1)) {
  if (b > x.size()) b = x.size();
  if (b <= a) return 0;
  double s = 0;
  for (size_t i = a; i < b; ++i) s += double(x[i]);
  return s / double(b - a);
}
inline bool allFinite(const std::vector<float>& x) {
  for (float v : x)
    if (!std::isfinite(v)) return false;
  return true;
}
inline double maxStep(const std::vector<float>& x, size_t a = 1, size_t b = size_t(-1)) {
  if (b > x.size()) b = x.size();
  if (a < 1) a = 1;
  double m = 0;
  for (size_t i = a; i < b; ++i) m = std::fmax(m, std::fabs(double(x[i]) - double(x[i - 1])));
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
    const double ang = -2.0 * kPiD / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], t = a[i + k + len / 2] * w;
        a[i + k] = u + t;
        a[i + k + len / 2] = u - t;
        w *= wl;
      }
    }
  }
}

// Magnitude spectrum of x[start .. start+n) with a 4-term Blackman-Harris
// window (side lobes -92 dB), scaled so a full-scale sine reads 1.0.
inline std::vector<double> spectrum(const std::vector<float>& x, size_t start, size_t n) {
  std::vector<std::complex<double>> a(n);
  double wsum = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = 2.0 * kPiD * double(i) / double(n);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    wsum += w;
    a[i] = std::complex<double>((start + i < x.size() ? double(x[start + i]) : 0.0) * w, 0.0);
  }
  fft(a);
  std::vector<double> m(n / 2);
  for (size_t i = 0; i < n / 2; ++i) m[i] = std::abs(a[i]) * 2.0 / wsum;
  return m;
}

// Magnitude of the Hann-windowed DTFT at an arbitrary frequency.
inline double dtftMag(const std::vector<float>& x, size_t start, size_t n, double freq, double sr) {
  double re = 0, im = 0;
  const double w = 2.0 * kPiD * freq / sr;
  for (size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2.0 * kPiD * double(i) / double(n));
    const double v = double(x[start + i]) * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
  }
  return std::sqrt(re * re + im * im) * 4.0 / double(n);
}

// Refine a frequency estimate near `guess` (within +-`spanSemis`) by maximising the DTFT.
inline double findPeakFreq(const std::vector<float>& x, size_t start, size_t n, double guess, double sr,
                           double spanSemis = 1.0) {
  double lo = guess * std::pow(2.0, -spanSemis / 12.0), hi = guess * std::pow(2.0, spanSemis / 12.0);
  // coarse scan, then golden-section
  double best = guess, bestMag = -1;
  const int scan = 60;
  for (int i = 0; i <= scan; ++i) {
    const double f = lo * std::pow(hi / lo, double(i) / scan);
    const double m = dtftMag(x, start, n, f, sr);
    if (m > bestMag) { bestMag = m; best = f; }
  }
  double a = best * std::pow(hi / lo, -1.0 / scan), b = best * std::pow(hi / lo, 1.0 / scan);
  const double gr = 0.6180339887498949;
  double c = b - gr * (b - a), d = a + gr * (b - a);
  double fc = dtftMag(x, start, n, c, sr), fd = dtftMag(x, start, n, d, sr);
  for (int it = 0; it < 40; ++it) {
    if (fc > fd) { b = d; d = c; fd = fc; c = b - gr * (b - a); fc = dtftMag(x, start, n, c, sr); }
    else { a = c; c = d; fc = fd; d = a + gr * (b - a); fd = dtftMag(x, start, n, d, sr); }
  }
  return 0.5 * (a + b);
}

inline double midiHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }
inline double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

// Spectral centroid (Hz) of a segment.
inline double centroid(const std::vector<float>& x, size_t start, size_t n, double sr) {
  std::vector<double> m = spectrum(x, start, n);
  double num = 0, den = 0;
  for (size_t i = 1; i < m.size(); ++i) {
    const double p = m[i] * m[i];
    num += p * double(i) * sr / double(n);
    den += p;
  }
  return den > 0 ? num / den : 0;
}

// ---------------------------------------------------------------- test plumbing
struct Checker {
  int fails = 0;
  void check(bool ok, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("%s  ", ok ? "  ok " : " FAIL");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!ok) ++fails;
  }
  int result(const char* name) const {
    std::printf("%s: %s (%d failure%s)\n", name, fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
  }
};

}  // namespace bt
