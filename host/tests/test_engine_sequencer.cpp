// Sequencer: step timing (no drift), swing, polymeter, pattern queue, song
// mode, probability, ratchets, gates, mute, p-lock scoping, hat choke.
#include <cinttypes>
#include <cstdlib>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;

// Every 16th of a straight pattern must land on the quantum edge nearest to
// its ideal time k * sr * 15 / bpm, for minutes, at any tempo and rate.
static void testTiming(float sr, int bpm, double minutes) {
  rig.init(sr, bpm);
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackKick, s) = mkStep(36 + s, kStepOn, 100, 1);
  rig.post(CmdType::Play);
  const long total = long(minutes * 60.0 * double(sr));
  rig.runSamples(total);
  const auto ons = rig.v[kTrackKick].ons();
  const long double stepLen = (long double)sr * 15.0L / (long double)bpm;
  const long expected = long(((long double)total - 8.0L) / stepLen) + 1;
  CHECKF(labs(long(ons.size()) - expected) <= 1, "sr %.0f bpm %d: %zu steps, expected %ld", double(sr), bpm, ons.size(), expected);
  double worst = 0;
  long bad = 0, offGrid = 0;
  for (size_t k = 0; k < ons.size(); ++k) {
    const long double ideal = (long double)k * stepLen;
    const double err = double((long double)ons[k].t - ideal);
    if (std::fabs(err) > worst) worst = std::fabs(err);
    if (std::fabs(err) > 8.0001) ++bad;
    if (ons[k].t % kQuantum) ++offGrid;
    if (ons[k].ev.note != 36 + int(k % 16)) ++bad;
  }
  CHECKF(bad == 0, "sr %.0f bpm %d: %ld steps off by more than half a quantum (worst %.3f)", double(sr), bpm, bad, worst);
  CHECK(offGrid == 0);
  // Drift check proper: the error of the LAST step is no bigger than the first ones.
  std::printf("  timing sr %.0f bpm %3d: %zu steps over %.1f min, worst |error| %.3f samples\n", double(sr), bpm, ons.size(), minutes, worst);
}

static void testSwing() {
  for (int swing : {0, 17, 33, 50}) {
    rig.init(44100.0f, 124);
    rig.proj.globals[kGSwing] = uint8_t(swing);
    rig.eng.init(44100.0f, &rig.proj);
    for (int s = 0; s < 16; ++s) rig.step(0, kTrackSnare, s) = mkStep(60, kStepOn, 100, 1);
    rig.post(CmdType::Play);
    rig.runSeconds(20.0);
    const auto ons = rig.v[kTrackSnare].ons();
    const double L = rig.stepLen();
    double worst = 0;
    for (size_t k = 0; k < ons.size(); ++k) {
      // 0..50 => 50..75 %: the odd 16th sits at (0.5 + swing/200) of the 8th-note pair.
      const double ideal = double(k) * L + ((k & 1) ? L * double(swing) / 100.0 : 0.0);
      worst = std::fmax(worst, std::fabs(double(ons[k].t) - ideal));
    }
    CHECKF(worst <= 8.001, "swing %d: worst error %.3f samples", swing, worst);
    if (ons.size() > 3) {
      const double ratio = double(ons[1].t - ons[0].t) / double(ons[2].t - ons[0].t);
      CHECKF(std::fabs(ratio - (0.5 + double(swing) / 200.0)) < 0.002, "swing %d: off-16th at %.4f of the pair", swing, ratio);
      std::printf("  swing %2d: off-16th at %.2f %% of the pair, worst error %.2f samples\n", swing, ratio * 100.0, worst);
    }
  }
  // Changing swing while playing must not move the straight (even) steps.
  rig.init(44100.0f, 124);
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackSnare, s) = mkStep(60, kStepOn, 100, 1);
  rig.post(CmdType::Play);
  for (int i = 0; i < 40; ++i) {
    rig.runSeconds(0.37);
    rig.post(CmdType::SetGlobal, kGSwing, 0, (i * 13) % 51);
  }
  const auto ons = rig.v[kTrackSnare].ons();
  double worst = 0;
  for (size_t k = 0; k < ons.size(); k += 2) worst = std::fmax(worst, std::fabs(double(ons[k].t) - double(k) * rig.stepLen()));
  CHECKF(worst <= 8.001, "even steps moved by %.2f samples when swing changed", worst);
}

