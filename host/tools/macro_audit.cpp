// macro_audit: renders a macro set (.pgm, docs/MACROLAB.md) over a grid of
// macro positions through the real instrument and reports, per set, whether
// every point makes a sane sound and whether every macro is alive along its
// travel. Claude cannot listen; this is how a set gets checked before the
// owner plays it.
//
//   macro_audit [options] PATH...      PATH = a .pgm file or a folder (any depth)
//     --hard          hard checks only, one line per set (the ctest)
//     --points        print every grid point (default: only the summary, the
//                     single-macro lines and the problems)
//     --wav DIR       write every point's render to DIR/<set>_<point>.wav
//     --jump DB       level-jump threshold between neighbours (default 6)
//     --dead DB       dead-travel threshold, spectrogram distance (default 0.5)
//
// THE GRID. Each macro alone at 0, 1/8 .. 1 (the others at their start
// positions), every corner of the macro space, and a 5 x 5 grid of the first
// two macros (the XY pad; the others at their start).
//
// THE PHRASE (per voice, 120 BPM, one 16-step bar plus a second of tail):
//   bass   C2 short, G2 accent + slide into C3, G1 long (soft), G3 accent
//   lead   C4, A3 min7 chord, C5 long, C3 accent + slide into G3
//   drums  hits at velocity 127 (accent), 100, 60, 100 (note +7), 100
// It plays the track alone with the ducker off, through the engine and the
// track's own mixer row; the measurement is the track's stem (what it adds to
// the bus) plus its own share of the delay and reverb returns, before the
// master, as the mono sum (L+R)/2 (the device is mono: WIDTH and PAN moves are
// inaudible there and the audit hears them as dead).
//
// MEASURED per point: peak, loudness (dBFS: the loudest 100 ms, RMS, so a
// short pluck and a long note are compared by how loud they sound, not by
// their energy), RMS (dBFS, whole render), spectral centroid
// (Hz, power weighted over the render), decay (ms: from the loudest 10 ms in
// the "decay window" until 20 dB below it; synths: the long note, drums: the
// first hit; ">" = never within the window), and a log-band spectrogram used
// to compare neighbours: the mean absolute dB difference over the time-
// frequency cells that are within 60 dB of the louder render's top cell.
//
// HARD FAILURES (exit 1): a parse error, a silent point (peak < -45 dBFS),
// a clipping point (stem peak > 0 dBFS), NaN / Inf, engine faults.
// WARNINGS (reported, exit 0): dead travel (neighbours on a macro's line, or
// on the XY grid, whose spectrogram distance < --dead), a level jump between
// neighbours (|delta loudness| > --jump dB), hot points (peak > -3 dBFS), a
// loudness range across the whole space wider than 18 dB, subnormal samples
// (with where: the track stem, the delay or the reverb return; the engine's
// delay makes them on its default settings today, so they cannot fail a set).
//
// A file can declare a deliberate dead zone or jump so the warning is not
// raised for it: a comment line "# audit: allow-dead NAME" / "# audit:
// allow-jump NAME" (NAME = a macro of the set, or * for all).
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/bounce.h"
#include "common/wav.h"
#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::macro;

