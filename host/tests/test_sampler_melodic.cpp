// MelodicSampler (pg/sampler/melodic.h), the LEAD track's sampler, on the host:
//   pitch     a 441 Hz sine (at note 48 = as recorded) plays at the right pitch from
//             note 24 to 84 (four octaves and more), with PITCH and FINE, and reversed
//   tune      the track's tune (setTune) +-12, +7, +-50 cents, with PITCH / FINE, to 0.2 %;
//             changed under a sounding note: the pitch follows, no jump
//   chords    a chord plays its notes at their pitches; MIN9 four of them
//   cap       four notes at most: a fifth steals the oldest gated one; a steal never
//             jumps by more than the material's own steepest step
//   pong      a held PONG note: no step above the material's steepest, no level dip
//             (RMS in 10 ms windows within 0.5 dB), the playhead turns at both ends,
//             the release ends it; the window follows START while it is held (no jump)
//   one-shot  plays from START to the window's end and ends there, de-clicked
//   glide     a slide moves the pitch from the old note to the new one gradually,
//             keeps the playhead running and the envelope up (no retrigger)
//   bounds    a zero sample between sentinel guards: no parameter extreme, note,
//             loop, reverse, window move or slide ever reads a guard
//   fine      setParamsFine then setParams plays exactly what setParams alone plays
//   silent    the silent slot (its own, or the PERC sampler's it follows) starts nothing
//             and fades a sounding note; another slot plays
//   pool gen  replacing the slot under a sounding note fades it
//   bench     host ns per output sample: one note, four notes, every stage on
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "pg/sampler/melodic.h"
#include "pg/sampler/sample_format.h"
#include "pg/sampler/sample_pool.h"

using namespace pg;
using pg::sampler::MelodicSampler;
using pg::sampler::MemoryPool;
using pg::sampler::SamplerVoice;
using pg::sampler::kSlotMaxSamples;

namespace {

int g_fail = 0, g_pass = 0;
template <class A = double, class B = double>
void check(bool ok, const char* what, A a = 0, B b = 0) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("  FAIL %s (%g, %g)\n", what, double(a), double(b));
  }
}

constexpr float kFs = 44100.0f;
namespace mel = pg::sampler::mel;
using namespace pg::sampler::mel;

struct Rig {
  MemoryPool pool;
  MelodicSampler v;
  uint8_t p[kSoundParams] = {};
  Rig() {
    v.setPool(&pool);
    v.init(kFs);
    for (int i = 0; i < mel::kNumParams; ++i) p[i] = mel::paramInfo(i)->def;
    v.setParams(p);
  }
  void set(int i, int val) {
    p[i] = uint8_t(val);
    v.setParams(p);
  }
  void on(int note = 48, int vel = 127, uint8_t chord = 0, bool slide = false, int slot = 0) {
    v.setSampleSlot(uint8_t(slot));
    NoteEvent e{uint8_t(note), uint8_t(vel), false, slide, chord};
    v.noteOn(e);
  }
  void off(int note = 48) { v.noteOff(uint8_t(note)); }
  void run(int quanta, std::vector<float>& out) {
    float L[kQuantum], R[kQuantum];
    for (int q = 0; q < quanta; ++q) {
      if (v.active()) {
        v.render(L, R, kQuantum);
      } else {
        for (int i = 0; i < kQuantum; ++i) L[i] = 0.0f;
      }
      out.insert(out.end(), L, L + kQuantum);
    }
  }
};

std::vector<int16_t> sine(float hz, uint32_t n, float amp = 0.9f) {
  std::vector<int16_t> s(kSlotMaxSamples, 0);
  for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(std::lrint(amp * 32767.0f * std::sin(6.2831853f * hz * float(i) / 22050.0f)));
  return s;
}

double freq(const std::vector<float>& x, size_t from, size_t to) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = from + 1; i < to && i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]);
      if (first < 0) first = t;
      else ++n;
      last = t;
    }
  return n > 0 ? double(kFs) * n / (last - first) : 0.0;
}

