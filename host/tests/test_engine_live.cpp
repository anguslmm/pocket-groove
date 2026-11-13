// Live notes, record quantise, command queue (overflow, wrap, cross-thread),
// suspend/resume, transport, edit commands, project init/sanitising, tables.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;

static Command noteOn(int track, int note, int vel, int flags = 0, int chord = 0) {
  Command c = cmd(CmdType::NoteOn, track, note, vel);
  c.step.setFlags(uint8_t(flags));
  c.step.setChord(uint8_t(chord));
  return c;
}

static void testLiveNotes() {
  rig.init();
  rig.run(10);
  rig.post(noteOn(kTrackBass, 50, 90, kStepAccent));
  rig.post(noteOn(kTrackLead, 62, 200, 0, kChordMaj7));     // velocity clamps to 127
  rig.post(noteOn(kTrackKick, 60, 0));                        // velocity clamps to 1
  rig.run(1);
  const long t0 = rig.time - kQuantum;
  auto b = rig.v[kTrackBass].calls, l = rig.v[kTrackLead].calls, k = rig.v[kTrackKick].calls;
  CHECK(b.size() == 1 && b[0].type == Call::On && b[0].t == t0 && b[0].ev.note == 50 && b[0].ev.vel == 90 && b[0].ev.accent && !b[0].ev.slide && b[0].ev.chord == 0);
  CHECK(l.size() == 1 && l[0].ev.note == 62 && l[0].ev.vel == 127 && l[0].ev.chord == kChordMaj7);
  CHECK(k.size() == 1 && k[0].ev.vel == 1);
  rig.run(5);
  rig.post(CmdType::NoteOff, kTrackBass, 50);
  rig.run(1);
  b = rig.v[kTrackBass].calls;
  CHECK(b.size() == 2 && b[1].type == Call::Off && b[1].note == 50 && b[1].t == rig.time - kQuantum);
  // Slide flag on a live note = legato request.
  rig.post(noteOn(kTrackBass, 52, 100, kStepSlide));
  rig.post(noteOn(kTrackKick, 60, 100, kStepSlide));          // drums: ignored
  rig.run(1);
  CHECK(rig.v[kTrackBass].calls.back().ev.slide && !rig.v[kTrackKick].calls.back().ev.slide);
  // Nothing was written: not playing / not recording.
  const EngineStatus st = rig.eng.status();
  CHECK(st.revision == 0 && !st.playing && !st.recording);
  CHECK(st.trig[kTrackBass] == 2 && st.trig[kTrackKick] == 2 && st.trig[kTrackLead] == 1);
  // Invalid track is ignored.
  rig.post(noteOn(12, 60, 100));
  rig.post(CmdType::NoteOff, 200, 60);
  rig.run(1);
}

// Play a live note `atSteps` after Play and report where it was recorded.
struct RecResult { int pattern, step; size_t seqOnsNear; bool found; };
static RecResult recordAt(double atSteps, int track, int swing = 0, int trackLen = 16, int queuePattern = -1) {
  rig.init(44100.0f, 124);
  rig.proj.globals[kGSwing] = uint8_t(swing);
  rig.proj.patterns[0].tracks[track].length = uint8_t(trackLen);
  rig.eng.init(44100.0f, &rig.proj);
  const double L = rig.stepLen();
  rig.post(CmdType::SetRecord, 0, 0, 1);
  rig.post(CmdType::Play);
  if (queuePattern >= 0) rig.post(CmdType::QueuePattern, queuePattern);
  rig.runSamples(long(atSteps * L));
  rig.post(noteOn(track, 67, 93, kStepAccent, kChordMin));
  rig.runSamples(long(1.6 * L));
  RecResult r{-1, -1, 0, false};
  for (int p = 0; p < kNumPatterns; ++p)
    for (int s = 0; s < kMaxSteps; ++s)
      if (rig.proj.patterns[p].tracks[track].steps[s].flags() & kStepOn) { r.pattern = p; r.step = s; r.found = true; }
  r.seqOnsNear = rig.v[track].ons().size();   // 1 = only the live note sounded
  return r;
}