namespace {

pg::Instrument g_inst;
ParamTable g_pt;
uint8_t g_defaults[kNumTracks][kParamsPerTrack];

struct Opts {
  bool hard = false, points = false;
  std::string wavDir;
  float jumpDb = 6.0f, deadDb = 0.5f;
} g_o;

constexpr float kSr = 44100.0f;
constexpr int kBpm = 120;
constexpr long kStepFrames = long(kSr * 15.0f / float(kBpm));   // one 16th
constexpr int kFft = 2048, kHop = 512, kBands = 96;
constexpr float kSilentDb = -45.0f;   // hard: a point whose peak stays below this
constexpr float kHotDb = -3.0f;       // warn: a point whose peak goes above this
constexpr float kRangeDb = 18.0f;     // warn: loudness range across the whole space

// ---- the phrase ------------------------------------------------------------------
Step note(int n, int vel, int gate, uint8_t flags = 0, int chord = 0) {
  Step s = kDefaultStep;
  s.note = uint8_t(n);
  s.vel = uint8_t(vel);
  s.gate = uint8_t(gate);
  s.flags = uint8_t(kStepOn | flags);
  s.chord = uint8_t(chord);
  return s;
}

// Fills pattern 0 of `p` with the phrase for `track`; returns the decay window in steps.
void writePhrase(Project& p, int track, int* decayFrom, int* decayTo) {
  TrackPattern& tp = p.patterns[0].tracks[track];
  p.patterns[0].length = 16;
  tp.length = 16;
  for (int i = 0; i < kMaxSteps; ++i) tp.steps[i] = kDefaultStep;
  if (track == kTrackBass) {
    tp.steps[0] = note(36, 100, 2);
    tp.steps[3] = note(43, 110, 4, kStepAccent | kStepSlide);
    tp.steps[4] = note(48, 100, 2);
    tp.steps[8] = note(31, 80, 14);
    tp.steps[12] = note(55, 120, 2, kStepAccent);
    *decayFrom = 8;
    *decayTo = 12;
  } else if (track == kTrackLead) {
    tp.steps[0] = note(60, 100, 3);
    tp.steps[2] = note(57, 100, 4, 0, kChordMin7);
    tp.steps[6] = note(72, 90, 20);
    tp.steps[12] = note(48, 120, 4, kStepAccent | kStepSlide);
    tp.steps[13] = note(55, 100, 4);
    *decayFrom = 6;
    *decayTo = 12;
  } else {
    tp.steps[0] = note(60, 127, 2, kStepAccent);
    tp.steps[4] = note(60, 100, 2);
    tp.steps[8] = note(60, 60, 2);
    tp.steps[11] = note(67, 100, 2);
    tp.steps[14] = note(60, 100, 2);
    *decayFrom = 0;
    *decayTo = 4;
  }
}

// ---- a tiny FFT ------------------------------------------------------------------
void fft(std::vector<std::complex<float>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * 3.14159265f / float(len);
    const std::complex<float> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// ---- one grid point ---------------------------------------------------------------
struct Point {
  float pos[kMaxMacros] = {};
  std::string label;
  uint8_t params[kParamsPerTrack] = {};
  // measurements
  float peakDb = -200, rmsDb = -200, loudDb = -200, centroid = 0, hfDb = -200, decayMs = 0;
  bool decayOpen = false;     // never fell 20 dB inside the window
  bool nonFinite = false, subnormal = false;
  unsigned subnormalIn = 0;   // bit 0: the track stem, 1: delay return, 2: reverb return
  uint32_t faults = 0;
  std::vector<float> spec;    // frames x kBands, dB
  int frames = 0;
};

float db(double x) { return float(10.0 * std::log10(x + 1e-20)); }

bool render(int track, Point& pt) {
  static Project proj;
  g_inst.engine().initProject(proj);
  proj.globals[kGTempo] = kBpm;
  proj.globals[kGSwing] = 0;
  for (int t = 0; t < kNumTracks; ++t) proj.tracks[t].mute = t == track ? 0 : 1;
  std::memcpy(proj.tracks[track].params, pt.params, kParamsPerTrack);
  int dFrom = 0, dTo = 4;
  writePhrase(proj, track, &dFrom, &dTo);

  bounce::Options o;
  o.pattern = 0;
  o.loops = 1;
  o.tailSec = 1.0f;
  o.sampleRate = kSr;
  bounce::Plan plan;
  std::string err;
  if (!bounce::makePlan(proj, o, plan, &err)) {
    std::fprintf(stderr, "plan: %s\n", err.c_str());
    return false;
  }
  bounce::Pass pass;
  pass.noDuck = true;
  pass.sendMask = 1u << track;   // its own delay / reverb share: a send macro is heard
  std::vector<float> mono;
  bounce::PassResult res;
  const bool ok = bounce::renderPass(
      g_inst, proj, o, plan, pass, 0,
      [&](const bounce::Block& b) {
        for (int i = 0; i < b.n; ++i) {
          const float src[6] = {b.track[track][0][i], b.track[track][1][i], b.delay[0][i], b.delay[1][i],
                                b.reverb[0][i], b.reverb[1][i]};
          for (int k = 0; k < 6; ++k)
            if (src[k] != 0.0f && std::fpclassify(src[k]) == FP_SUBNORMAL) pt.subnormalIn |= 1u << (k / 2);
        }
        for (int i = 0; i < b.n; ++i)
          mono.push_back(0.5f * (b.track[track][0][i] + b.track[track][1][i] + b.delay[0][i] + b.delay[1][i] +
                                 b.reverb[0][i] + b.reverb[1][i]));
      },
      res, &err);
  if (!ok) {
    std::fprintf(stderr, "render: %s\n", err.c_str());
    return false;
  }
  pt.faults = res.faults;

  // peak, RMS, sanity
  double sq = 0.0;
  float peak = 0.0f;
  for (float x : mono) {
    if (!std::isfinite(x)) { pt.nonFinite = true; continue; }
    if (x != 0.0f && std::fpclassify(x) == FP_SUBNORMAL) pt.subnormal = true;
    peak = std::max(peak, std::fabs(x));
    sq += double(x) * double(x);
  }
  pt.peakDb = 20.0f * std::log10(peak + 1e-10f);
  pt.rmsDb = db(sq / double(std::max<size_t>(1, mono.size())));
  // loudness: the loudest 100 ms (RMS, 25 ms hop): how loud the notes are, whatever their length
  {
    const size_t w = size_t(kSr * 0.1f), hop = size_t(kSr * 0.025f);
    double best = 0.0;
    for (size_t s = 0; s + w <= mono.size(); s += hop) {
      double e = 0.0;
      for (size_t k = 0; k < w; ++k) {
        const float x = std::isfinite(mono[s + k]) ? mono[s + k] : 0.0f;
        e += double(x) * double(x);
      }
      best = std::max(best, e / double(w));
    }
    pt.loudDb = db(best);
  }

  // decay: 10 ms RMS windows inside the decay window
  {
    const long a = dFrom * kStepFrames, b = std::min<long>(long(mono.size()), dTo * kStepFrames);
    const int w = int(kSr * 0.01f);
    std::vector<float> env;
    for (long i = a; i + w <= b; i += w) {
      double e = 0.0;
      for (int k = 0; k < w; ++k) e += double(mono[size_t(i + k)]) * double(mono[size_t(i + k)]);
      env.push_back(db(e / w));
    }
    size_t top = 0;
    for (size_t i = 0; i < env.size(); ++i)
      if (env[i] > env[top]) top = i;
    pt.decayOpen = true;
    pt.decayMs = float(env.size()) * 10.0f;
    for (size_t i = top; i < env.size(); ++i)
      if (env[i] < env[top] - 20.0f) {
        pt.decayMs = float(i - top) * 10.0f;
        pt.decayOpen = false;
        break;
      }
  }

  // spectrogram + centroid
  {
    static float hann[kFft];
    static int bandOf[kFft / 2];
    static bool init = false;
    if (!init) {
      for (int i = 0; i < kFft; ++i) hann[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * float(i) / float(kFft));
      for (int k = 0; k < kFft / 2; ++k) {
        const float f = float(k) * kSr / float(kFft);
        const float x = std::log2(std::max(f, 40.0f) / 40.0f) / std::log2(16000.0f / 40.0f);
        bandOf[k] = std::min(kBands - 1, std::max(0, int(x * float(kBands))));
      }
      init = true;
    }
    std::vector<std::complex<float>> buf(kFft);
    double num = 0.0, den = 0.0, hf = 0.0;
    pt.spec.clear();
    pt.frames = 0;
    for (size_t s = 0; s + kFft <= mono.size(); s += kHop) {
      for (int i = 0; i < kFft; ++i) {
        const float x = mono[s + size_t(i)];
        buf[size_t(i)] = std::complex<float>(std::isfinite(x) ? x * hann[i] : 0.0f, 0.0f);
      }
      fft(buf);
      double band[kBands] = {};
      for (int k = 1; k < kFft / 2; ++k) {
        const double pw = double(std::norm(buf[size_t(k)]));
        band[bandOf[k]] += pw;
        num += pw * double(k) * double(kSr) / double(kFft);
        den += pw;
        if (k * kSr >= 1500.0f * kFft) hf += pw;
      }
      for (int bnd = 0; bnd < kBands; ++bnd) pt.spec.push_back(db(band[bnd]));
      ++pt.frames;
    }
    pt.centroid = den > 0.0 ? float(num / den) : 0.0f;
    pt.hfDb = den > 0.0 ? db(hf / den) : -200.0f;
  }

  if (!g_o.wavDir.empty()) {
    std::string name = g_o.wavDir + "/" + pt.label + ".wav";
    for (char& c : name) if (c == ' ' || c == '=' || c == ',') c = '_';
    wav::writeFloat(name.c_str(), mono, int(kSr), 1);
  }
  return true;
}

// Spectrogram distance (dB): the mean |a - b| over the time-frequency cells
// within 60 dB of the louder render's top cell, each cell weighted by its
// amplitude to the power 1/2 (the louder of the two; a cell 40 dB down counts a
// tenth), so the loud part of the sound counts most, the top end still counts
// and the noise floor of a tail does not.
float distance(const Point& a, const Point& b) {
  const size_t n = std::min(a.spec.size(), b.spec.size());
  float top = -200.0f;
  for (size_t i = 0; i < n; ++i) top = std::max(top, std::max(a.spec[i], b.spec[i]));
  const float floor = top - 60.0f;
  double sum = 0.0, wsum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float x = std::max(a.spec[i], floor), y = std::max(b.spec[i], floor);
    if (x <= floor && y <= floor) continue;
    const double w = std::pow(10.0, double(std::max(x, y) - top) / 40.0);
    sum += w * std::fabs(double(x) - double(y));
    wsum += w;
  }
  return wsum > 0.0 ? float(sum / wsum) : 0.0f;
}

// ---- one set ----------------------------------------------------------------------
std::string readFile(const std::string& path) {
  std::string text;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return text;
  char b[4096];
  size_t n;
  while ((n = std::fread(b, 1, sizeof(b), fp)) > 0) text.append(b, n);
  std::fclose(fp);
  return text;
}

// "# audit: allow-dead NAME" lines
void allowances(const std::string& text, std::set<std::string>* dead, std::set<std::string>* jump) {
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    i = e + 1;
    char kind[32], name[32];
    const size_t h = line.find('#');
    if (h == std::string::npos) continue;
    if (std::sscanf(line.c_str() + h, "# audit: allow-%31s %31s", kind, name) == 2) {
      std::string n = name;
      for (char& c : n) c = char(std::toupper(c));
      if (!std::strcmp(kind, "dead")) dead->insert(n);
      if (!std::strcmp(kind, "jump")) jump->insert(n);
    }
  }
}