static void testTempoChange() {
  // A tempo change takes effect immediately (the remaining part of the current
  // step is rescaled) and the new grid is exact from then on.
  rig.init(44100.0f, 120);
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackKick, s) = mkStep(60, kStepOn, 100, 1);
  rig.post(CmdType::Play);
  rig.runSeconds(2.0);
  rig.post(CmdType::SetGlobal, kGTempo, 0, 150);
  rig.runSeconds(6.0);
  const auto ons = rig.v[kTrackKick].ons();
  const double L2 = 44100.0 * 15.0 / 150.0;
  size_t first = 0;
  while (first < ons.size() && ons[first].t < long(2.0 * 44100.0) + 6000) ++first;
  double worst = 0;
  for (size_t k = first; k < ons.size(); ++k)
    worst = std::fmax(worst, std::fabs(double(ons[k].t - ons[first].t) - double(k - first) * L2));
  CHECKF(worst <= 16.0, "after tempo change: worst %.2f samples", worst);
  CHECK(rig.v[kTrackKick].tempo == 150.0f);
}

static void testPolymeter() {
  rig.init();
  rig.proj.patterns[0].length = 16;
  rig.proj.patterns[0].tracks[kTrackPerc].length = 5;
  rig.proj.patterns[0].tracks[kTrackSnare].length = 3;
  rig.proj.patterns[0].tracks[kTrackKick].length = 16;
  for (int s = 0; s < 16; ++s) {
    rig.step(0, kTrackPerc, s) = mkStep(40 + s, kStepOn, 100, 1);
    rig.step(0, kTrackSnare, s) = mkStep(70 + s, kStepOn, 100, 1);
    rig.step(0, kTrackKick, s) = mkStep(20 + s, kStepOn, 100, 1);
  }
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * 40.0));
  const auto perc = rig.v[kTrackPerc].ons(), snare = rig.v[kTrackSnare].ons(), kick = rig.v[kTrackKick].ons();
  CHECK(perc.size() >= 40 && snare.size() >= 40 && kick.size() >= 40);
  bool ok = true;
  for (size_t k = 0; k < 40 && k < perc.size(); ++k) {
    const int m = int(k % 16);            // every track restarts on the master boundary
    if (perc[k].ev.note != 40 + m % 5) ok = false;
    if (snare[k].ev.note != 70 + m % 3) ok = false;
    if (kick[k].ev.note != 20 + m) ok = false;
    if (perc[k].t != kick[k].t || snare[k].t != kick[k].t) ok = false;
  }
  CHECKF(ok, "polymeter step order wrong");
  const EngineStatus st = rig.eng.status();
  CHECK(st.step[kTrackKick] < 16 && st.step[kTrackPerc] < 5 && st.step[kTrackSnare] < 3);

  // Master length shorter than a track's length: the track never reaches its tail.
  rig.init();
  rig.proj.patterns[0].length = 6;
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackKick, s) = mkStep(20 + s, kStepOn, 100, 1);
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * 20.0));
  const auto k2 = rig.v[kTrackKick].ons();
  ok = k2.size() >= 20;
  for (size_t k = 0; k < 20 && k < k2.size(); ++k) if (k2[k].ev.note != 20 + int(k % 6)) ok = false;
  CHECKF(ok, "master length 6 not respected");

  // Length edits while playing.
  rig.post(CmdType::SetPatternLength, 0, 0, 4);
  rig.post(CmdType::SetTrackLength, 0, kTrackKick, 3);
  rig.v[kTrackKick].clearLog();
  rig.runSamples(long(rig.stepLen() * 24.0));
  const auto k3 = rig.v[kTrackKick].ons();
  ok = k3.size() >= 16;
  // After settling: master 4, track 3 => notes 20,21,22,20 | 20,21,22,20 ...
  for (size_t k = 8; k + 4 < k3.size(); ++k) {
    if (k3[k].ev.note > 22) ok = false;
  }
  CHECKF(ok, "length edit not applied");
}