static void testRecord() {
  struct Case { const char* what; double at; int swing, len, queue; int expPattern, expStep; };
  const Case cases[] = {
      {"just after step 3", 3.20, 0, 16, -1, 0, 3},
      {"just before the midpoint", 3.47, 0, 16, -1, 0, 3},
      {"just after the midpoint", 3.53, 0, 16, -1, 0, 4},
      {"just before step 4", 3.80, 0, 16, -1, 0, 4},
      {"a hair before step 4", 3.995, 0, 16, -1, 0, 4},
      {"wrap to step 0 at the pattern end", 15.80, 0, 16, -1, 0, 0},
      {"polymeter track wrap", 4.80, 0, 5, -1, 0, 0},
      {"polymeter track, mid", 7.20, 0, 5, -1, 0, 2},
      // swing 50: even steps last 1.5 L, odd steps 0.5 L, odd steps fire at n + 0.5
      {"swing: 0.6 after an even step is still that step", 2.60, 50, 16, -1, 0, 2},
      {"swing: 0.9 after an even step is the swung next", 2.90, 50, 16, -1, 0, 3},
      {"swing: just after the swung step", 3.60, 50, 16, -1, 0, 3},
      {"swing: late in the short odd step", 3.90, 50, 16, -1, 0, 4},
      {"downbeat of a queued pattern", 15.80, 0, 16, 5, 5, 0},
      {"last step with a pattern queued", 15.30, 0, 16, 5, 0, 15},
  };
  for (const Case& c : cases) {
    for (int track : {int(kTrackSnare), int(kTrackBass)}) {
      const RecResult r = recordAt(c.at, track, c.swing, c.len, c.queue);
      CHECKF(r.found && r.pattern == c.expPattern && r.step == c.expStep, "record %s (track %d): landed on pattern %d step %d, expected %d/%d",
             c.what, track, r.pattern, r.step, c.expPattern, c.expStep);
      // Exactly one note-on so far: the live one. A note quantised forward
      // must not be triggered again when the sequencer reaches its step.
      CHECKF(r.seqOnsNear == 1, "record %s (track %d): %zu note-ons, expected only the live one", c.what, track, r.seqOnsNear);
    }
    std::printf("  ok  record %-48s -> pattern %d step %d\n", c.what, c.expPattern, c.expStep);
  }

  // What gets written, and that it plays on the next pass (and every pass after).
  rig.init();
  const double L = rig.stepLen();
  rig.proj.patterns[0].length = 8;
  Step pre = kDefaultStep;
  pre.note = 55; pre.setLock(3, 9); pre.setProb(80); pre.setFlags(uint8_t(2 << kStepRatchetShift));
  rig.step(0, kTrackSnare, 4) = pre;                        // off, but carries tuning/lock/prob/ratchet
  rig.step(0, kTrackLead, 4) = pre;
  rig.post(CmdType::SetRecord, 0, 0, 1);
  rig.post(CmdType::Play);
  rig.run(4);
  CHECK(rig.eng.status().recording);
  const uint32_t rev0 = rig.eng.status().revision;
  rig.runSamples(long(3.8 * L) - rig.time);
  rig.post(noteOn(kTrackSnare, 72, 93));
  rig.post(noteOn(kTrackLead, 72, 93, kStepAccent | kStepSlide, kChordSus4));
  rig.runSamples(long(7.5 * L) - rig.time);
  const Step& d = rig.step(0, kTrackSnare, 4);
  CHECKF(d.flags() == (kStepOn | (2 << kStepRatchetShift)) && d.vel == 93 && d.note == 55 && d.lockParam() == 3 && d.lockValue() == 9 && d.prob() == 80 && d.gate == pre.gate,
         "drum record wrote flags %02x vel %d note %d", d.flags(), d.vel, d.note);
  const Step& s = rig.step(0, kTrackLead, 4);
  CHECKF(s.flags() == (kStepOn | kStepAccent | kStepSlide | (2 << kStepRatchetShift)) && s.vel == 93 && s.note == 72 && s.chord() == kChordSus4 && s.gate == kDefaultStep.gate,
         "synth record wrote flags %02x vel %d note %d chord %d gate %d", s.flags(), s.vel, s.note, s.chord(), s.gate);
  CHECK(rig.eng.status().revision == rev0 + 2);
  CHECK(rig.v[kTrackLead].ons().size() == 1);
  rig.proj.patterns[0].tracks[kTrackLead].steps[4].setFlags(kStepOn);   // drop ratchet for a simple count
  rig.proj.patterns[0].tracks[kTrackLead].steps[4].setProb(100);
  rig.runSamples(long(20.5 * L) - rig.time);
  const auto lo = rig.v[kTrackLead].ons();
  CHECKF(lo.size() == 3, "recorded note should play on passes 2 and 3 (got %zu note-ons)", lo.size());
  if (lo.size() == 3) {
    CHECK(std::fabs(double(lo[1].t) - 12.0 * L) <= 8.5 && std::fabs(double(lo[2].t) - 20.0 * L) <= 8.5);
    CHECK(lo[1].ev.note == 72 && lo[1].ev.vel == 93 && lo[1].ev.chord == kChordSus4);
  }

  // Not recording, or recording while stopped: nothing is written.
  rig.init();
  rig.post(CmdType::Play);
  rig.runSamples(long(2.3 * L));
  rig.post(noteOn(kTrackSnare, 60, 100));
  rig.run(4);
  rig.post(CmdType::Stop);
  rig.post(CmdType::SetRecord, 0, 0, 1);
  rig.run(4);
  rig.post(noteOn(kTrackSnare, 60, 100));
  rig.run(4);
  bool any = false;
  for (int st = 0; st < kMaxSteps; ++st) any |= (rig.step(0, kTrackSnare, st).flags() & kStepOn) != 0;
  CHECK(!any && rig.eng.status().revision == 0);
  CHECK(rig.v[kTrackSnare].ons().size() == 2);
}

