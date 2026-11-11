// Shared helpers for the drum tests (not a test itself: only test_*.cpp are built).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
#include "pg/drums/drums.h"

namespace dt {

using pg::kQuantum;
using pg::kSoundParams;

struct Params {
  uint8_t v[kSoundParams];
  uint8_t& operator[](int i) { return v[i]; }
};

inline Params defaults(const pg::Voice& voice) {
  Params p;
  for (int i = 0; i < kSoundParams; ++i) {
    const pg::ParamInfo* pi = voice.paramInfo(i);
    p.v[i] = pi ? pi->def : 0;
  }
  return p;
}
inline int numParams(const pg::Voice& voice) {
  int n = 0;
  while (n < kSoundParams && voice.paramInfo(n)) ++n;
  return n;
}

struct Event {
  int quantum;       // when (in quanta from the start)
  uint8_t note, vel;
  bool accent;
  bool choke;        // true: call choke() instead of noteOn
};
inline Event hit(float sec, float fs, uint8_t vel = 127, uint8_t note = 60, bool accent = false) {
  return Event{int(sec * fs / float(kQuantum) + 0.5f), note, vel, accent, false};
}
inline Event chokeAt(float sec, float fs) {
  return Event{int(sec * fs / float(kQuantum) + 0.5f), 60, 0, false, true};
}

// Counts samples where L != R (the drum voices are mono; the codec is too).
inline long& monoMismatches() { static long n = 0; return n; }

// Drive a voice the way the engine does: per quantum setParams -> events ->
// render if active (else silence). Returns the left channel; checks L == R.
// If `activeLog` is given it receives active() after each quantum.
inline std::vector<float> run(pg::Voice& voice, const Params& params, const std::vector<Event>& events,
                              float seconds, float fs, std::vector<char>* activeLog = nullptr) {
  const int quanta = int(seconds * fs / float(kQuantum)) + 1;
  std::vector<float> out;
  out.reserve(size_t(quanta) * kQuantum);
  float l[kQuantum], r[kQuantum];
  for (int q = 0; q < quanta; ++q) {
    voice.setParams(params.v);
    for (const Event& e : events) {
      if (e.quantum != q) continue;
      if (e.choke) voice.choke();
      else voice.noteOn(pg::NoteEvent{e.note, e.vel, e.accent, false, 0});
    }
    if (voice.active()) {
      voice.render(l, r, kQuantum);
      for (int i = 0; i < kQuantum; ++i) {
        if (l[i] != r[i]) ++monoMismatches();
        out.push_back(l[i]);
      }
    } else {
      for (int i = 0; i < kQuantum; ++i) out.push_back(0.0f);
    }
    if (activeLog) activeLog->push_back(voice.active() ? 1 : 0);
  }
  return out;
}

inline float peak(const std::vector<float>& x, size_t from = 0, size_t to = size_t(-1)) {
  float p = 0.0f;
  if (to > x.size()) to = x.size();
  for (size_t i = from; i < to; ++i) p = std::fabs(x[i]) > p ? std::fabs(x[i]) : p;
  return p;
}
inline float rms(const std::vector<float>& x, size_t from = 0, size_t to = size_t(-1)) {
  if (to > x.size()) to = x.size();
  if (to <= from) return 0.0f;
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += double(x[i]) * double(x[i]);
  return float(std::sqrt(s / double(to - from)));
}
inline float mean(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += double(v);
  return x.empty() ? 0.0f : float(s / double(x.size()));
}
inline float maxJump(const std::vector<float>& x, size_t from = 1, size_t to = size_t(-1)) {
  float j = 0.0f;
  if (to > x.size()) to = x.size();
  if (from < 1) from = 1;
  for (size_t i = from; i < to; ++i) {
    const float d = std::fabs(x[i] - x[i - 1]);
    j = d > j ? d : j;
  }
  return j;
}
inline bool allFinite(const std::vector<float>& x) {
  for (float v : x) if (!std::isfinite(v)) return false;
  return true;
}
inline float dB(float x) { return 20.0f * std::log10(x > 1e-12f ? x : 1e-12f); }

// ---- spectra -------------------------------------------------------------
// Power at one frequency (Goertzel), for fine-grained looks at a narrow band.
inline double goertzelPower(const std::vector<float>& x, size_t from, size_t n, double freq, double fs) {
  const double w = 2.0 * 3.14159265358979323846 * freq / fs;
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double s = double(x[from + i]) + c * s1 - s2;
    s2 = s1;
    s1 = s;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}

// Power spectrum of x[from, from+n) by a plain radix-2 FFT (zero padded), so
// band sums are true energy fractions. Drum hits start at full level and die
// away, so no symmetric window: flat, with a raised-cosine fade over the last
// quarter to keep truncation leakage out of the bands.
struct Spectrum {
  std::vector<double> power;  // bins 0 .. N/2
  double binHz = 1.0;
};
inline Spectrum spectrum(const std::vector<float>& x, double fs, size_t from = 0, size_t n = size_t(-1)) {
  Spectrum sp;
  if (from > x.size()) from = x.size();
  if (n > x.size() - from) n = x.size() - from;
  size_t N = 1024;
  while (N < n) N <<= 1;
  std::vector<double> re(N, 0.0), im(N, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const double u = double(i) / double(n > 1 ? n - 1 : 1);
    const double win = u < 0.75 ? 1.0 : 0.5 + 0.5 * std::cos(3.14159265358979323846 * (u - 0.75) / 0.25);
    re[i] = double(x[from + i]) * win;
  }
  for (size_t i = 1, j = 0; i < N; ++i) {  // bit reversal
    size_t bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (size_t len = 2; len <= N; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979323846 / double(len);
    const double wr = std::cos(ang), wi = std::sin(ang);
    for (size_t i = 0; i < N; i += len) {
      double cr = 1.0, ci = 0.0;
      for (size_t k = 0; k < len / 2; ++k) {
        const size_t a = i + k, b = i + k + len / 2;
        const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
  sp.power.resize(N / 2 + 1);
  for (size_t k = 0; k <= N / 2; ++k) sp.power[k] = re[k] * re[k] + im[k] * im[k];
  sp.binHz = fs / double(N);
  return sp;
}
// Fraction of the total energy that lies in [f0, f1) Hz.
inline double bandFraction(const Spectrum& sp, double f0, double f1) {
  double tot = 0.0, in = 0.0;
  for (size_t k = 0; k < sp.power.size(); ++k) {
    const double f = double(k) * sp.binHz;
    tot += sp.power[k];
    if (f >= f0 && f < f1) in += sp.power[k];
  }
  return tot > 0.0 ? in / tot : 0.0;
}
inline double centroid(const Spectrum& sp) {
  double tot = 0.0, m = 0.0;
  for (size_t k = 0; k < sp.power.size(); ++k) {
    tot += sp.power[k];
    m += sp.power[k] * double(k) * sp.binHz;
  }
  return tot > 0.0 ? m / tot : 0.0;
}

// Fundamental from positive-going zero crossings in [from, to), in Hz.
inline float zeroCrossFreq(const std::vector<float>& x, size_t from, size_t to, float fs) {
  if (to > x.size()) to = x.size();
  double first = -1.0, last = -1.0;
  int count = 0;
  for (size_t i = from + 1; i < to; ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]);
      if (first < 0.0) first = t;
      last = t;
      ++count;
    }
  }
  if (count < 2) return 0.0f;
  return float(double(count - 1) * double(fs) / (last - first));
}

// Tiny test bookkeeping.
struct Checker {
  int failures = 0;
  void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("  FAIL: %s\n", what); }
  }
  void range(float v, float lo, float hi, const char* what) {
    const bool ok = v >= lo && v <= hi;
    std::printf("  %-44s %10.4f  [%g .. %g]%s\n", what, double(v), double(lo), double(hi), ok ? "" : "  <-- FAIL");
    if (!ok) ++failures;
  }
  int done(const char* name) {
    std::printf("%s: %s (%d failure%s)\n", name, failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
  }
};

}  // namespace dt