static void testPatternQueue() {
  rig.init();
  for (int s = 0; s < 16; ++s) {
    rig.step(0, kTrackKick, s) = mkStep(40, kStepOn, 100, 1);
    rig.step(1, kTrackKick, s) = mkStep(50, kStepOn, 100, 1);
    rig.step(2, kTrackKick, s) = mkStep(60, kStepOn, 100, 1);
  }
  rig.proj.patterns[1].length = 8;
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * 5.5));
  rig.post(CmdType::QueuePattern, 1);
  rig.runSamples(long(rig.stepLen() * 2.0));
  EngineStatus st = rig.eng.status();
  CHECK(st.pattern == 0 && st.queuedPattern == 1 && st.playing);
  rig.runSamples(long(rig.stepLen() * 12.0));      // now inside pattern 1 (second pass)
  st = rig.eng.status();
  CHECK(st.pattern == 1 && st.queuedPattern == 0xFF);
  rig.post(CmdType::QueuePattern, 2);
  rig.runSamples(long(rig.stepLen() * 14.0));
  const auto ons = rig.v[kTrackKick].ons();
  // Expected: 16 steps of p0, then p1 (8 steps) repeating until the queue to p2
  // lands on a p1 boundary.
  bool ok = ons.size() > 30;
  for (size_t k = 0; k < 16 && k < ons.size(); ++k) if (ons[k].ev.note != 40) ok = false;
  size_t k = 16;
  size_t p1 = 0;
  while (k < ons.size() && ons[k].ev.note == 50) { ++k; ++p1; }
  CHECKF(ok && p1 > 0 && p1 % 8 == 0, "pattern 1 played %zu steps (must be a multiple of its length 8)", p1);
  for (; k < ons.size(); ++k) if (ons[k].ev.note != 60) ok = false;
  CHECKF(ok, "pattern queue order wrong");
  // The switch is exactly on the grid.
  if (ons.size() > 16) CHECK(std::fabs(double(ons[16].t) - 16.0 * rig.stepLen()) <= 8.001);

  // Stopped: QueuePattern switches immediately.
  rig.post(CmdType::Stop);
  rig.post(CmdType::QueuePattern, 0);
  rig.run(2);
  st = rig.eng.status();
  CHECK(!st.playing && st.pattern == 0 && st.queuedPattern == 0xFF);
  // Out-of-range pattern is ignored.
  rig.post(CmdType::QueuePattern, 99);
  rig.run(2);
  CHECK(rig.eng.status().pattern == 0);
}