static void testQueue() {
  // Overflow: exactly 256 commands fit.
  rig.init();
  int accepted = 0;
  for (int i = 0; i < 300; ++i) {
    Command c = cmd(CmdType::SetStep, i % 16, (i / 16) % 8, (i / 128) % 64);
    c.step = mkStep(30 + i % 90, kStepOn, 1 + i % 127);
    if (rig.eng.post(c)) ++accepted;
  }
  CHECKF(accepted == 256, "queue accepted %d commands (capacity must be 256)", accepted);
  CHECK(!rig.eng.post(cmd(CmdType::Stop)));
  rig.run(1);
  CHECK(rig.eng.post(cmd(CmdType::SetMute, 0, 0, 0)));      // space again after one quantum
  rig.run(8);
  int applied = 0;
  for (int i = 0; i < 256; ++i)
    if (rig.proj.patterns[i % 16].tracks[(i / 16) % 8].steps[(i / 128) % 64].flags() & kStepOn) ++applied;
  CHECKF(applied == 256, "%d of 256 queued edits applied", applied);
  CHECK(rig.eng.status().revision == 257);

  // Wrap: push far more than the ring size through it in small bursts, in order.
  rig.init();
  uint32_t n = 0;
  uint32_t lcg = 1;
  for (int round = 0; round < 3000; ++round) {
    lcg = lcg * 1664525u + 1013904223u;
    const int burst = 1 + int((lcg >> 24) % 40);
    for (int i = 0; i < burst; ++i) {
      ++n;
      rig.post(CmdType::SetGlobal, 31, 0, int(n & 0xFF));     // unused global slot: stored raw
    }
    rig.run(1);
    if (round % 97 == 0) CHECK(rig.proj.globals[31] == (n & 0xFF));
  }
  rig.run(2);
  CHECK(rig.proj.globals[31] == (n & 0xFF));
  CHECKF(rig.eng.status().revision == n, "revision %u after %u edits", rig.eng.status().revision, n);
  std::printf("  queue: %u commands through a 256-slot ring in bursts, none lost\n", n);

  // Heavy commands are spread over quanta (bounded work per block) but still all applied, in order.
  rig.init();
  rig.step(0, kTrackKick, 0) = mkStep(41);
  for (int i = 1; i < 16; ++i) rig.post(CmdType::CopyPattern, i - 1, i);
  rig.run(1);
  int copied = 0;
  for (int i = 1; i < 16; ++i) copied += (rig.step(i, kTrackKick, 0).flags() & kStepOn) ? 1 : 0;
  CHECKF(copied >= 1 && copied < 15, "bulk copies in the first quantum: %d (should be throttled)", copied);
  rig.run(20);
  copied = 0;
  for (int i = 1; i < 16; ++i) copied += (rig.step(i, kTrackKick, 0).note == 41) ? 1 : 0;
  CHECK(copied == 15);

  // Cross-thread: a producer thread posts while this thread runs the audio callback.
  rig.init();
  const uint32_t total = 200000;
  std::atomic<bool> done{false};
  std::atomic<uint32_t> retries{0};
  std::thread producer([&] {
    for (uint32_t i = 1; i <= total; ++i) {
      Command c = cmd(CmdType::SetGlobal, 30, 0, int(i & 0xFF));
      while (!rig.eng.post(c)) { retries.fetch_add(1); std::this_thread::yield(); }
    }
    done.store(true);
  });
  uint8_t last = 0;
  long outOfOrder = 0;
  int16_t buf[2 * kQuantum];
  while (!done.load() || rig.eng.status().revision < total) {
    rig.eng.process(buf, kQuantum);
    const uint8_t now = rig.proj.globals[30];
    // values only ever move forward (mod 256) by at most the per-quantum budget
    if (uint8_t(now - last) > 64) ++outOfOrder;
    last = now;
  }
  producer.join();
  CHECKF(rig.eng.status().revision == total, "cross-thread: %u of %u commands applied", rig.eng.status().revision, total);
  CHECK(rig.proj.globals[30] == (total & 0xFF));
  CHECK(outOfOrder == 0);
  std::printf("  queue: %u commands posted from a second thread (%u full-queue retries), none lost or reordered\n", total, retries.load());
}