struct Summary {
  std::string path, name, voice;
  int points = 0;
  float loMin = 0, loMax = 0, pkMax = 0, cenMin = 0, cenMax = 0;
  int hard = 0, warn = 0;
  std::string travel;
};
std::vector<Summary> g_sum;

std::string posLabel(const MacroSet& s, const float* pos) {
  std::string l;
  char b[32];
  for (int i = 0; i < s.numMacros; ++i) {
    std::snprintf(b, sizeof(b), "%s%s=%.2f", i ? " " : "", s.macros[i].name, double(pos[i]));
    l += b;
  }
  return l;
}

void printPoint(const Point& p, const char* tag) {
  std::printf("    %-34s %6.1f %6.1f %6.1f %6.0f %6.1f %5.0f%s %s\n", p.label.c_str(), double(p.peakDb),
              double(p.loudDb), double(p.rmsDb), double(p.centroid), double(p.hfDb), double(p.decayMs), p.decayOpen ? ">" : " ",
              tag);
}

// A trim line per macro that would hold the loudness along its line (the others at
// their start) at what it is at the macro's start position: the existing trims of
// that macro plus the measured difference, at 0, 1/4 .. 1, rounded to 0.5 dB.
// Printed ready to paste (in place of that macro's own trim lines) when the line
// strays more than 2 dB from its start. Whether flat is right is the author's call:
// a "more" macro may be meant to get louder.
void suggestTrims(const MacroSet& s, const std::vector<Point>& pts, const std::vector<std::vector<size_t>>& lines,
                  const float* start) {
  for (int m = 0; m < s.numMacros; ++m) {
    const std::vector<size_t>& L = lines[size_t(m)];
    if (L.size() < 9) continue;
    auto loudAt = [&](float x) {   // linear between the line's 1/8 points
      const float f = x * 8.0f;
      const int k = std::min(7, int(f));
      return pts[L[size_t(k)]].loudDb + (pts[L[size_t(k + 1)]].loudDb - pts[L[size_t(k)]].loudDb) * (f - float(k));
    };
    const float ref = loudAt(start[m]);
    float worst = 0.0f;
    for (size_t k = 0; k < L.size(); ++k) worst = std::max(worst, std::fabs(pts[L[k]].loudDb - ref));
    if (worst <= 2.0f) continue;
    std::string line = "  trim";
    bool boost = false;
    for (int q = 0; q <= 4; ++q) {
      const float x = float(q) / 4.0f;
      float have = 0.0f;   // this macro's own trims (not scaled by another macro)
      for (int k = 0; k < s.numTrims; ++k)
        if (s.trims[k].macro == m && s.trims[k].by == kNoMacro)
          have += curveAt(s.points + s.trims[k].first, s.trims[k].n, Interp(s.trims[k].interp), x);
      float db = have - (pts[L[size_t(q * 2)]].loudDb - ref);
      db = std::round(db * 2.0f) / 2.0f;
      db = std::max(-float(kTrimMaxDb), std::min(float(kTrimMaxDb), db));
      boost |= db > 2.0f;
      char t[32];
      std::snprintf(t, sizeof(t), " %g:%g", double(x), double(db == 0.0f ? 0.0f : db));
      line += t;
    }
    std::printf("  suggest, macro %s (strays up to %.1f dB from its start's %.1f dB)%s; paste under it:\n%s\n",
                s.macros[m].name, double(worst), double(ref),
                boost ? " (a boost over +2 dB may hit the level's ceiling)" : "", line.c_str());
  }
}