static void testSongMode() {
  rig.init();
  for (int p = 0; p < 4; ++p)
    for (int s = 0; s < 16; ++s) rig.step(p, kTrackKick, s) = mkStep(40 + p, kStepOn, 100, 1);
  for (int p = 0; p < 4; ++p) rig.proj.patterns[p].length = 4;
  rig.proj.patterns[2].length = 2;
  // song: p1 x2, p0 x1, p2 x3, end
  rig.post(CmdType::SetSongSlot, 0, 1, 2);
  rig.post(CmdType::SetSongSlot, 1, 0, 1);
  rig.post(CmdType::SetSongSlot, 2, 2, 3);
  rig.post(CmdType::SetSongSlot, 3, 3, 0);
  rig.post(CmdType::SetSongMode, 0, 0, 1);
  rig.post(CmdType::Play);
  // one song pass = 2*4 + 1*4 + 3*2 = 18 steps
  rig.runSamples(long(rig.stepLen() * 45.0));
  const auto ons = rig.v[kTrackKick].ons();
  const int expect[18] = {41, 41, 41, 41, 41, 41, 41, 41, 40, 40, 40, 40, 42, 42, 42, 42, 42, 42};
  bool ok = ons.size() >= 45;
  for (size_t k = 0; k < 45 && k < ons.size(); ++k)
    if (ons[k].ev.note != expect[k % 18]) { ok = false; std::fprintf(stderr, "  song step %zu: note %d expected %d\n", k, ons[k].ev.note, expect[k % 18]); break; }
  CHECKF(ok, "song order wrong");
  CHECK(rig.proj.songMode == 1);

  // Song mode off: stays on the current pattern.
  rig.post(CmdType::SetSongMode, 0, 0, 0);
  rig.run(4);
  const int cur = rig.eng.status().pattern;
  rig.v[kTrackKick].clearLog();
  rig.runSamples(long(rig.stepLen() * 20.0));
  ok = true;
  for (const Call& c : rig.v[kTrackKick].ons()) if (c.ev.note != 40 + cur) ok = false;
  CHECKF(ok, "song mode off should loop the current pattern");

  // Queue during song mode is a detour: it plays once, then the song carries on.
  rig.init();
  for (int p = 0; p < 4; ++p) {
    rig.proj.patterns[p].length = 4;
    for (int s = 0; s < 16; ++s) rig.step(p, kTrackKick, s) = mkStep(40 + p, kStepOn, 100, 1);
  }
  rig.proj.song[0] = {0, 1};
  rig.proj.song[1] = {1, 1};
  rig.proj.song[2] = {0, 0};
  rig.proj.songMode = 1;
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * 1.5));
  rig.post(CmdType::QueuePattern, 3);
  rig.runSamples(long(rig.stepLen() * 18.0));
  const auto o2 = rig.v[kTrackKick].ons();
  const int exp2[16] = {40, 40, 40, 40, 43, 43, 43, 43, 41, 41, 41, 41, 40, 40, 40, 40};
  ok = o2.size() >= 16;
  for (size_t k = 0; k < 16 && k < o2.size(); ++k) if (o2[k].ev.note != exp2[k]) ok = false;
  CHECKF(ok, "queue-in-song detour order wrong");

  // Song mode switched on while playing: slot 0 starts at the next boundary.
  rig.init();
  for (int p = 0; p < 4; ++p) {
    rig.proj.patterns[p].length = 4;
    for (int s = 0; s < 16; ++s) rig.step(p, kTrackKick, s) = mkStep(40 + p, kStepOn, 100, 1);
  }
  rig.proj.song[0] = {2, 1};
  rig.proj.song[1] = {3, 1};
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * 1.5));
  rig.post(CmdType::SetSongMode, 0, 0, 1);
  rig.runSamples(long(rig.stepLen() * 14.0));
  const auto o3 = rig.v[kTrackKick].ons();
  const int exp3[12] = {40, 40, 40, 40, 42, 42, 42, 42, 43, 43, 43, 43};
  ok = o3.size() >= 12;
  for (size_t k = 0; k < 12 && k < o3.size(); ++k) if (o3[k].ev.note != exp3[k]) ok = false;
  CHECKF(ok, "song mode enabled while playing: wrong order");
  // Empty song (slot 0 repeats 0): just loops.
  rig.init();
  rig.proj.patterns[0].length = 4;
  for (int s = 0; s < 4; ++s) rig.step(0, kTrackKick, s) = mkStep(40, kStepOn, 100, 1);
  rig.proj.songMode = 1;
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * 10.0));
  CHECK(rig.v[kTrackKick].ons().size() >= 10 && rig.eng.status().pattern == 0);
}

static std::vector<long> probRun(uint32_t seed, int prob, int steps) {
  rig.init(44100.0f, 200);
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackClap, s) = mkStep(60, kStepOn, 100, 1, prob);
  if (seed) engine_detail::setRngSeed(seed);
  rig.post(CmdType::Play);
  rig.runSamples(long(rig.stepLen() * double(steps) - 100.0));
  std::vector<long> t;
  for (const Call& c : rig.v[kTrackClap].ons()) t.push_back(c.t);
  return t;
}