static void testSuspend() {
  // Single-threaded: suspend() must return immediately when process() is not running.
  rig.init();
  rig.neutralMix();
  rig.v[kTrackBass].mode = StubVoice::Dc;
  rig.v[kTrackBass].amp = 0.5f;
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackKick, s) = mkStep(60);
  rig.eng.init(44100.0f, &rig.proj);
  rig.post(CmdType::Play);
  rig.capture = true;
  rig.run(200);
  const long fSuspend = rig.frames();
  rig.eng.suspend();
  {
    Command c = cmd(CmdType::SetStep, 0, kTrackSnare, 3);    // must NOT be applied while suspended
    c.step = mkStep(60);
    rig.post(c);
  }
  // The UI now owns the Project: trash it completely, as a loader would mid-read.
  std::memset(&rig.proj, 0xA5, sizeof(Project));
  const size_t kicksAtSuspend = rig.v[kTrackKick].ons().size();
  rig.run(400);
  bool untouched = true;
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&rig.proj);
  for (size_t i = 0; i < sizeof(Project); ++i) if (raw[i] != 0xA5) { untouched = false; break; }
  CHECKF(untouched, "engine wrote to the Project while suspended");
  CHECK(rig.v[kTrackKick].ons().size() == kicksAtSuspend);
  // Output: fades to silence in a few ms, without a step.
  double maxStep = 0;
  for (long f = fSuspend - 50; f < rig.frames(); ++f) maxStep = std::fmax(maxStep, std::fabs(double(rig.sample(f, 0) - rig.sample(f - 1, 0))));
  CHECKF(maxStep < 0.01, "suspend fade has a step of %.4f", maxStep);
  CHECK(std::fabs(double(rig.sample(fSuspend - 1, 0)) - 0.5) < 0.01);
  CHECK(peakOf(rig.out, fSuspend + 200, rig.frames(), 0) == 0.0);
  // Load a new (valid) project into the same memory, resume.
  rig.eng.initProject(rig.proj);
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackClap, s) = mkStep(60);
  rig.proj.globals[kGTempo] = 140;
  const uint32_t revBefore = rig.eng.status().revision;
  rig.eng.resume();
  rig.run(50);
  EngineStatus st = rig.eng.status();
  CHECK(!st.playing && st.pattern == 0 && st.queuedPattern == 0xFF && st.revision != revBefore);
  // An edit queued while suspended was meant for the old project: discarded
  // (see Engine::suspend(); test_engine_suspend covers the details).
  CHECK(!(rig.step(0, kTrackSnare, 3).flags() & kStepOn));
  CHECK(rig.v[kTrackKick].chokes().size() >= 1);
  rig.post(CmdType::Play);
  rig.runSeconds(1.0);
  CHECK(rig.v[kTrackClap].ons().size() >= 9 && rig.v[kTrackKick].ons().size() == kicksAtSuspend);
  CHECK(rig.v[kTrackClap].tempo == 140.0f);
  rig.capture = false;

  // Resume onto a garbage project must be survivable (sanitised, no crash).
  rig.eng.suspend();
  std::memset(&rig.proj, 0xFF, sizeof(Project));
  rig.eng.resume();
  rig.post(CmdType::Play);
  rig.runSeconds(0.5);
  CHECK(rig.proj.patterns[3].length <= kMaxSteps && rig.proj.patterns[3].tracks[2].length <= kMaxSteps);
  CHECK(rig.proj.globals[kGTempo] == 200 && rig.proj.patterns[0].tracks[0].steps[0].prob() == 100);
  CHECK(engine_detail::faultCount() == 0);

  // Two threads: the audio thread hammers process(); suspend() must only return
  // once the audio thread can no longer be inside a quantum that reads the Project.
  rig.init();
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackKick, s) = mkStep(60);
  rig.post(CmdType::Play);
  std::atomic<bool> quit{false};
  std::atomic<long> calls{0};
  std::thread audio([&] {
    int16_t buf[2 * 64];
    while (!quit.load()) { rig.eng.process(buf, 64); calls.fetch_add(1); }
  });
  static Project snapshot;
  int mismatches = 0;
  for (int round = 0; round < 200; ++round) {
    while (calls.load() < (round + 1) * 20) std::this_thread::yield();
    // keep edits flowing so that an engine that ignored suspend WOULD write
    for (int i = 0; i < 100; ++i) rig.eng.post(cmd(CmdType::SetGlobal, 29, 0, i));
    rig.eng.suspend();
    std::memcpy(&snapshot, &rig.proj, sizeof(Project));
    const long c0 = calls.load();
    while (calls.load() < c0 + 5) std::this_thread::yield();
    if (std::memcmp(&snapshot, &rig.proj, sizeof(Project)) != 0) ++mismatches;
    rig.eng.resume();
  }
  quit.store(true);
  audio.join();
  CHECKF(mismatches == 0, "Project changed under a suspended engine in %d of 200 rounds", mismatches);
  std::printf("  suspend/resume: 200 rounds against a free-running audio thread (%ld callbacks), Project never touched while suspended\n", calls.load());
}