int auditFile(const std::string& path) {
  const std::string text = readFile(path);
  static MacroSet s;
  ParseError e;
  Warnings pw;
  Summary sm;
  sm.path = path;
  if (!parse(text.data(), text.size(), g_pt, &s, &e, &pw)) {
    std::printf("%s:%d: %s\n", path.c_str(), e.line, e.msg);
    sm.hard = 1;
    g_sum.push_back(sm);
    return 1;
  }
  std::set<std::string> allowDead, allowJump;
  allowances(text, &allowDead, &allowJump);
  sm.name = s.name[0] ? s.name : path;
  sm.voice = trackName(s.track);
  const int track = s.track;
  const int nm = s.numMacros;

  // the grid
  std::vector<Point> pts;
  std::map<std::string, size_t> index;
  auto add = [&](const float* pos) -> size_t {
    Point p;
    for (int i = 0; i < nm; ++i) p.pos[i] = pos[i];
    p.label = posLabel(s, pos);
    auto it = index.find(p.label);
    if (it != index.end()) return it->second;
    evaluate(s, g_pt, p.pos, g_defaults[track], p.params);
    // as an instrument plays it: the mixer rows are the player's (here at their defaults),
    // combined with the set's level and sends by the mixer rule (docs/MACROLAB.md)
    combineMixer(g_defaults[track], p.params, g_defaults[track], p.params);
    index[p.label] = pts.size();
    pts.push_back(p);
    return pts.size() - 1;
  };
  float start[kMaxMacros] = {};
  for (int i = 0; i < nm; ++i) start[i] = s.macros[i].start();
  std::vector<std::vector<size_t>> lines(static_cast<size_t>(nm));
  for (int m = 0; m < nm; ++m)
    for (int k = 0; k <= 8; k += g_o.hard ? 2 : 1) {   // --hard: 0, 0.25 .. 1 is enough
      float pos[kMaxMacros];
      std::memcpy(pos, start, sizeof(pos));
      pos[m] = float(k) / 8.0f;
      lines[size_t(m)].push_back(add(pos));
    }
  for (int c = 0; c < (1 << nm); ++c) {
    float pos[kMaxMacros] = {};
    for (int i = 0; i < nm; ++i) pos[i] = (c >> i) & 1 ? 1.0f : 0.0f;
    add(pos);
  }
  std::vector<std::vector<size_t>> grid;
  if (nm >= 2) {
    grid.assign(5, std::vector<size_t>(5));
    for (int x = 0; x < 5; ++x)
      for (int y = 0; y < 5; ++y) {
        float pos[kMaxMacros];
        std::memcpy(pos, start, sizeof(pos));
        pos[0] = float(x) / 4.0f;
        pos[1] = float(y) / 4.0f;
        grid[size_t(x)][size_t(y)] = add(pos);
      }
  }
  for (Point& p : pts) {
    if (!render(track, p)) {
      sm.hard++;
      continue;
    }
  }

  // hard checks
  std::vector<std::string> problems;
  char b[400];
  // the parser's warnings (a plain set wiping out an earlier line, an unused patch)
  for (int k = 0; k < pw.count && k < kMaxWarnings; ++k) {
    std::snprintf(b, sizeof(b), "warn  %s:%d: %s", path.c_str(), pw.w[k].line, pw.w[k].msg);
    problems.push_back(b);
    sm.warn++;
  }
  if (pw.count > kMaxWarnings) {
    std::snprintf(b, sizeof(b), "warn  %d more parser warning(s)", pw.count - kMaxWarnings);
    problems.push_back(b);
    sm.warn++;
  }
  unsigned subWhere = 0;
  int subPoints = 0;
  for (const Point& p : pts) {
    if (p.subnormal || p.subnormalIn) {
      subWhere |= p.subnormalIn;
      ++subPoints;
    }
    const char* why = nullptr;
    if (p.nonFinite) why = "NaN/Inf in the output";
    else if (p.faults) why = "engine faults";
    else if (p.peakDb < kSilentDb) why = "silent";
    else if (p.peakDb > 0.0f) why = "clips (stem peak above 0 dBFS)";
    if (why) {
      std::snprintf(b, sizeof(b), "HARD  %s at %s (peak %.1f dB)", why, p.label.c_str(), double(p.peakDb));
      problems.push_back(b);
      sm.hard++;
    }
  }
  // warnings
  if (subPoints) {
    // A warning, not a failure: today the engine's own delay line makes them on
    // the default patches (its tone filter's one-pole states decay into the
    // subnormal range once the input goes quiet), which no set can avoid.
    std::snprintf(b, sizeof(b), "warn  subnormal samples at %d point(s), in:%s%s%s", subPoints,
                  subWhere & 1 ? " the track stem" : "", subWhere & 2 ? " the delay return" : "",
                  subWhere & 4 ? " the reverb return" : "");
    problems.push_back(b);
    sm.warn++;
  }
  sm.points = int(pts.size());
  sm.loMin = 1e9f;
  sm.loMax = -1e9f;
  sm.pkMax = -1e9f;
  sm.cenMin = 1e9f;
  sm.cenMax = 0.0f;
  int hot = 0;
  const Point* hottest = nullptr;
  const Point *quietest = nullptr, *loudest = nullptr;
  for (const Point& p : pts) {
    if (!quietest || p.loudDb < quietest->loudDb) quietest = &p;
    if (!loudest || p.loudDb > loudest->loudDb) loudest = &p;
    sm.loMin = std::min(sm.loMin, p.loudDb);
    sm.loMax = std::max(sm.loMax, p.loudDb);
    sm.pkMax = std::max(sm.pkMax, p.peakDb);
    sm.cenMin = std::min(sm.cenMin, p.centroid);
    sm.cenMax = std::max(sm.cenMax, p.centroid);
    if (p.peakDb > kHotDb && p.peakDb <= 0.0f) {
      ++hot;
      if (!hottest || p.peakDb > hottest->peakDb) hottest = &p;
    }
  }
  if (hot) {
    std::snprintf(b, sizeof(b), "warn  hot: %d point(s) peak above %.0f dBFS, worst %.1f dB at %s", hot,
                  double(kHotDb), double(hottest->peakDb), hottest->label.c_str());
    problems.push_back(b);
    sm.warn++;
  }
  if (sm.loMax - sm.loMin > kRangeDb) {
    std::snprintf(b, sizeof(b), "warn  loudness range %.1f dB across the space (quietest %s, loudest %s)",
                  double(sm.loMax - sm.loMin), quietest->label.c_str(), loudest->label.c_str());
    problems.push_back(b);
    sm.warn++;
  }
  auto allowed = [](const std::set<std::string>& a, const char* name) { return a.count("*") || a.count(name); };
  auto jump = [&](size_t i, size_t j, const char* macro, const char* axis) {
    const Point &p = pts[i], &q = pts[j];
    const float dl = q.loudDb - p.loudDb;
    if (std::fabs(dl) > g_o.jumpDb && !allowed(allowJump, macro)) {
      std::snprintf(b, sizeof(b), "warn  level jump on %s%s: %s -> %s (%+.1f dB)", macro, axis, p.label.c_str(),
                    q.label.c_str(), double(dl));
      problems.push_back(b);
      sm.warn++;
    }
  };
  // each macro's line: dead stretches (merged), jumps, how far it travels
  for (int m = 0; m < nm; ++m) {
    const std::vector<size_t>& L = lines[size_t(m)];
    const char* name = s.macros[m].name;
    float travel = 0.0f, lo = 1e9f, hi = -1e9f;
    int deadFrom = -1;
    for (size_t k = 0; k < L.size(); ++k) {
      lo = std::min(lo, pts[L[k]].loudDb);
      hi = std::max(hi, pts[L[k]].loudDb);
      const bool last = k + 1 == L.size();
      const float d = last ? 1e9f : distance(pts[L[k]], pts[L[k + 1]]);
      if (!last) {
        travel += d;
        jump(L[k], L[k + 1], name, "");
      }
      const bool dead = d < g_o.deadDb;
      if (dead && deadFrom < 0) deadFrom = int(k);
      if (!dead && deadFrom >= 0) {
        if (!allowed(allowDead, name)) {
          std::snprintf(b, sizeof(b), "warn  dead travel on %s from %.3f to %.3f (others at their start)", name,
                        double(pts[L[size_t(deadFrom)]].pos[m]), double(pts[L[k]].pos[m]));
          problems.push_back(b);
          sm.warn++;
        }
        deadFrom = -1;
      }
    }
    char t[80];
    std::snprintf(t, sizeof(t), "%s%s %.0f", sm.travel.empty() ? "" : "  ", name, double(travel));
    sm.travel += t;
    (void)lo;
    (void)hi;
  }
  // the XY grid: the rows / columns away from the lines already checked
  int padDead[2] = {0, 0};
  for (size_t x = 0; x < grid.size(); ++x)
    for (size_t y = 0; y < grid.size(); ++y) {
      for (int axis = 0; axis < 2; ++axis) {
        const size_t nx = x + (axis == 0), ny = y + (axis == 1);
        if (nx >= grid.size() || ny >= grid.size()) continue;
        const float other = axis == 0 ? float(y) / 4.0f : float(x) / 4.0f;
        if (std::fabs(other - start[1 - axis]) < 1e-4f) continue;
        const size_t i = grid[x][y], j = grid[nx][ny];
        jump(i, j, s.macros[axis].name, " (pad)");
        if (distance(pts[i], pts[j]) < g_o.deadDb && !allowed(allowDead, s.macros[axis].name)) ++padDead[axis];
      }
    }
  for (int axis = 0; axis < 2 && nm >= 2; ++axis)
    if (padDead[axis]) {
      std::snprintf(b, sizeof(b), "warn  dead travel on %s on the pad: %d quarter-steps elsewhere on the pad",
                    s.macros[axis].name, padDead[axis]);
      problems.push_back(b);
      sm.warn++;
    }

  if (g_o.hard) {
    std::printf("%-30s %-5s %-12s %2d points  %s\n", path.c_str(), sm.voice.c_str(), sm.name.c_str(), sm.points,
                sm.hard ? "FAIL" : "ok");
    for (const std::string& pr : problems)
      if (pr.compare(0, 4, "HARD") == 0) std::printf("  %s\n", pr.c_str());
  } else {
    std::printf("\n== %s  (%s %s, %d macros, %d points)\n", path.c_str(), sm.voice.c_str(), sm.name.c_str(), nm,
                sm.points);
    if (s.about[0]) std::printf("   %s\n", s.about);
    std::printf("    %-34s %6s %6s %6s %6s %6s %6s\n", "point", "peak", "loud", "rms", "centr", "hf", "decay");
    for (int m = 0; m < nm; ++m) {
      const Macro& mac = s.macros[m];
      if (mac.lo[0]) std::printf("  %s alone (%s .. %s):\n", mac.name, mac.lo, mac.hi);
      else std::printf("  %s alone:\n", mac.name);
      // where an enum parameter jumps along this line (the others at their start)
      float jp[24];
      uint8_t jparam[24];
      const int nj = enumJumps(s, g_pt, m, start, g_defaults[track], jp, jparam, 24);
      if (nj) {
        std::string j = "    enum jumps:";
        for (int k = 0; k < nj; ++k) {
          char t[48];
          std::snprintf(t, sizeof(t), " %s@%.3f", g_pt.info[track][jparam[k]]->name, double(jp[k]));
          j += t;
        }
        std::printf("%s\n", j.c_str());
      }
      for (size_t k = 0; k < lines[size_t(m)].size(); ++k) {
        const Point& p = pts[lines[size_t(m)][k]];
        char tag[32] = "";
        if (k + 1 < lines[size_t(m)].size())
          std::snprintf(tag, sizeof(tag), "  d %.1f", double(distance(p, pts[lines[size_t(m)][k + 1]])));
        printPoint(p, tag);
      }
    }
    if (g_o.points) {
      std::printf("  every point:\n");
      for (const Point& p : pts) printPoint(p, "");
    } else {
      std::printf("  corners:\n");
      for (const Point& p : pts) {
        bool corner = true;
        for (int i = 0; i < nm; ++i) corner &= p.pos[i] == 0.0f || p.pos[i] == 1.0f;
        if (corner) printPoint(p, "");
      }
    }
    std::printf("  loudness %.1f .. %.1f dB (range %.1f), peak max %.1f dB, centroid %.0f .. %.0f Hz\n",
                double(sm.loMin), double(sm.loMax), double(sm.loMax - sm.loMin), double(sm.pkMax),
                double(sm.cenMin), double(sm.cenMax));
    std::printf("  travel (sum of neighbour distances along each macro): %s\n", sm.travel.c_str());
    suggestTrims(s, pts, lines, start);
    for (const std::string& pr : problems) std::printf("  %s\n", pr.c_str());
    if (problems.empty()) std::printf("  no problems\n");
  }
  g_sum.push_back(sm);
  return sm.hard ? 1 : 0;
}