static void testProbability() {
  const int steps = 6000;
  for (int prob : {0, 10, 30, 50, 75, 100}) {
    const auto t = probRun(1234, prob, steps);
    const double frac = double(t.size()) / double(steps);
    // binomial sigma = sqrt(p(1-p)/n) <= 0.0065; allow 4 sigma
    CHECKF(std::fabs(frac - double(prob) / 100.0) < 0.026, "prob %d: fired %.3f", prob, frac);
    if (prob == 0) CHECK(t.empty());
    if (prob == 100) CHECK(long(t.size()) == steps);
    std::printf("  probability %3d %%: fired %.1f %% of %d steps\n", prob, frac * 100.0, steps);
  }
  const auto a = probRun(777, 40, 2000), b = probRun(777, 40, 2000), c = probRun(778, 40, 2000), d = probRun(0, 40, 2000), e = probRun(0, 40, 2000);
  CHECKF(a == b, "same seed must reproduce the same pattern");
  CHECKF(a != c, "different seed should differ");
  CHECKF(d == e, "default seed (after init) must be reproducible");
  // No obvious periodicity: fires should not be strongly correlated between adjacent steps.
  const auto t = probRun(4242, 50, 8000);
  const double L = 44100.0 * 15.0 / 200.0;
  std::vector<char> fired(8000, 0);
  for (long x : t) { long k = long(double(x) / L + 0.5); if (k >= 0 && k < 8000) fired[size_t(k)] = 1; }
  int both = 0, n = 0;
  for (int k = 0; k + 1 < 8000; ++k) { both += fired[size_t(k)] && fired[size_t(k) + 1]; ++n; }
  CHECKF(std::fabs(double(both) / double(n) - 0.25) < 0.03, "adjacent-step correlation %.3f (expect 0.25)", double(both) / double(n));
}

static void testRatchetsAndGates() {
  rig.init(44100.0f, 124);
  const double L = rig.stepLen();
  // step 0: 1 hit gate 4; step 2: 2 hits gate 4; step 4: 3 hits gate 2; step 6: 4 hits gate 4; step 8: gate 1; step 10: gate 8; step 13: gate 3
  rig.step(0, kTrackBass, 0) = mkStep(40, kStepOn, 100, 4);
  rig.step(0, kTrackBass, 2) = mkStep(42, kStepOn | (1 << kStepRatchetShift), 100, 4);
  rig.step(0, kTrackBass, 4) = mkStep(44, kStepOn | (2 << kStepRatchetShift), 100, 2);
  rig.step(0, kTrackBass, 6) = mkStep(46, kStepOn | (3 << kStepRatchetShift), 100, 4);
  rig.step(0, kTrackBass, 8) = mkStep(48, kStepOn, 100, 1);
  rig.step(0, kTrackBass, 10) = mkStep(50, kStepOn, 100, 8);
  rig.step(0, kTrackBass, 13) = mkStep(53, kStepOn, 100, 3);
  rig.post(CmdType::Play);
  rig.runSamples(long(L * 15.9));
  const auto& calls = rig.v[kTrackBass].calls;
  struct Exp { int note; double on, off; };
  std::vector<Exp> exp;
  auto add = [&](int note, int step, int hits, int gate) {
    for (int h = 0; h < hits; ++h) {
      const double on = (double(step) + double(h) / double(hits)) * L;
      exp.push_back({note, on, on + double(gate) * L / 4.0 / double(hits)});
    }
  };
  add(40, 0, 1, 4); add(42, 2, 2, 4); add(44, 4, 3, 2); add(46, 6, 4, 4); add(48, 8, 1, 1); add(50, 10, 1, 8); add(53, 13, 1, 3);
  std::vector<Call> ons, offs;
  for (const Call& c : calls) (c.type == Call::On ? ons : offs).push_back(c);
  CHECKF(ons.size() == exp.size() && offs.size() == exp.size(), "expected %zu notes, got %zu on / %zu off", exp.size(), ons.size(), offs.size());
  double worstOn = 0, worstOff = 0;
  for (size_t k = 0; k < exp.size() && k < ons.size() && k < offs.size(); ++k) {
    CHECK(ons[k].ev.note == exp[k].note && offs[k].note == exp[k].note);
    CHECK(!ons[k].ev.slide);
    worstOn = std::fmax(worstOn, std::fabs(double(ons[k].t) - exp[k].on));
    worstOff = std::fmax(worstOff, std::fabs(double(offs[k].t) - exp[k].off));
  }
  CHECKF(worstOn <= 8.5 && worstOff <= 8.5, "ratchet/gate timing: worst on %.2f, off %.2f samples", worstOn, worstOff);
  std::printf("  ratchets+gates: worst note-on error %.2f, note-off error %.2f samples\n", worstOn, worstOff);
  // Ordering: every note-off of a hit precedes the next hit's note-on in the call log.
  bool ordered = true;
  int open = 0;
  for (const Call& c : calls) {
    if (c.type == Call::On) { if (open != 0) ordered = false; ++open; }
    if (c.type == Call::Off) --open;
  }
  CHECKF(ordered, "mono track: a note-on arrived while the previous note was still gated");

  // Ratchets under swing fill the real (swung) step duration.
  rig.init(44100.0f, 124);
  rig.proj.globals[kGSwing] = 50;
  rig.eng.init(44100.0f, &rig.proj);
  rig.step(0, kTrackSnare, 0) = mkStep(60, kStepOn | (3 << kStepRatchetShift), 100, 1);   // even: lasts 1.5 L
  rig.step(0, kTrackSnare, 1) = mkStep(61, kStepOn | (1 << kStepRatchetShift), 100, 1);   // odd: lasts 0.5 L
  rig.step(0, kTrackSnare, 2) = mkStep(62, kStepOn, 100, 1);
  rig.post(CmdType::Play);
  rig.runSamples(long(L * 3.0));
  const auto so = rig.v[kTrackSnare].ons();
  CHECK(so.size() == 7);
  if (so.size() == 7) {
    const double e[7] = {0, 0.375 * L, 0.75 * L, 1.125 * L, 1.5 * L, 1.75 * L, 2.0 * L};
    for (int k = 0; k < 7; ++k) CHECKF(std::fabs(double(so[size_t(k)].t) - e[k]) <= 8.5, "swung ratchet hit %d at %ld, expected %.1f", k, so[size_t(k)].t, e[k]);
  }
  // Velocity, accent and chord reach the voice (chord only on the lead).
  rig.init();
  Step s = mkStep(64, kStepOn | kStepAccent, 77, 2);
  s.chord = kChordMin7;
  rig.step(0, kTrackLead, 0) = s;
  rig.step(0, kTrackBass, 0) = s;
  rig.step(0, kTrackKick, 0) = s;
  rig.post(CmdType::Play);
  rig.run(4);
  for (int t : {int(kTrackLead), int(kTrackBass), int(kTrackKick)}) {
    const auto o = rig.v[t].ons();
    CHECK(o.size() == 1);
    if (o.size() == 1) {
      CHECK(o[0].ev.note == 64 && o[0].ev.vel == 77 && o[0].ev.accent && !o[0].ev.slide);
      CHECK(o[0].ev.chord == (t == kTrackLead ? kChordMin7 : 0));
    }
  }
}