static void testTransport() {
  rig.init();
  rig.neutralMix();
  const double L = rig.stepLen();
  for (int s = 0; s < 16; ++s) rig.step(0, kTrackKick, s) = mkStep(40 + s, kStepOn, 100, 1);
  rig.step(0, kTrackBass, 2) = mkStep(45, kStepOn, 100, 16);      // long note, still gated when we stop
  rig.step(0, kTrackClap, 2) = mkStep(60, kStepOn, 100, 2);
  rig.v[kTrackClap].mode = StubVoice::Blip;
  rig.proj.tracks[kTrackClap].params[kMixReverbSend] = 127;
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.runSamples(long(3.5 * L));
  CHECK(rig.eng.status().playing);
  rig.post(CmdType::Stop);
  rig.run(1);
  const long fStop = rig.frames();
  rig.runSeconds(0.5);
  CHECK(!rig.eng.status().playing);
  // Bass: note-off for the gated note, then choke. Every voice gets choked.
  const auto& b = rig.v[kTrackBass].calls;
  CHECK(b.size() == 3 && b[1].type == Call::Off && b[1].note == 45 && b[2].type == Call::Choke);
  for (int t = 0; t < kNumTracks; ++t) CHECK(rig.v[t].chokes().size() == 1);
  CHECK(rig.v[kTrackKick].ons().size() == 4);
  // FX tail keeps ringing after stop (reverb send on the clap), and then dies away.
  const double tail = rmsOf(rig.out, fStop + 2000, fStop + 6000, 0);
  CHECKF(tail > 1e-4, "no reverb tail after stop (rms %.2e)", tail);
  rig.runSeconds(12.0);
  CHECK(peakOf(rig.out, rig.frames() - 4000, rig.frames(), 0) == 0.0);
  CHECK(engine_detail::fxIdle());
  rig.capture = false;
  rig.out.clear();

  // Continue: picks up at the next step, not at the top.
  rig.v[kTrackKick].clearLog();
  rig.post(CmdType::Play, 1);
  rig.runSamples(long(3.2 * L));
  auto k = rig.v[kTrackKick].ons();
  CHECKF(k.size() == 4 && k[0].ev.note == 44 && k[3].ev.note == 47, "continue resumed at note %d", k.empty() ? -1 : k[0].ev.note);
  // Play(continue) while playing changes nothing.
  rig.post(CmdType::Play, 1);
  rig.runSamples(long(1.0 * L));
  k = rig.v[kTrackKick].ons();
  CHECK(k.size() == 5 && k[4].ev.note == 48);
  // Play (from the top) while playing restarts at step 0.
  rig.post(CmdType::Play);
  rig.run(2);
  k = rig.v[kTrackKick].ons();
  CHECK(k.size() == 6 && k[5].ev.note == 40);
  CHECK(rig.eng.status().step[kTrackKick] == 0);
  rig.runSamples(long(2.1 * L));
  CHECK(rig.eng.status().step[kTrackKick] == 2);
}

