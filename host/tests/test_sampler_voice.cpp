// SamplerVoice (pg/sampler/sampler.h) and the sample pool's format
// (sample_format.h, sample_pool.h), on the host:
//   format    crc32 check value, header build / validate / corrupt, name cleaning
//   pool      MemoryPool slots, an empty slot resolves to ROM (slot mod 3), generation
//   rom       the built-in table is sane (lengths, peaks, overview, flash budget)
//   pitch     a 441 Hz sine plays at 441 / 882 / 661 Hz (note 60, 72, PITCH +7) and SPEED x2
//   slices    each of 16 slices of a staircase plays its own level; SLICES 4 / SLICE 6 wraps
//   reverse   a rising ramp plays falling
//   gate      the note-off releases and the voice goes idle; LOOP holds past the end
//   click     a cut (new note), the region's end, a loop's wrap and choke never jump
//             by more than the material's own steepest step
//   bounds    a zero sample between sentinel guards: no parameter extreme, note or
//             speed ever reads a guard (output stays exactly 0)
//   repeats   RTRG 1/16 at 120 BPM restarts the chop every 16th while gated
//   fine      setParamsFine then setParams plays exactly what setParams alone plays
//   pool gen  replacing the slot under a sounding chop chokes it
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "pg/sampler/rom_samples.h"
#include "pg/sampler/sample_format.h"
#include "pg/sampler/sample_pool.h"
#include "pg/sampler/sampler.h"

using namespace pg;
using namespace pg::sampler;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, double a = 0, double b = 0) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("  FAIL %s (%g, %g)\n", what, a, b);
  }
}

constexpr float kFs = 44100.0f;