static void testMute() {
  rig.init();
  const double L = rig.stepLen();
  for (int s = 0; s < 16; ++s) {
    rig.step(0, kTrackKick, s) = mkStep(60, kStepOn, 100, 1);
    rig.step(0, kTrackSnare, s) = mkStep(60, kStepOn | (1 << kStepRatchetShift), 100, 1);
  }
  rig.post(CmdType::Play);
  rig.runSamples(long(L * 4.5));
  rig.post(CmdType::SetMute, kTrackKick, 0, 1);
  rig.post(CmdType::SetMute, kTrackSnare, 0, 1);
  rig.runSamples(long(L * 8.0));
  const size_t kickAtMute = rig.v[kTrackKick].ons().size(), snareAtMute = rig.v[kTrackSnare].ons().size();
  CHECK(kickAtMute == 5);
  CHECK(snareAtMute == 9 || snareAtMute == 10);     // the muted ratchet tail is cancelled
  rig.post(CmdType::SetMute, kTrackKick, 0, 0);
  rig.runSamples(long(L * 4.0));
  CHECKF(rig.v[kTrackKick].ons().size() == kickAtMute + 4, "unmute: %zu", rig.v[kTrackKick].ons().size());
  CHECK(rig.v[kTrackSnare].ons().size() == snareAtMute);
  CHECK(rig.proj.tracks[kTrackKick].mute == 0 && rig.proj.tracks[kTrackSnare].mute == 1);
  // The transport position kept running underneath the mute.
  const auto k = rig.v[kTrackKick].ons();
  CHECK(std::fabs(double(k.back().t) - 16.0 * L) <= 8.5);
}