static void testEdits() {
  rig.init();
  uint32_t rev = 0;
  auto expectRev = [&](int delta, const char* what) {
    rig.run(6);
    rev += uint32_t(delta);
    CHECKF(rig.eng.status().revision == rev, "%s: revision %u, expected %u", what, rig.eng.status().revision, rev);
  };
  // SetStep stores the step, clamping out-of-range fields.
  Command c = cmd(CmdType::SetStep, 2, kTrackLead, 63);
  c.step = {200, kStepOn | kStepAccent, 0, 255, 100, {77, 9, 3}};
  c.step.setProb(127);
  c.step.setChord(15);
  rig.post(c);
  expectRev(1, "SetStep");
  const Step& s = rig.step(2, kTrackLead, 63);
  CHECK(s.note == 127 && s.flags() == (kStepOn | kStepAccent) && s.vel == 1 && s.gate == 255 && s.prob() == 100 && s.chord() == 0 && s.lockParam() == 0);
  c = cmd(CmdType::SetStep, 0, kTrackBass, 0);
  c.step = mkStep(60);
  c.step.setLock(4 + 1, 200);            // stub param 4 has range 5..50
  rig.post(c);
  c.c = 1; c.step.setLock(kMixPan + 1, 255);
  rig.post(c);
  expectRev(2, "SetStep locks");
  CHECK(rig.step(0, kTrackBass, 0).lockValue() == 50 && rig.step(0, kTrackBass, 1).lockValue() == 127);
  // Out-of-range addresses are ignored.
  rig.post(CmdType::SetStep, 16, 0, 0);
  rig.post(CmdType::SetStep, 0, 8, 0);
  rig.post(CmdType::SetStep, 0, 0, 64);
  rig.post(CmdType::SetTrackParam, 8, 0, 1);
  rig.post(CmdType::SetTrackParam, 0, 32, 1);
  rig.post(CmdType::SetGlobal, 32, 0, 1);
  rig.post(CmdType::SetTrackLength, 16, 0, 4);
  rig.post(CmdType::SetTrackLength, 0, 8, 4);
  rig.post(CmdType::SetPatternLength, 16, 0, 4);
  rig.post(CmdType::SetMute, 8, 0, 1);
  rig.post(CmdType::SetSongSlot, 32, 0, 1);
  rig.post(CmdType::SetSongSlot, 0, 16, 1);
  rig.post(CmdType::ClearTrack, 16, 0);
  rig.post(CmdType::ClearTrack, 0, 8);
  rig.post(CmdType::ClearPattern, 16);
  rig.post(CmdType::CopyPattern, 16, 0);
  rig.post(CmdType::CopyPattern, 0, 16);
  rig.post(CmdType::CopyPattern, 3, 3);
  rig.post(CmdType::CopyTrack, 0, kTrackKick, kTrackBass);      // drum -> synth: refused
  rig.post(CmdType::CopyTrack, 0, kTrackLead, kTrackSnare);     // synth -> drum: refused
  rig.post(CmdType::CopyTrack, 0, kTrackKick, 9);
  expectRev(0, "invalid commands");

  // Params clamp to their ParamInfo range.
  rig.post(CmdType::SetTrackParam, kTrackSnare, 4, 200);
  rig.post(CmdType::SetTrackParam, kTrackSnare, 5, 99);
  rig.post(CmdType::SetTrackParam, kTrackSnare, kMixLevel, 255);
  rig.post(CmdType::SetTrackParam, kTrackSnare, 20, 222);       // unused voice slot: stored raw
  expectRev(4, "SetTrackParam");
  CHECK(rig.proj.tracks[kTrackSnare].params[4] == 50 && rig.proj.tracks[kTrackSnare].params[5] == 99 &&
        rig.proj.tracks[kTrackSnare].params[kMixLevel] == 127 && rig.proj.tracks[kTrackSnare].params[20] == 222);
  CHECK(rig.v[kTrackSnare].params[4] == 50 && rig.v[kTrackSnare].params[5] == 99);
  rig.post(CmdType::SetGlobal, kGTempo, 0, 10);
  rig.post(CmdType::SetGlobal, kGSwing, 0, 200);
  rig.post(CmdType::SetGlobal, kGDelayTime, 0, 200);
  rig.post(CmdType::SetGlobal, kGScaleType, 0, 200);
  expectRev(4, "SetGlobal");
  CHECK(rig.proj.globals[kGTempo] == 60 && rig.proj.globals[kGSwing] == 50);
  CHECK(rig.proj.globals[kGDelayTime] == Engine::globalParamInfo(kGDelayTime)->max);
  CHECK(rig.proj.globals[kGScaleType] == Engine::globalParamInfo(kGScaleType)->max);
  CHECK(rig.v[0].tempo == 60.0f);
  rig.post(CmdType::SetTrackLength, 1, 2, 0);
  rig.post(CmdType::SetTrackLength, 1, 3, 100);
  rig.post(CmdType::SetTrackLength, 1, 4, 7);
  rig.post(CmdType::SetPatternLength, 1, 0, 0);
  rig.post(CmdType::SetPatternLength, 2, 0, 200);
  rig.post(CmdType::SetMute, 3, 0, 9);
  rig.post(CmdType::SetSongSlot, 5, 9, 3);
  expectRev(7, "lengths/mute/song");
  CHECK(rig.proj.patterns[1].tracks[2].length == 1 && rig.proj.patterns[1].tracks[3].length == 64 && rig.proj.patterns[1].tracks[4].length == 7);
  CHECK(rig.proj.patterns[1].length == 1 && rig.proj.patterns[2].length == 64 && rig.proj.tracks[3].mute == 1);
  CHECK(rig.proj.song[5].pattern == 9 && rig.proj.song[5].repeats == 3);

  // Copy / clear.
  Step a = mkStep(50, kStepOn | kStepSlide, 90, 3);
  a.setChord(kChordMin); a.setLock(2, 33);
  rig.step(4, kTrackLead, 7) = a;
  Step m = mkStep(51, kStepOn, 80, 2);
  m.setLock(kMixPan + 1, 20);
  rig.step(4, kTrackLead, 8) = m;
  rig.step(4, kTrackKick, 1) = mkStep(61, kStepOn, 70, 1);
  rig.proj.patterns[4].length = 12;
  rig.proj.patterns[4].tracks[kTrackLead].length = 9;
  rig.post(CmdType::CopyPattern, 4, 6);
  expectRev(1, "CopyPattern");
  CHECK(std::memcmp(&rig.proj.patterns[4], &rig.proj.patterns[6], sizeof(Pattern)) == 0);
  rig.post(CmdType::CopyTrack, 6, kTrackLead, kTrackBass);
  rig.post(CmdType::CopyTrack, 6, kTrackKick, kTrackPerc);
  expectRev(2, "CopyTrack");
  const Step& bs = rig.step(6, kTrackBass, 7);
  CHECKF(bs.note == 50 && bs.flags() == (kStepOn | kStepSlide) && bs.vel == 90 && bs.gate == 3 && bs.chord() == 0 && bs.lockParam() == 0,
         "lead->bass copy: chord %d lock %d", bs.chord(), bs.lockParam());
  CHECK(rig.step(6, kTrackBass, 8).lockParam() == kMixPan + 1 && rig.step(6, kTrackBass, 8).lockValue() == 20);   // mixer locks survive
  CHECK(rig.proj.patterns[6].tracks[kTrackBass].length == 9);
  CHECK(std::memcmp(&rig.proj.patterns[6].tracks[kTrackKick], &rig.proj.patterns[6].tracks[kTrackPerc], sizeof(TrackPattern)) == 0);
  rig.post(CmdType::ClearTrack, 6, kTrackLead);
  expectRev(1, "ClearTrack");
  bool clean = rig.proj.patterns[6].tracks[kTrackLead].length == 9;
  for (int i = 0; i < kMaxSteps; ++i) clean &= std::memcmp(&rig.step(6, kTrackLead, i), &kDefaultStep, sizeof(Step)) == 0;
  CHECK(clean && (rig.step(6, kTrackBass, 7).flags() & kStepOn));
  rig.post(CmdType::ClearPattern, 6);
  expectRev(1, "ClearPattern");
  clean = rig.proj.patterns[6].length == 16;
  for (int t = 0; t < kNumTracks; ++t) {
    clean &= rig.proj.patterns[6].tracks[t].length == 16;
    for (int i = 0; i < kMaxSteps; ++i) clean &= std::memcmp(&rig.step(6, t, i), &kDefaultStep, sizeof(Step)) == 0;
  }
  CHECK(clean);
  CHECK(rig.step(4, kTrackLead, 7).note == 50);                  // source untouched
}