void listFiles(const std::string& dir, std::vector<std::string>* out) {
  struct stat st;
  if (stat(dir.c_str(), &st) != 0) return;
  if (!S_ISDIR(st.st_mode)) {
    out->push_back(dir);
    return;
  }
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;
    names.push_back(n);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    const std::string p = dir + "/" + n;
    if (stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) listFiles(p, out);
    else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0) out->push_back(p);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--hard") g_o.hard = true;
    else if (a == "--points") g_o.points = true;
    else if (a == "--wav" && i + 1 < argc) g_o.wavDir = argv[++i];
    else if (a == "--jump" && i + 1 < argc) g_o.jumpDb = float(std::atof(argv[++i]));
    else if (a == "--dead" && i + 1 < argc) g_o.deadDb = float(std::atof(argv[++i]));
    else if (a == "-h" || a == "--help" || a[0] == '-') {
      std::fprintf(stderr,
                   "usage: macro_audit [--hard] [--points] [--wav DIR] [--jump DB] [--dead DB] PATH...\n"
                   "  PATH: a .pgm file or a folder of them (any depth). See docs/MACROLAB.md.\n");
      return 2;
    } else paths.push_back(a);
  }
  if (paths.empty()) paths.push_back("macros");
  if (!g_o.wavDir.empty()) mkdir(g_o.wavDir.c_str(), 0755);

  g_inst.init(kSr, false);
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) {
      g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
      g_defaults[t][p] = g_inst.project().tracks[t].params[p];
    }

  std::vector<std::string> files;
  for (const std::string& p : paths) listFiles(p, &files);
  if (files.empty()) {
    std::fprintf(stderr, "macro_audit: no .pgm files in the given paths\n");
    return 2;
  }
  int fails = 0;
  for (const std::string& f : files) fails += auditFile(f);

  if (!g_o.hard) {
    std::printf("\n%-5s %-12s %3s %15s %5s %5s %4s %4s  %s\n", "voice", "set", "pts", "loudness dB", "range",
                "peak", "hard", "warn", "travel per macro");
    for (const Summary& s : g_sum)
      std::printf("%-5s %-12s %3d %7.1f..%-6.1f %5.1f %5.1f %4d %4d  %s\n", s.voice.c_str(), s.name.c_str(),
                  s.points, double(s.loMin), double(s.loMax), double(s.loMax - s.loMin), double(s.pkMax), s.hard,
                  s.warn, s.travel.c_str());
  }
  std::printf("macro_audit: %zu set(s), %d with hard failures\n", files.size(), fails);
  return fails ? 1 : 0;
}