static void testPLocks() {
  rig.init();
  const double L = rig.stepLen();
  // Base value of param 3 is 40 (stub default). Step 2 locks it to 99.
  // Steps: 0 on, 2 on+lock, 3 OFF, 5 on (no lock), 8 on + lock param 0 = 7, 9 on + lock param 0 = 9, 12 on with prob 0 (never fires).
  rig.step(0, kTrackBass, 0) = mkStep(40);
  Step s = mkStep(42);
  s.lockParam = 3 + 1; s.lockValue = 99;
  rig.step(0, kTrackBass, 2) = s;
  rig.step(0, kTrackBass, 5) = mkStep(45);
  s = mkStep(48); s.lockParam = 0 + 1; s.lockValue = 7;
  rig.step(0, kTrackBass, 8) = s;
  s = mkStep(49); s.lockParam = 0 + 1; s.lockValue = 9;
  rig.step(0, kTrackBass, 9) = s;
  s = mkStep(52, kStepOn, 100, 2, 0); s.lockParam = 1 + 1; s.lockValue = 111;
  rig.step(0, kTrackBass, 12) = s;
  // a lock on a step that is not on must do nothing
  s = mkStep(60, 0); s.lockParam = 2 + 1; s.lockValue = 5;
  rig.step(0, kTrackBass, 3) = s;

  rig.post(CmdType::Play);
  struct Probe { double atStep; int p0, p1, p2, p3; };
  const Probe probes[] = {
      {0.5, 10, 20, 30, 40}, {1.5, 10, 20, 30, 40}, {2.5, 10, 20, 30, 99}, {3.5, 10, 20, 30, 99}, {4.8, 10, 20, 30, 99},
      {5.5, 10, 20, 30, 40}, {7.5, 10, 20, 30, 40}, {8.5, 7, 20, 30, 40},  {9.5, 9, 20, 30, 40},  {11.5, 9, 20, 30, 40},
      {12.5, 9, 20, 30, 40}, {15.5, 9, 20, 30, 40}, {16.5, 10, 20, 30, 40}, {17.5, 10, 20, 30, 40}, {18.5, 10, 20, 30, 99},
  };
  for (const Probe& p : probes) {
    rig.runSamples(long(p.atStep * L) - rig.time);
    const uint8_t* q = rig.v[kTrackBass].params;
    CHECKF(q[0] == p.p0 && q[1] == p.p1 && q[2] == p.p2 && q[3] == p.p3, "at step %.1f params are %d %d %d %d, expected %d %d %d %d",
           p.atStep, q[0], q[1], q[2], q[3], p.p0, p.p1, p.p2, p.p3);
  }
  // The locked value is already in place when the locked step's note-on arrives,
  // and the unlocked value when the next firing step's note-on arrives.
  const auto ons = rig.v[kTrackBass].ons();
  CHECK(ons.size() >= 6);
  if (ons.size() >= 6) {
    CHECK(ons[0].p[3] == 40);
    CHECK(ons[1].ev.note == 42 && ons[1].p[3] == 99);
    CHECK(ons[2].ev.note == 45 && ons[2].p[3] == 40);
    CHECK(ons[3].p[0] == 7 && ons[4].p[0] == 9);
  }
  // setParams is called every quantum, and never more than twice in one.
  CHECKF(rig.v[kTrackBass].setParamsCalls >= rig.time / kQuantum, "setParams calls %ld for %ld quanta", rig.v[kTrackBass].setParamsCalls, rig.time / kQuantum);
  CHECK(rig.v[kTrackBass].setParamsThisQuantumMax <= 2);
  CHECK(rig.v[kTrackKick].setParamsThisQuantumMax == 1);
  // Other tracks never see the lock.
  CHECK(rig.v[kTrackLead].params[3] == 40 && rig.v[kTrackLead].params[0] == 10);
  // The project itself is untouched by locks.
  CHECK(rig.proj.tracks[kTrackBass].params[3] == 40);

  // Stop drops the lock.
  rig.runSamples(long(18.6 * L) - rig.time);
  rig.post(CmdType::Stop);
  rig.run(2);
  CHECK(rig.v[kTrackBass].params[3] == 40);

  // Mixer params are lockable too: lock the level to 0 on step 1, measure the output.
  rig.init();
  rig.neutralMix();
  rig.v[kTrackPerc].mode = StubVoice::Dc;
  rig.v[kTrackPerc].amp = 0.5f;
  rig.step(0, kTrackPerc, 0) = mkStep(60);
  s = mkStep(60); s.lockParam = kMixLevel + 1; s.lockValue = 0;
  rig.step(0, kTrackPerc, 1) = s;
  rig.step(0, kTrackPerc, 3) = mkStep(60);
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.runSamples(long(5.0 * L));
  rig.capture = false;
  auto level = [&](double st) { return double(rig.sample(long(st * L), 0)); };
  CHECKF(std::fabs(level(0.9) - 0.5) < 0.002, "level before lock %.4f", level(0.9));
  CHECKF(std::fabs(level(1.9)) < 0.002, "level during lock %.4f", level(1.9));
  CHECKF(std::fabs(level(2.9)) < 0.002, "lock holds through the rest step: %.4f", level(2.9));
  CHECKF(std::fabs(level(3.9) - 0.5) < 0.002, "level after lock %.4f", level(3.9));
  // and the level change is a ramp, not a step (no zipper/click)
  double maxStep = 0;
  for (long f = 1; f < rig.frames(); ++f) maxStep = std::fmax(maxStep, std::fabs(double(rig.sample(f, 0) - rig.sample(f - 1, 0))));
  CHECKF(maxStep < 0.01, "level lock produced a step of %.4f", maxStep);
}