static bool upperOrPunct(const char* s) {
  for (; *s; ++s) if (*s >= 'a' && *s <= 'z') return false;
  return true;
}

static void testTablesAndInit() {
  rig.init();
  const Project& p = rig.proj;
  CHECK(p.magic[0] == 'P' && p.magic[1] == 'G' && p.magic[2] == 'R' && p.magic[3] == 'V' && p.version == kProjectVersion && p.songMode == 0);
  for (int i = 0; i < kGNumUsed; ++i) {
    const ParamInfo* info = Engine::globalParamInfo(i);
    CHECK(info != nullptr);
    if (!info) continue;
    CHECKF(std::strlen(info->name) >= 1 && std::strlen(info->name) <= 4 && upperOrPunct(info->name), "global %d name '%s'", i, info->name);
    CHECKF(std::strlen(info->longName) <= 12, "global %d long name '%s'", i, info->longName);
    CHECK(info->min <= info->def && info->def <= info->max);
    if (info->fmt == ParamFormat::Enum) {
      CHECK(info->enumNames != nullptr);
      for (int v = 0; info->enumNames && v <= info->max - info->min; ++v) CHECK(info->enumNames[v] && std::strlen(info->enumNames[v]) >= 1 && std::strlen(info->enumNames[v]) <= 5);
    }
    if (i != kGTempo) CHECK(p.globals[i] == info->def);
  }
  CHECK(Engine::globalParamInfo(-1) == nullptr && Engine::globalParamInfo(kGNumUsed) == nullptr && Engine::globalParamInfo(31) == nullptr);
  CHECK(Engine::globalParamInfo(kGTempo)->min == 60 && Engine::globalParamInfo(kGTempo)->max == 200 && Engine::globalParamInfo(kGTempo)->def == 124);
  CHECK(Engine::globalParamInfo(kGSwing)->max == 50 && Engine::globalParamInfo(kGMasterFilter)->def == 64);
  CHECK(Engine::globalParamInfo(kGScaleType)->max >= 8 && Engine::globalParamInfo(kGScaleRoot)->max == 11);
  CHECK(Engine::globalParamInfo(kGDelayTime)->max >= 4);
  for (int t = 0; t < kNumTracks; ++t) {
    for (int i = 0; i < kParamsPerTrack; ++i) {
      const ParamInfo* info = rig.eng.trackParamInfo(t, i);
      if (i < 8) CHECK(info == rig.v[t].paramInfo(i));
      if (i >= 8 && i < kSoundParams) CHECK(info == nullptr);
      if (i >= kMixLevel && i <= kMixDuck) {
        CHECK(info != nullptr);
        if (info) {
          CHECK(std::strlen(info->name) <= 4 && upperOrPunct(info->name) && std::strlen(info->longName) <= 12 && info->min == 0 && info->max == 127);
          CHECK(p.tracks[t].params[i] == info->def);
        }
      }
      if (i > kMixDuck) CHECK(info == nullptr);
      if (i < 8) CHECK(p.tracks[t].params[i] == rig.v[t].paramInfo(i)->def);
    }
    CHECK(p.tracks[t].mute == 0);
  }
  CHECK(rig.eng.trackParamInfo(-1, 0) == nullptr && rig.eng.trackParamInfo(8, 0) == nullptr && rig.eng.trackParamInfo(0, 32) == nullptr);
  // Brief: kick is not ducked by default, bass heavily, lead moderately.
  CHECK(p.tracks[kTrackKick].params[kMixDuck] == 0);
  CHECK(p.tracks[kTrackBass].params[kMixDuck] >= 96);
  CHECK(p.tracks[kTrackLead].params[kMixDuck] >= 40 && p.tracks[kTrackLead].params[kMixDuck] <= 90);
  CHECK(p.tracks[kTrackKick].params[kMixPan] == 64 && p.tracks[kTrackBass].params[kMixPan] == 64);
  bool ok = true;
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    ok &= p.patterns[pt].length == 16;
    for (int t = 0; t < kNumTracks; ++t) {
      ok &= p.patterns[pt].tracks[t].length == 16;
      for (int s = 0; s < kMaxSteps; ++s) ok &= std::memcmp(&p.patterns[pt].tracks[t].steps[s], &kDefaultStep, sizeof(Step)) == 0;
    }
  }
  CHECK(ok);
  for (int i = 0; i < kSongSlots; ++i) CHECK(p.song[i].repeats == 0);

  // A track without a voice is silent and harmless.
  rig.init();
  rig.eng.setVoice(kTrackKick, nullptr);
  rig.eng.setVoice(kTrackLead, nullptr);
  for (int s = 0; s < 16; ++s) { rig.step(0, kTrackKick, s) = mkStep(60); rig.step(0, kTrackLead, s) = mkStep(60, kStepOn | kStepSlide); }
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.post(noteOn(kTrackKick, 60, 100));
  rig.runSeconds(1.0);
  CHECK(peakOf(rig.out, 0, rig.frames(), 0) == 0.0 && rig.eng.trackParamInfo(kTrackKick, 0) == nullptr);
  CHECK(rig.eng.status().trig[kTrackKick] > 4);
  rig.capture = false;

  // process() with a frame count that is not a multiple of the quantum zero-fills the remainder.
  int16_t buf[2 * 40];
  for (int16_t& x : buf) x = 1234;
  rig.eng.process(buf, 40);
  CHECK(buf[2 * 32] == 0 && buf[2 * 39 + 1] == 0);

  std::printf("  static RAM: engine state %zu bytes (delay line %zu, reverb %zu)\n", engine_detail::staticRamBytes(),
              engine_detail::delayRamBytes(), engine_detail::reverbRamBytes());
  CHECK(engine_detail::reverbRamBytes() < 28 * 1024 + 512);
  CHECK(engine_detail::delayRamBytes() < 68 * 1024);
  CHECK(engine_detail::staticRamBytes() < 104 * 1024);
}

int main() {
  testLiveNotes();
  testRecord();
  testQueue();
  testSuspend();
  testTransport();
  testEdits();
  testTablesAndInit();
  return finish("test_engine_live");
}