float maxStep(const std::vector<float>& x, size_t from, size_t to) {
  float m = 0.0f;
  for (size_t i = from + 1; i < to && i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += double(x[i]) * double(x[i]);
  return std::sqrt(s / double(to - from));
}

void testPitch() {
  std::printf("pitch\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, kSlotMaxSamples);
  g.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  g.set(kRelease, 10);
  struct C { int note, pitch, fine, rev; double hz; } cs[] = {
      {48, 64, 64, 0, 441.0},        {60, 64, 64, 0, 882.0},          {36, 64, 64, 0, 220.5},
      {24, 64, 64, 0, 110.25},       {72, 64, 64, 0, 1764.0},         {84, 64, 64, 0, 3528.0},
      {55, 64, 64, 0, 441.0 * std::pow(2.0, 7.0 / 12.0)},             {48, 71, 64, 0, 441.0 * std::pow(2.0, 7.0 / 12.0)},
      {48, 52, 64, 0, 220.5},        {48, 64, 96, 0, 441.0 * std::pow(2.0, 0.5 / 12.0)},
      {48, 64, 0, 0, 441.0 / std::pow(2.0, 1.0 / 12.0)},              {60, 64, 64, 1, 882.0}};
  for (const C& c : cs) {
    g.set(kPitch, c.pitch);
    g.set(kFine, c.fine);
    g.set(kReverse, c.rev);
    g.on(c.note);
    std::vector<float> o;
    g.run(400, o);
    const double f = freq(o, 2000, o.size());
    char what[64];
    std::snprintf(what, sizeof what, "pitch note %d PTCH %d FINE %d%s", c.note, c.pitch, c.fine, c.rev ? " reversed" : "");
    check(std::fabs(f / c.hz - 1.0) < 0.001, what, f, c.hz);
    g.v.choke();
    g.run(20, o);
  }
}

// The track's tune (Voice::setTune, params.h kMixTune): on top of PITCH / FINE, every
// note, exact to 0.2 %; a change while notes sound moves them at the next quantum
// without a jump, and survives a parameter change (updateTone keeps it).
void testTune() {
  std::printf("tune\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, kSlotMaxSamples);
  g.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  g.set(kRelease, 10);
  struct C { int note, pitch, fine; float tune; double hz; } cs[] = {
      {48, 64, 64, 12.0f, 882.0}, {48, 64, 64, -12.0f, 220.5}, {48, 64, 64, 7.0f, 441.0 * std::pow(2.0, 7.0 / 12.0)},
      {48, 64, 64, 0.5f, 441.0 * std::pow(2.0, 0.5 / 12.0)}, {48, 64, 64, -0.5f, 441.0 / std::pow(2.0, 0.5 / 12.0)},
      {60, 71, 96, -9.5f, 882.0 * std::pow(2.0, -2.0 / 12.0)},   // PITCH +7, FINE +0.5, tune -9.5: -2
      {48, 64, 64, 0.0f, 441.0}};
  for (const C& c : cs) {
    g.set(kPitch, c.pitch);
    g.set(kFine, c.fine);
    g.v.setTune(c.tune);
    g.set(kCutoff, 127);   // a parameter change after the tune keeps it
    g.on(c.note);
    std::vector<float> o;
    g.run(400, o);
    const double f = freq(o, 2000, o.size());
    char what[64];
    std::snprintf(what, sizeof what, "tune %+.1f note %d PTCH %d FINE %d", double(c.tune), c.note, c.pitch, c.fine);
    check(std::fabs(f / c.hz - 1.0) < 0.002, what, f, c.hz);
    g.v.choke();
    g.run(20, o);
  }
  g.set(kPitch, 64);
  g.set(kFine, 64);
  const float sineStep = 0.9f * MelodicSampler::kLevel * 6.2831853f * 441.0f / kFs;
  for (int k = 0; k < 8; ++k) {
    Rig h;
    h.pool.set(0, s.data(), kSlotMaxSamples, "sine");
    h.on(48);
    std::vector<float> o;
    h.run(100 + k, o);
    const size_t at = o.size();
    h.v.setTune(k & 1 ? 2.0f : -2.0f);
    h.run(300, o);
    const float w = maxStep(o, at - 64, at + 64);
    check(w < 1.3f * 1.13f * sineStep, "tune change: no jump", w, sineStep);
    const double f = freq(o, at + 1000, o.size()), want = 441.0 * std::pow(2.0, (k & 1 ? 2.0 : -2.0) / 12.0);
    check(std::fabs(f / want - 1.0) < 0.002, "tune change: the sounding note follows", f, want);
  }
}

void testChords() {
  std::printf("chords\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, kSlotMaxSamples);
  g.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  g.on(48, 127, 3);   // MAJ: 0 4 7 12
  std::vector<float> o;
  g.run(4, o);
  check(g.v.soundingNotes() == 4, "MAJ: four notes", g.v.soundingNotes());
  int want[4] = {0, 4, 7, 12}, got[4];
  int n = 0;
  for (int k = 0; k < kMaxNotes; ++k)
    if (g.v.view(k).on) got[n++] = int(std::lrint(g.v.view(k).semis));
  std::sort(got, got + n);
  bool same = n == 4;
  for (int i = 0; i < n && i < 4; ++i) same &= got[i] == want[i];
  check(same, "MAJ: 0 4 7 12 semitones");
  g.off(48);
  g.run(2000, o);
  check(!g.v.active(), "released, idle");
  g.on(50, 127, 9);   // MIN9: 0 3 10 14
  g.run(2, o);
  n = 0;
  for (int k = 0; k < kMaxNotes; ++k)
    if (g.v.view(k).on) got[n++] = int(std::lrint(g.v.view(k).semis));
  std::sort(got, got + n);
  check(n == 4 && got[0] == 2 && got[1] == 5 && got[2] == 12 && got[3] == 16, "MIN9 on D: 2 5 12 16", n, got[3]);
  // level: a single note and a four-note chord are close (1/sqrt(n) per note)
  Rig a, b;
  a.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  b.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  std::vector<float> oa, ob;
  a.on(48);
  b.on(48, 127, 6);
  a.run(600, oa);
  b.run(600, ob);
  const double ra = rms(oa, 5000, oa.size()), rb = rms(ob, 5000, ob.size());
  check(std::fabs(20.0 * std::log10(rb / ra)) < 3.0, "a MIN7 chord within 3 dB of one note", 20.0 * std::log10(rb / ra));
}

void testCapAndSteal() {
  std::printf("cap, steal\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, kSlotMaxSamples);
  g.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  g.set(kRelease, 100);
  std::vector<float> o;
  const int notes[6] = {48, 52, 55, 59, 62, 65};
  for (int i = 0; i < 6; ++i) {
    g.on(notes[i]);
    g.run(200, o);
    check(g.v.soundingNotes() == std::min(i + 1, 4), "never more than four notes", g.v.soundingNotes());
  }
  // the oldest (48, 52) were stolen
  bool has48 = false, has65 = false;
  for (int k = 0; k < kMaxNotes; ++k) {
    has48 |= g.v.view(k).on && g.v.view(k).note == 48;
    has65 |= g.v.view(k).on && g.v.view(k).note == 65;
  }
  check(!has48 && has65, "the oldest note was stolen, the newest plays");
  // no jump at any steal: the steepest step anywhere stays below what four sines
  // at the highest pitch could make (the material's own steepest step)
  const double top = 441.0 * std::pow(2.0, 17.0 / 12.0);
  const float material = float(4.0 * 0.9 * double(MelodicSampler::kLevel) * 2.0 * 3.14159265 * top / double(kFs));
  check(maxStep(o, 0, o.size()) <= material, "no steal jumps above the material's steepest step", maxStep(o, 0, o.size()),
        material);
  // a steal of a loud note: the step at the steal is within one note's steepest step x 1.5
  Rig h;
  std::vector<int16_t> sq(kSlotMaxSamples, 0);
  for (uint32_t i = 0; i < kSlotMaxSamples; ++i) sq[i] = int16_t(29000);   // DC: steepest step 0
  h.pool.set(0, sq.data(), kSlotMaxSamples, "dc");
  h.set(kAttack, 0);
  std::vector<float> d;
  for (int i = 0; i < 4; ++i) h.on(60 + i);
  h.run(100, d);
  const size_t at = d.size();
  h.on(70);   // steals one of four DC notes at full level
  h.run(100, d);
  const float stepDc = maxStep(d, at - 4, d.size());
  const float noteLevel = 29000.0f / 32768.0f * MelodicSampler::kLevel * 0.5f;   // one of four (1/sqrt(4))
  check(stepDc < noteLevel * 0.05f, "a stolen note's level decays, it does not jump", stepDc, noteLevel);
}

void testPong() {
  std::printf("pong\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, 22050);
  g.pool.set(0, s.data(), 22050, "sine");
  g.set(kLoop, kPingPong);
  g.set(kStart, 32);
  g.set(kLength, 40);   // a short window: many turns
  g.set(kRelease, 30);
  g.set(kAttack, 0);
  g.on(48);
  std::vector<float> o;
  bool turnedDown = false, turnedUp = false;
  int lastDir = 1;
  for (int q = 0; q < 3000; ++q) {   // ~1.1 s
    g.run(1, o);
    const int d = g.v.view(0).dir;
    if (d < 0 && lastDir > 0) turnedDown = true;
    if (d > 0 && lastDir < 0) turnedUp = true;
    lastDir = d;
  }
  check(turnedDown && turnedUp, "turns at both ends of the window");
  check(g.v.active() && g.v.soundingNotes() == 1, "still sounding while held");
  const float material = float(0.9 * double(MelodicSampler::kLevel) * 2.0 * 3.14159265 * 441.0 / double(kFs));
  check(maxStep(o, 200, o.size()) <= material * 1.01f, "no step above the material's steepest", maxStep(o, 200, o.size()),
        material);
  // no dip: RMS over 10 ms windows
  const size_t w = 441;
  double lo = 1e9, hi = 0.0;
  for (size_t i = 4410; i + w <= o.size(); i += w) {
    const double r = rms(o, i, i + w);
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  check(20.0 * std::log10(hi / lo) < 0.5, "no level dip (10 ms RMS within 0.5 dB)", 20.0 * std::log10(hi / lo));
  // the window follows START while the note is held, the playhead never jumps
  int32_t prev = g.v.view(0).pos;
  int32_t maxJump = 0;
  for (int st = 32; st <= 100; st += 4) {
    g.set(kStart, st);
    for (int q = 0; q < 60; ++q) {
      g.run(1, o);
      const int32_t p = g.v.view(0).pos;
      maxJump = std::max(maxJump, std::abs(p - prev));
      prev = p;
    }
  }
  for (int q = 0; q < 1500; ++q) {   // it travels to the window at the note's speed
    g.run(1, o);
    const int32_t p = g.v.view(0).pos;
    maxJump = std::max(maxJump, std::abs(p - prev));
    prev = p;
  }
  int32_t a, b;
  MelodicSampler::region(g.p, 22050, 22050, &a, &b);
  check(maxJump <= 9, "the playhead never jumps (at most one quantum's travel)", maxJump);
  check(g.v.view(0).pos >= a - 200 && g.v.view(0).pos < b + 200, "it reached the moved window", g.v.view(0).pos, a);
  const size_t rel = o.size();
  g.off(48);
  g.run(4000, o);
  check(!g.v.active(), "the release ends it");
  check(rms(o, rel + 2000 * 16, o.size()) < 1e-5, "silent after the release");
}

void testOneShot() {
  std::printf("one-shot\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, 22050);
  g.pool.set(0, s.data(), 22050, "sine");
  g.set(kLoop, kOneShot);
  g.set(kStart, 64);   // the second half: 0.5 s at x1
  g.on(48);
  std::vector<float> o;
  g.run(2000, o);   // 0.73 s, held
  check(!g.v.active(), "a held one-shot ends at the window's end");
  const size_t end = size_t(0.5 * double(kFs));
  check(rms(o, 1000, end - 1000) > 0.1, "it played");
  check(rms(o, end + 1000, o.size()) < 1e-6, "silent after it");
  const float material = float(0.9 * double(MelodicSampler::kLevel) * 2.0 * 3.14159265 * 441.0 / double(kFs));
  check(maxStep(o, 100, o.size()) <= material * 1.01f, "the end is de-clicked", maxStep(o, 100, o.size()), material);
}

void testGlide() {
  std::printf("glide\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, kSlotMaxSamples);
  g.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  g.set(kGlide, 70);
  g.on(48);
  std::vector<float> o;
  g.run(300, o);
  const int32_t posBefore = g.v.view(0).pos;
  const float envBefore = g.v.view(0).env;
  g.on(55, 127, 0, true);   // legato
  g.off(48);                // the old step's note-off is dropped by the engine; a stray one must not end it
  check(g.v.soundingNotes() == 1, "a slide does not add a note", g.v.soundingNotes());
  g.run(1, o);
  const MelodicSampler::NoteView v1 = g.v.view(0);
  check(v1.pos > posBefore && v1.pos - posBefore < 20, "the playhead runs on (no retrigger)", v1.pos, posBefore);
  check(v1.env >= envBefore * 0.99f, "the envelope stays up", v1.env, envBefore);
  check(v1.semis > 0.0f && v1.semis < 6.0f, "the pitch glides (not a jump)", v1.semis);
  float prevSemis = v1.semis;
  bool monotone = true;
  for (int q = 0; q < 2000; ++q) {
    g.run(1, o);
    monotone &= g.v.view(0).semis >= prevSemis - 1e-6f;
    prevSemis = g.v.view(0).semis;
  }
  check(monotone && std::fabs(prevSemis - 7.0f) < 1e-3f, "it arrives at the new note", prevSemis);
  const double f = freq(o, o.size() - 8000, o.size());
  check(std::fabs(f / (441.0 * std::pow(2.0, 7.0 / 12.0)) - 1.0) < 0.002, "at its pitch", f);
  // a slide into a chord: each old note glides; extra ones start
  g.on(48, 127, 3, true);
  g.run(2, o);
  check(g.v.soundingNotes() == 4, "slide into MAJ: four notes", g.v.soundingNotes());
}

void testBounds() {
  std::printf("bounds\n");
  const uint32_t G = 4096;
  std::mt19937 rng(7);
  for (uint32_t N : {64u, 65u, 300u, 441u, 5000u, 70000u, kSlotMaxSamples}) {
    std::vector<int16_t> buf(N + 2 * G, int16_t(32767));
    for (uint32_t i = 0; i < N; ++i) buf[G + i] = 0;
    Rig g;
    g.pool.set(0, buf.data() + G, N, "z");
    float out = 0.0f;
    std::vector<float> o;
    for (int trial = 0; trial < 300; ++trial) {
      for (int i = 0; i < mel::kNumParams; ++i) {
        const ParamInfo* pi = mel::paramInfo(i);
        const int r = int(rng() % 4);
        g.p[i] = uint8_t(r == 0 ? pi->min : (r == 1 ? pi->max : pi->min + int(rng() % (pi->max - pi->min + 1))));
      }
      g.v.setParams(g.p);
      const int note = int(rng() % 128);
      g.on(note, 1 + int(rng() % 127), uint8_t(rng() % kChordNum), (rng() & 1) != 0);
      const int q = 1 + int(rng() % 120);
      for (int k = 0; k < q; ++k) {
        if ((rng() & 7) == 0) {   // the window moves while it sounds (fractions too)
          float f[kSoundParams];
          for (int i = 0; i < kSoundParams; ++i) f[i] = float(g.p[i]);
          f[kStart] = float(rng() % 12800) * 0.01f;
          f[kLength] = float(rng() % 12800) * 0.01f;
          f[kPitch] = float(16 + int(rng() % 97));
          g.v.setParamsFine(f);
        }
        g.run(1, o);
      }
      if (rng() & 1) g.off(note);
      for (float x : o) out = std::max(out, std::fabs(x));
      o.clear();
    }
    check(out == 0.0f, "never reads a guard", out, N);
  }
}

void testFine() {
  std::printf("fine\n");
  std::vector<int16_t> s = sine(300.0f, 30000);
  Rig a, b;
  a.pool.set(0, s.data(), 30000, "s");
  b.pool.set(0, s.data(), 30000, "s");
  for (Rig* r : {&a, &b}) {
    r->set(kCutoff, 80);
    r->set(kEnvAmt, 40);
    r->set(kLoop, 1);
    r->set(kDrive, 30);
  }
  float f[kSoundParams];
  for (int i = 0; i < kSoundParams; ++i) f[i] = float(a.p[i]);
  f[kCutoff] = 55.5f;
  f[kStart] = 20.25f;
  a.v.setParamsFine(f);
  a.v.setParams(a.p);   // back on the bytes
  std::vector<float> oa, ob;
  a.on(50, 100, 3);
  b.on(50, 100, 3);
  a.run(300, oa);
  b.run(300, ob);
  check(oa == ob, "setParamsFine then setParams = setParams alone");
}

void testSilentAndGen() {
  std::printf("silent, pool generation\n");
  std::vector<int16_t> s = sine(300.0f, 30000), t = sine(500.0f, 30000);
  SamplerVoice perc;
  Rig g;
  g.v.followSilent(&perc);
  g.pool.set(0, s.data(), 30000, "s");
  g.pool.set(1, t.data(), 30000, "t");
  g.set(kLoop, 1);
  perc.setSilentSlot(0);
  g.on(48);
  std::vector<float> o;
  g.run(20, o);
  check(!g.v.active() && rms(o, 0, o.size()) == 0.0, "the PERC sampler's silent slot starts nothing here");
  g.on(48, 127, 0, false, 1);
  g.run(20, o);
  check(g.v.active() && g.v.soundingNotes() == 1, "another slot plays");
  g.v.choke();
  g.run(20, o);
  perc.setSilentSlot(-1);
  g.on(48);
  g.run(20, o);
  check(g.v.soundingNotes() == 1, "plays after");
  g.v.setSilentSlot(0);
  g.run(20, o);
  check(g.v.soundingNotes() == 0, "a sounding note of the silent slot fades within a few quanta");
  g.v.setSilentSlot(-1);
  g.on(48);
  g.run(20, o);
  g.pool.set(0, t.data(), 30000, "t");
  g.run(20, o);
  check(g.v.soundingNotes() == 0, "replaced slot fades the note");
}

// The ESP32-S3's FPU is slow on subnormal operands: no output sample and no tail may be one.
void testSubnormal() {
  std::printf("subnormal\n");
  std::vector<int16_t> s = sine(300.0f, 30000);
  for (int cfg = 0; cfg < 4; ++cfg) {
    Rig g;
    g.pool.set(0, s.data(), 30000, "s");
    g.set(kLoop, cfg & 1);
    g.set(kCutoff, 60);
    g.set(kReso, 100);
    g.set(kEnvAmt, 60);
    g.set(kDrift, 90);
    if (cfg & 2) {
      g.set(kBits, 70);
      g.set(kDown, 50);
      g.set(kDrive, 40);
    }
    g.set(kRelease, 90);
    std::vector<float> o;
    g.on(48, 127, 3);
    g.run(400, o);
    g.off(48);
    int q = 0;
    while (g.v.active() && q < 200000) {
      g.run(1, o);
      ++q;
    }
    bool sub = false;
    for (size_t i = 0; i < o.size(); ++i)
      if (o[i] != 0.0f && std::fpclassify(o[i]) == FP_SUBNORMAL) {
        if (!sub) std::printf("    first subnormal at %zu of %zu: %g (prev %g)\n", i, o.size(), double(o[i]), double(o[i - 1]));
        sub = true;
      }
    check(!sub && !g.v.active(), "no subnormal output, the tail ends", cfg, q);
  }
}

double benchOne(int notes, bool worst) {
  std::vector<int16_t> s = sine(300.0f, kSlotMaxSamples);
  Rig g;
  g.pool.set(0, s.data(), kSlotMaxSamples, "s");
  g.set(kLoop, 1);
  g.set(kSustain, 127);
  if (worst) {
    g.set(kCutoff, 70);
    g.set(kReso, 60);
    g.set(kEnvAmt, 50);
    g.set(kFltDecay, 127);
    g.set(kBits, 60);
    g.set(kDown, 40);
    g.set(kDrive, 60);
    g.set(kDrift, 60);
  }
  for (int k = 0; k < notes; ++k) g.on(48 + 3 * k);
  float L[kQuantum], R[kQuantum], acc = 0.0f;
  double best = 1e9;
  for (int rep = 0; rep < 5; ++rep) {
    const int quanta = 60000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int q = 0; q < quanta; ++q) {
      g.v.setParams(g.p);
      g.v.render(L, R, kQuantum);
      acc += L[3];
    }
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) /
                      (double(quanta) * kQuantum);
    best = std::min(best, ns);
  }
  if (acc == 12345.0f) std::printf(" ");
  return best;
}

// Host ns per output sample (regression numbers, not device figures; the device
// estimate is in docs/STATE.md). The per-note cost is the slope from one note to
// four; the shared part what one note costs beyond it.
void bench() {
  std::printf("bench (best of 5, ns per output sample on this host)\n");
  for (int worst = 0; worst < 2; ++worst) {
    const double n1 = benchOne(1, worst != 0), n4 = benchOne(4, worst != 0);
    const double per = (n4 - n1) / 3.0, shared = n1 - per;
    std::printf("  %s: 1 note %.2f, 4 notes %.2f -> per note %.2f, shared %.2f\n", worst ? "every stage on" : "plain    ", n1,
                n4, per, shared);
    check(n4 < 400.0, "not absurdly slow", n4);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
    bench();
    return 0;
  }
  testPitch();
  testTune();
  testChords();
  testCapAndSteal();
  testPong();
  testOneShot();
  testGlide();
  testBounds();
  testFine();
  testSilentAndGen();
  testSubnormal();
  bench();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