struct Rig {
  MemoryPool pool;
  SamplerVoice v;
  uint8_t p[kSoundParams] = {};
  Rig() {
    v.setPool(&pool);
    v.init(kFs);
    for (int i = 0; i < kNumParams; ++i) p[i] = paramInfo(i)->def;
    v.setParams(p);
  }
  void set(int i, int val) {
    p[i] = uint8_t(val);
    v.setParams(p);
  }
  void on(int note = 60, int vel = 127) {
    v.setSampleSlot(0);
    NoteEvent e{uint8_t(note), uint8_t(vel), false, false, 0};
    v.noteOn(e);
  }
  // n quanta; appends the output
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

// Frequency by rising zero crossings (linear interpolation) over [from, to).
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

void testFormat() {
  std::printf("format\n");
  check(crc32("123456789", 9) == 0xCBF43926u, "crc32 check value");
  check(crc32("6789", 4, crc32("12345", 5)) == 0xCBF43926u, "crc32 chains");
  std::vector<int16_t> s = sine(441.0f, 5000);
  SlotHeader h;
  makeHeader(s.data(), 5000, "Oh yeah!", &h);
  check(headerValid(h), "header valid");
  char nm[kNameLen + 1];
  headerName(h, nm);
  check(std::strcmp(nm, "OHYEAH") == 0, "name cleaned");
  check(h.peak >= 29000 && h.peak <= 29500, "peak", h.peak);
  int omin = 0, omax = 0;
  for (int c = 0; c < kOverviewCols; ++c) {
    omin = std::min(omin, int(h.overview[c][0]));
    omax = std::max(omax, int(h.overview[c][1]));
    if (h.overview[c][0] > h.overview[c][1]) omin = 999;
  }
  check(omin <= -110 && omax >= 110, "overview spans the sine", omin, omax);
  SlotHeader bad = h;
  bad.overview[10][0] ^= 1;
  check(!headerValid(bad), "a flipped bit invalidates");
  bad = h;
  bad.samples = kSlotMaxSamples + 1;
  bad.headerCrc = crc32(&bad, offsetof(SlotHeader, headerCrc));
  check(!headerValid(bad), "too long is invalid");
  SlotHeader erased;
  std::memset(&erased, 0xFF, sizeof erased);
  check(!headerValid(erased), "an erased sector is empty");
  makeHeader(s.data(), 5000, "averyveryLongName", &h);
  headerName(h, nm);
  check(std::strcmp(nm, "AVERYVER") == 0, "name cut to 8");
}

void testPoolAndRom() {
  std::printf("pool, rom\n");
  size_t total = 0;
  for (int r = 0; r < kRomCount; ++r) {
    const RomSample& s = kRom[r];
    total += s.length * 2;
    check(s.length >= 8000 && s.length <= kSlotMaxSamples, "rom length", s.length);
    int peak = 0;
    for (uint32_t i = 0; i < s.length; ++i) peak = std::max(peak, std::abs(int(s.data[i])));
    check(peak == s.peak && peak > 20000 && peak < 32767, "rom peak", peak);
    check(std::strlen(s.name) > 0 && std::strlen(s.name) <= size_t(kNameLen), "rom name");
  }
  check(total <= 150u * 1024u, "rom within 150 KB", double(total));
  MemoryPool pool;
  SampleRef r;
  for (int slot = 0; slot < kPoolSlots; ++slot) {
    resolve(&pool, slot, &r);
    check(r.builtin && r.rom == slot % kRomCount && r.data == kRom[slot % kRomCount].data, "empty -> rom");
  }
  resolve(nullptr, 99, &r);
  check(r.builtin && r.data, "no pool, out of range slot");
  std::vector<int16_t> s = sine(300.0f, 9000);
  const uint32_t g0 = pool.generation();
  check(pool.set(3, s.data(), 9000, "tone"), "set");
  check(pool.generation() != g0, "generation bumps");
  resolve(&pool, 3, &r);
  check(!r.builtin && r.data == s.data() && r.length == 9000 && std::strcmp(r.name, "TONE") == 0, "user slot");
  check(!pool.set(4, s.data(), 10, "short"), "too short refused");
  resolve(&pool, 4, &r);
  check(r.builtin, "refused stays empty");
  pool.clear(3);
  resolve(&pool, 3, &r);
  check(r.builtin && r.rom == 0, "cleared");
}

void testPitch() {
  std::printf("pitch\n");
  Rig g;
  std::vector<int16_t> s = sine(441.0f, kSlotMaxSamples);
  g.pool.set(0, s.data(), kSlotMaxSamples, "sine");
  g.set(kRelease, 127);
  struct C { int note, pitch, speed; double hz; } cs[] = {
      {60, 64, 96, 441.0}, {72, 64, 96, 882.0}, {60, 71, 96, 441.0 * 1.4983071}, {48, 64, 96, 220.5},
      {60, 64, 127, 441.0 * 63.0 / 32.0}, {60, 52, 96, 220.5}};
  for (const C& c : cs) {
    g.set(kPitch, c.pitch);
    g.set(kSpeed, c.speed);
    g.on(c.note);
    std::vector<float> o;
    g.run(400, o);
    const double f = freq(o, 2000, o.size());
    check(std::fabs(f / c.hz - 1.0) < 0.002, "pitch", f, c.hz);
    g.v.choke();
    g.run(10, o);
  }
}

void testSlicesReverse() {
  std::printf("slices, reverse\n");
  Rig g;
  const uint32_t n = 16000;
  std::vector<int16_t> s(kSlotMaxSamples, 0);
  for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(1500 * (int(i * 16 / n) + 1));   // 16 steps
  g.pool.set(0, s.data(), n, "steps");
  g.set(kSlices, 16);
  g.set(kRelease, 127);
  for (int k = 0; k < 16; ++k) {
    g.set(kSlice, k);
    g.on();
    std::vector<float> o;
    g.run(30, o);   // 480 samples: the slice is 1000 source = 2000 output samples
    const float want = 1500.0f * float(k + 1) / 32768.0f * SamplerVoice::kLevel;
    check(std::fabs(o[400] - want) < 0.002f, "slice level", o[400], want);
    const int32_t ph = g.v.playhead();
    check(ph >= int32_t(n * k / 16) && ph < int32_t(n * (k + 1) / 16), "playhead in slice", ph);
    g.v.choke();
    g.run(10, o);
  }
  g.set(kSlices, 4);
  g.set(kSlice, 6);   // 6 mod 4 = 2: source 8000..12000, levels 9..12
  g.on();
  std::vector<float> o;
  g.run(30, o);
  check(std::fabs(o[400] - 1500.0f * 9.0f / 32768.0f * SamplerVoice::kLevel) < 0.002f, "slice wraps", o[400]);
  g.v.choke();
  g.run(10, o);
  // reverse on a ramp
  for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(-20000 + int(40000.0 * i / n));
  g.pool.set(0, s.data(), n, "ramp");
  g.set(kSlices, 1);
  g.set(kSlice, 0);
  g.set(kReverse, 1);
  g.on();
  o.clear();
  g.run(200, o);
  bool falling = true;
  for (size_t i = 200; i < 3000; ++i) falling = falling && o[i] <= o[i - 1] + 1e-6f;
  check(falling && o[200] > 0.5f * SamplerVoice::kLevel, "reverse falls from the top", o[200]);
}

void testGateLoop() {
  std::printf("gate, release, loop\n");
  Rig g;
  std::vector<int16_t> s = sine(300.0f, 4000);
  g.pool.set(0, s.data(), 4000, "short");
  g.set(kRelease, 30);
  g.on();
  std::vector<float> o;
  g.run(100, o);
  g.v.noteOff(60);
  g.run(700, o);
  check(!g.v.active(), "released and idle");
  check(maxStep(o, 1700, 1700 + 1000) > 0.0f, "sounded");
  // the end of a one-shot: idle after the region (4000 source = 8000 output samples)
  Rig h;
  h.pool.set(0, s.data(), 4000, "short");
  h.on();
  std::vector<float> o2;
  h.run(560, o2);   // 8960 samples
  check(!h.v.active(), "a one-shot ends at the region's end");
  // loop holds
  Rig l;
  l.pool.set(0, s.data(), 4000, "short");
  l.set(kLoop, 1);
  l.on();
  std::vector<float> o3;
  l.run(2000, o3);
  float tail = 0.0f;
  for (size_t i = o3.size() - 2000; i < o3.size(); ++i) tail = std::max(tail, std::fabs(o3[i]));
  check(l.v.active() && tail > 0.3f, "loop keeps sounding while gated", tail);
}

// The steepest step a 300 Hz full-scale sine has at the voice's level: anything
// above ~1.3x of it is a click.
void testClicks() {
  std::printf("click\n");
  const float sineStep = 0.9f * SamplerVoice::kLevel * 6.2831853f * 300.0f / kFs;
  std::vector<int16_t> s = sine(300.0f, 30000);
  // cuts at many phases, to a different note
  float worst = 0.0f;
  for (int k = 0; k < 24; ++k) {
    Rig g;
    g.pool.set(0, s.data(), 30000, "s");
    g.on(60);
    std::vector<float> o;
    g.run(40 + k, o);
    const size_t at = o.size();
    g.on(67);
    g.run(40, o);
    worst = std::max(worst, maxStep(o, at - 64, at + 400));
  }
  // (the new note is a fifth up: its own steepest step is 1.5x the old one's)
  check(worst < 1.3f * 1.5f * sineStep, "note cut", worst, 1.5f * sineStep);
  // the region's end at full level (LENGTH cut mid-wave), and loop wraps
  for (int lp = 0; lp < 2; ++lp) {
    float w = 0.0f;
    for (int len = 20; len < 127; len += 13) {
      Rig g;
      g.pool.set(0, s.data(), 30000, "s");
      g.set(kLength, len);
      g.set(kLoop, lp);
      g.on();
      std::vector<float> o;
      g.run(3000, o);
      w = std::max(w, maxStep(o, 100, o.size()));
    }
    check(w < 1.3f * sineStep, lp ? "loop wrap" : "region end", w, sineStep);
  }
  Rig g;
  g.pool.set(0, s.data(), 30000, "s");
  g.on();
  std::vector<float> o;
  g.run(77, o);
  const size_t at = o.size();
  g.v.choke();
  g.run(20, o);
  check(maxStep(o, at - 16, o.size()) < 1.3f * sineStep && !g.v.active(), "choke");
}

void testBounds() {
  std::printf("bounds\n");
  std::mt19937 rng(1234);
  const uint32_t lens[] = {64, 65, 100, 999, 22050, kSlotMaxSamples};
  int runs = 0;
  bool clean = true;
  for (uint32_t L : lens) {
    // guards of +32767 on both sides of a zero sample
    std::vector<int16_t> buf(size_t(L) + 2 * 4096, int16_t(32767));
    std::fill(buf.begin() + 4096, buf.begin() + 4096 + L, int16_t(0));
    for (int trial = 0; trial < 60; ++trial) {
      Rig g;
      g.pool.set(0, buf.data() + 4096, L, "z");
      for (int i = 0; i < kNumParams; ++i) {
        const ParamInfo* pi = paramInfo(i);
        const int r = int(rng() % 3);
        g.p[i] = uint8_t(r == 0 ? pi->min : (r == 1 ? pi->max : pi->min + int(rng() % (pi->max - pi->min + 1))));
      }
      g.v.setParams(g.p);
      g.v.setTempo(float(60 + rng() % 140));
      g.on(int(rng() % 128));
      std::vector<float> o;
      for (int seg = 0; seg < 6; ++seg) {
        g.run(50 + int(rng() % 200), o);
        // move the continuous ones mid-chop, fractional too
        float f[kSoundParams] = {};
        for (int i = 0; i < kNumParams; ++i) {
          const ParamInfo* pi = paramInfo(i);
          f[i] = float(pi->min) + float(rng() % 1000) / 999.0f * float(pi->max - pi->min);
        }
        g.v.setParamsFine(f);
        if (rng() % 3 == 0) g.on(int(rng() % 128));
        if (rng() % 4 == 0) g.v.noteOff(60);
      }
      for (float x : o) clean = clean && x == 0.0f && std::isfinite(x);
      ++runs;
    }
  }
  check(clean, "never reads outside the sample", runs);
}

// ATTACK and RELEASE read their own parameters: attack 64 = 1 ms x 400^0.504 ~ 20.4 ms
// (half way at ~10 ms), release 64 = 5 ms x 400^0.504 ~ 102 ms time constant.
void testEnvelopeTimes() {
  std::printf("envelope times\n");
  Rig g;
  std::vector<int16_t> s(kSlotMaxSamples, 16384);
  g.pool.set(0, s.data(), kSlotMaxSamples, "dc");
  g.set(kAttack, 64);
  g.set(kRelease, 64);
  g.on();
  std::vector<float> o;
  g.run(int(0.2f * kFs / kQuantum), o);
  const float full = 0.5f * SamplerVoice::kLevel;
  const float half = o[size_t(0.0102f * kFs)] / full;
  check(half > 0.45f && half < 0.55f, "attack half way at 10 ms", half);
  g.v.noteOff(60);
  const size_t at = o.size();
  g.run(int(0.2f * kFs / kQuantum), o);
  const float e1 = o[at + size_t(0.102f * kFs)] / full;   // one time constant: 1/e
  check(e1 > 0.33f && e1 < 0.41f, "release time constant", e1);
}

void testRepeats() {
  std::printf("repeats\n");
  Rig g;
  std::vector<int16_t> s(kSlotMaxSamples, 0);
  for (int i = 0; i < 40; ++i) s[size_t(i)] = 30000;   // a click at the start, then silence
  g.pool.set(0, s.data(), 44100, "clk");
  g.v.setTempo(120.0f);
  g.set(kRetrig, 5);   // 1/16
  g.on();
  std::vector<float> o;
  g.run(int(44100 / kQuantum), o);   // one second = 8 sixteenths at 120 BPM
  int hits = 0;
  for (size_t i = 1; i < o.size(); ++i)
    if (o[i] > 0.2f && o[i - 1] <= 0.2f) ++hits;
  check(hits == 8, "one restart per 16th", hits);
  // after the gate the repeats go on, quieter, while the release lasts, then stop
  g.set(kRelease, 70);   // tau ~ 0.1 s
  g.v.noteOff(60);
  std::vector<float> o2;
  g.run(int(2 * 44100 / kQuantum), o2);
  int after = 0;
  for (size_t i = 1; i < o2.size(); ++i)
    if (std::fabs(o2[i]) > 0.01f && std::fabs(o2[i - 1]) <= 0.01f) ++after;
  check(after >= 2 && after <= 6 && !g.v.active(), "repeats die away with the release", after);
}

void testFineEqualsBytes() {
  std::printf("fine == bytes\n");
  std::vector<int16_t> s = sine(300.0f, 30000);
  Rig a, b;
  a.pool.set(0, s.data(), 30000, "s");
  b.pool.set(0, s.data(), 30000, "s");
  const int vals[] = {40, 90, 70, 30, 60, 50, 96, 0, 10, 60, 80, 40, 1, 30, 20, 50, 0, 64, 0, 0};
  for (int i = 0; i < kNumParams; ++i) a.p[i] = b.p[i] = uint8_t(vals[i]);
  float f[kSoundParams] = {};
  for (int i = 0; i < kNumParams; ++i) f[i] = float(vals[i]) + (paramInfo(i)->max > 1 ? 0.37f : 0.0f);
  b.v.setParamsFine(f);
  a.v.setParams(a.p);
  b.v.setParams(b.p);
  a.on();
  b.on();
  std::vector<float> oa, ob;
  a.run(300, oa);
  b.run(300, ob);
  check(oa == ob, "identical");
}

void testGeneration() {
  std::printf("pool generation\n");
  Rig g;
  std::vector<int16_t> s = sine(300.0f, 30000), t = sine(500.0f, 30000);
  g.pool.set(0, s.data(), 30000, "s");
  g.on();
  std::vector<float> o;
  g.run(50, o);
  g.pool.set(0, t.data(), 30000, "t");
  g.run(20, o);
  check(!g.v.active(), "replaced slot chokes the chop");
}

}  // namespace

int main() {
  testFormat();
  testPoolAndRom();
  testPitch();
  testSlicesReverse();
  testGateLoop();
  testClicks();
  testBounds();
  testEnvelopeTimes();
  testRepeats();
  testFineEqualsBytes();
  testGeneration();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