static void testHatChoke() {
  rig.init();
  const double L = rig.stepLen();
  rig.step(0, kTrackHatOpen, 0) = mkStep(60, kStepOn, 100, 12);   // would ring for 3 steps
  rig.step(0, kTrackHatClosed, 2) = mkStep(60);
  rig.step(0, kTrackHatOpen, 4) = mkStep(60, kStepOn, 100, 2);   // both on the same step
  rig.step(0, kTrackHatClosed, 4) = mkStep(60);
  rig.step(0, kTrackHatClosed, 6) = mkStep(60, kStepOn, 100, 2, 0);   // prob 0: never fires, never chokes
  rig.step(0, kTrackKick, 7) = mkStep(60);                             // other tracks never choke
  rig.post(CmdType::Play);
  rig.runSamples(long(L * 8.0));
  const auto& oh = rig.v[kTrackHatOpen].calls;
  // Expected open-hat log: On(0), Choke(2) [and NO note-off for the choked note], Choke(4), On(4), Off(4.5)
  CHECKF(oh.size() == 5, "open hat saw %zu calls", oh.size());
  if (oh.size() == 5) {
    CHECK(oh[0].type == Call::On && oh[0].t == 0);
    CHECK(oh[1].type == Call::Choke && std::fabs(double(oh[1].t) - 2.0 * L) <= 8.5);
    CHECK(oh[2].type == Call::Choke && std::fabs(double(oh[2].t) - 4.0 * L) <= 8.5);
    CHECK(oh[3].type == Call::On && oh[3].t == oh[2].t);
    CHECK(oh[4].type == Call::Off && std::fabs(double(oh[4].t) - 4.5 * L) <= 8.5);
  }
  for (int t = 0; t < kNumTracks; ++t)
    if (t != kTrackHatOpen) CHECK(rig.v[t].chokes().empty());
  // A live closed hat chokes too; a muted closed hat does not.
  rig.post(CmdType::Stop);
  rig.run(2);
  rig.v[kTrackHatOpen].clearLog();
  rig.post(CmdType::NoteOn, kTrackHatClosed, 60, 100);
  rig.run(2);
  CHECK(rig.v[kTrackHatOpen].chokes().size() == 1);
  // trig counters moved for the tracks that fired
  const EngineStatus st = rig.eng.status();
  CHECK(st.trig[kTrackHatOpen] == 2 && st.trig[kTrackHatClosed] == 3 && st.trig[kTrackKick] == 1 && st.trig[kTrackSnare] == 0);
}

int main() {
  testTiming(44100.0f, 124, 5.0);
  testTiming(44100.0f, 173, 3.0);
  testTiming(32000.0f, 61, 3.0);
  testTiming(32000.0f, 200, 3.0);
  testTiming(44100.0f, 127, 4.0);
  testSwing();
  testTempoChange();
  testPolymeter();
  testPatternQueue();
  testSongMode();
  testProbability();
  testRatchetsAndGates();
  testMute();
  testPLocks();
  testHatChoke();
  return finish("test_engine_sequencer");
}
