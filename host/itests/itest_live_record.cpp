// Live recording on the synth tracks through the real Ui + Engine: what the
// player holds is what the pattern plays back (MANUAL 4.3 / 6.3, the "long held
// notes come back as stabs" report). Each scenario records with the note keys,
// checks the recorded steps (gate, slide, chord), and measures how long the
// lead actually sounds on a later pass against how it sounded while held. Each
// scenario runs on the synth lead (HANDS), CLASSIC and the lead sampler (CLOUD).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/ui_harness.h"
#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::ui;
namespace pm = pg::macro;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fail;                                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    long _a = long(a), _b = long(b);                                                                \
    if (_a != _b) {                                                                                 \
      ++g_fail;                                                                                     \
      std::printf("FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b);       \
    }                                                                                               \
  } while (0)

static uih::Rig* R;
static int K(const char* n) { return keyFromName(n); }
static void tap(const char* n, uint32_t ms = 40) { R->tap(K(n), ms); R->run(20); }
static void down(const char* n) { R->down(K(n)); }
static void up(const char* n) { R->up(K(n)); }
static const Step& LS(int i, int pat = 0) { return R->proj->patterns[pat].tracks[kTrackLead].steps[i]; }

static const pm::MacroSet* findSet(int t, const char* name) {
  for (int i = 0; i < pm::instrumentCount(t); ++i)
    if (std::strcmp(pm::instrumentAt(t, i)->name, name) == 0) return pm::instrumentAt(t, i);
  return nullptr;
}

// One audio block at a time, the level of each (dB of the block's RMS).
static double g_level = -200.0;
static void block() {
  R->run(1);
  double s = 0.0;
  for (int i = 0; i < uih::Rig::kBlock * 2; ++i) s += double(R->audio[i]) * double(R->audio[i]);
  g_level = 10.0 * std::log10(s / (uih::Rig::kBlock * 2) / (32767.0 * 32767.0) + 1e-20);
}
static int curStep() { return R->eng.status().step[kTrackLead]; }
// Runs until the lead's playhead is on step n (blocks), returns false on timeout.
static bool toStep(int n, int maxMs = 40000) {
  for (int i = 0; i < maxMs; ++i) {
    if (curStep() == n) return true;
    block();
  }
  return false;
}
// Runs until step n, then a few ms into it: a press that lands on step n.
static bool onStep(int n) {
  if (!toStep(n)) return false;
  for (int i = 0; i < 4; ++i) block();
  return true;
}
// The lead's level at steps: runs from the current position to step `to` (of the
// next time it comes round), sampling the level in the middle of each listed step.
struct Probe {
  int step;
  double db;
};
static void probe(Probe* p, int n) {
  for (int i = 0; i < n; ++i) {
    toStep(p[i].step);
    for (int k = 0; k < 40; ++k) block();   // ~60 ms into the step (a step is 125 ms at 120 BPM)
    double mx = -200.0;
    for (int k = 0; k < 8; ++k) { block(); mx = std::fmax(mx, g_level); }
    p[i].db = mx;
  }
}

static void setup(const char* inst, int len) {
  R->devMode = false;
  R->start(false);
  R->run(100);
  if (inst) R->ui.setInstrument(kTrackLead, findSet(kTrackLead, inst)->id);
  Command c{};
  c.type = CmdType::SetPatternLength; c.a = 0; c.c = uint8_t(len);
  R->eng.post(c);
  c.type = CmdType::SetTrackLength; c.a = 0; c.b = kTrackLead; c.c = uint8_t(len);
  R->eng.post(c);
  R->run(100);
  down("shift"); tap("k"); up("shift");    // the lead
  R->run(50);
  CHECK_EQ(R->ui.selTrack(), kTrackLead);
}

static void startRecording() {
  tap("spc");
  tap("ent");
  CHECK(R->eng.status().recording);
}
static void stopRecording() {
  tap("ent");
  CHECK(!R->eng.status().recording);
}

// THE REPORT: a 64-step pattern, three notes played legato (each next key down
// before the previous one is up), each held for more than a page, the last one
// held past the end of the pattern. Recorded: three long connected notes.
static void testLegatoLongNotes(const char* inst) {
  std::printf("legato, long notes over pages, %s\n", inst ? inst : "CLASSIC");
  setup(inst, 64);
  startRecording();
  Probe live[4] = {{12, 0}, {32, 0}, {55, 0}, {4, 0}};
  CHECK(onStep(2));
  down("z");                                  // A on step 2
  probe(&live[0], 1);
  CHECK(onStep(22));
  down("x");                                  // B on step 22 (A still down)
  for (int i = 0; i < 40; ++i) block();
  up("z");
  probe(&live[1], 1);
  CHECK(onStep(44));
  down("c");                                  // C on step 44 (B still down)
  for (int i = 0; i < 40; ++i) block();
  up("x");
  probe(&live[2], 1);
  probe(&live[3], 1);                         // through the loop point, still held
  CHECK(onStep(6));
  up("c");                                    // C released on step 6 of the next pass
  for (int i = 0; i < 20; ++i) block();
  stopRecording();
  const Step a = LS(2), b = LS(22), c = LS(44);
  std::printf("  recorded: A gate %d flags %02x, B gate %d flags %02x, C gate %d flags %02x\n", a.gate, a.flags(),
              b.gate, b.flags(), c.gate, c.flags());
  CHECK((a.flags() & kStepOn) && (b.flags() & kStepOn) && (c.flags() & kStepOn));
  CHECK(a.flags() & kStepSlide);
  CHECK(b.flags() & kStepSlide);
  CHECK(!(c.flags() & kStepSlide));            // nothing followed it legato
  CHECK_EQ(a.gate, 20 * 4);                    // reaches B
  CHECK_EQ(b.gate, 22 * 4);                    // reaches C
  CHECK(c.gate >= 25 * 4 && c.gate <= 27 * 4); // its hold: step 44 .. step 6 of the next pass
  bool others = false;
  for (int i = 0; i < 64; ++i) others |= (i != 2 && i != 22 && i != 44 && (LS(i).flags() & kStepOn));
  CHECK(!others);
  // Playback: a whole pass later, the same probes (and the tail of C into the next pass).
  Probe play[5] = {{12, 0}, {32, 0}, {55, 0}, {4, 0}, {12, 0}};
  toStep(10);
  probe(play, 5);
  std::printf("  level held live : A %.1f  B %.1f  C %.1f  C after the loop %.1f dB\n", live[0].db, live[1].db,
              live[2].db, live[3].db);
  std::printf("  level played    : A %.1f  B %.1f  C %.1f  C after the loop %.1f  A again %.1f dB\n", play[0].db,
              play[1].db, play[2].db, play[3].db, play[4].db);
  for (int i = 0; i < 4; ++i) CHECK(play[i].db > live[i].db - 6.0 && play[i].db > -45.0);
  CHECK(play[4].db > -45.0);
  tap("spc");
}

// One key held longer than the whole (16-step) pattern: the player's hand wins
// while it is down (its own freshly recorded step coming round does not cut it),
// and the gate is capped at the loop.
static void testHeldPastLoop(const char* inst) {
  std::printf("held past the loop, %s\n", inst ? inst : "CLASSIC");
  setup(inst, 16);
  startRecording();
  CHECK(onStep(4));
  down("z");
  Probe live[3] = {{10, 0}, {5, 0}, {9, 0}};   // in the pass, after its own step comes round
  probe(live, 3);
  CHECK(onStep(12));                           // released on step 12 of the next pass: 24 steps
  up("z");
  for (int i = 0; i < 20; ++i) block();
  stopRecording();
  std::printf("  live: %.1f %.1f %.1f dB; recorded gate %d flags %02x\n", live[0].db, live[1].db, live[2].db,
              LS(4).gate, LS(4).flags());
  CHECK(live[1].db > live[0].db - 6.0 && live[2].db > live[0].db - 6.0);
  CHECK_EQ(LS(4).gate, 16 * 4);                // capped at the loop
  CHECK(!(LS(4).flags() & kStepSlide));
  toStep(0);                                   // the next pass
  Probe play[2] = {{12, 0}, {2, 0}};
  probe(play, 2);
  std::printf("  played: %.1f (step 12)  %.1f (step 2, next pass) dB\n", play[0].db, play[1].db);
  CHECK(play[0].db > live[0].db - 6.0 && play[1].db > live[0].db - 6.0);
  tap("spc");
}

// Legato across the loop point: A held from step 10 into B pressed on step 2 of
// the next pass. A reaches B through the wrap and slides into it on playback.
static void testLegatoAcrossLoop(const char* inst) {
  std::printf("legato across the loop point, %s\n", inst ? inst : "CLASSIC");
  setup(inst, 16);
  startRecording();
  CHECK(onStep(10));
  down("z");
  CHECK(onStep(2));
  down("c");
  for (int i = 0; i < 40; ++i) block();
  up("z");
  CHECK(onStep(6));
  up("c");
  for (int i = 0; i < 20; ++i) block();
  stopRecording();
  std::printf("  A (10) gate %d flags %02x, B (2) gate %d flags %02x\n", LS(10).gate, LS(10).flags(), LS(2).gate,
              LS(2).flags());
  CHECK(LS(10).flags() & kStepSlide);
  CHECK_EQ(LS(10).gate, 8 * 4);
  CHECK(!(LS(2).flags() & kStepSlide));
  CHECK(LS(2).gate >= 15 && LS(2).gate <= 17);
  toStep(8);
  Probe play[3] = {{13, 0}, {0, 0}, {4, 0}};
  probe(play, 3);
  std::printf("  played: %.1f (A, step 13)  %.1f (A, step 0)  %.1f (B, step 4) dB\n", play[0].db, play[1].db, play[2].db);
  for (int i = 0; i < 3; ++i) CHECK(play[i].db > -45.0);
  tap("spc");
}

// A hand-played chord: three keys within a few ms, held, released in any order.
// They land on one step; the step gets the gate of the last key released.
static void testHandChord(const char* inst) {
  std::printf("hand chord, %s\n", inst ? inst : "CLASSIC");
  for (int order = 0; order < 3; ++order) {
    setup(inst, 16);
    startRecording();
    CHECK(onStep(4));
    down("z"); block(); block();
    down("c"); block(); block();
    down("b");
    for (int i = 0; i < 6 * 86; ++i) block();   // ~6 steps
    const char* rel[3][3] = {{"z", "c", "b"}, {"b", "c", "z"}, {"c", "z", "b"}};
    for (int k = 0; k < 3; ++k) {   // a step apart: the last release decides
      up(rel[order][k]);
      if (k < 2) for (int i = 0; i < 86; ++i) block();
    }
    for (int i = 0; i < 20; ++i) block();
    stopRecording();
    std::printf("  release order %d: step 4 gate %d flags %02x note %d\n", order, LS(4).gate, LS(4).flags(), LS(4).note);
    CHECK(LS(4).gate >= 30 && LS(4).gate <= 34);   // first press .. last release: ~8 steps
    CHECK(!(LS(4).flags() & kStepSlide));
    int on = 0;
    for (int i = 0; i < 16; ++i) on += (LS(i).flags() & kStepOn) ? 1 : 0;
    CHECK_EQ(on, 1);
    tap("spc");
  }
}

// The lead's chord mode: one key plays (and records) a chord with the held gate.
static void testChordMode(const char* inst) {
  std::printf("chord mode, %s\n", inst ? inst : "CLASSIC");
  setup(inst, 16);
  tap("l");                                    // chord mode on
  startRecording();
  CHECK(onStep(4));
  down("z");
  for (int i = 0; i < 8 * 86; ++i) block();     // ~8 steps
  up("z");
  for (int i = 0; i < 20; ++i) block();
  stopRecording();
  std::printf("  step 4 gate %d chord %d\n", LS(4).gate, LS(4).chord());
  CHECK(LS(4).chord() != 0);
  CHECK(LS(4).gate >= 30 && LS(4).gate <= 34);
  tap("spc");
}

// Pressed just before a step (it lands on the upcoming one, which is then not
// fired once) and just before the end with another pattern queued.
static void testUpcomingAndQueued() {
  std::printf("upcoming step, queued pattern\n");
  setup("HANDS", 16);
  startRecording();
  CHECK(toStep(7));
  for (int i = 0; i < 70; ++i) block();         // late in step 7: lands on step 8
  down("z");
  for (int i = 0; i < 3 * 86; ++i) block();
  up("z");
  for (int i = 0; i < 10; ++i) block();
  std::printf("  step 8 gate %d (step 7 on: %d)\n", LS(8).gate, (LS(7).flags() & kStepOn) ? 1 : 0);
  CHECK(LS(8).flags() & kStepOn);
  CHECK(!(LS(7).flags() & kStepOn));
  CHECK(LS(8).gate >= 10 && LS(8).gate <= 14);
  down("opt"); tap("2"); up("opt");            // queue P02
  R->run(20);
  CHECK_EQ(R->eng.status().queuedPattern, 1);
  CHECK(toStep(15));
  for (int i = 0; i < 70; ++i) block();         // late in the last step: lands on P02 step 0
  down("x");
  for (int i = 0; i < 4 * 86; ++i) block();
  up("x");
  for (int i = 0; i < 10; ++i) block();
  stopRecording();
  std::printf("  P02 step 0 gate %d flags %02x, playing P%02d\n", LS(0, 1).gate, LS(0, 1).flags(),
              R->eng.status().pattern + 1);
  CHECK(LS(0, 1).flags() & kStepOn);
  CHECK(LS(0, 1).gate >= 14 && LS(0, 1).gate <= 18);
  tap("spc");
}

// STEP EDIT on a recorded long gate: shown as steps, nudged by one, not clamped
// back to 16 steps; editing another field leaves it alone.
static void testStepEditLongGate() {
  std::printf("step edit, a gate longer than 16 steps\n");
  setup("HANDS", 64);
  startRecording();
  CHECK(onStep(2));
  down("z");
  CHECK(onStep(30));
  up("z");
  for (int i = 0; i < 10; ++i) block();
  stopRecording();
  tap("spc");
  R->run(100);
  const int g = LS(2).gate;
  std::printf("  recorded gate %d\n", g);
  CHECK(g >= 110 && g <= 114);
  down("3");                                   // STEP EDIT on step 3 (index 2)
  R->run(400);
  CHECK(R->ui.stepEditOpen());
  tap("=");                                    // VEL + 1
  CHECK_EQ(LS(2).gate, g);
  tap("s"); tap("-");                          // GATE - 1
  CHECK_EQ(LS(2).gate, g - 1);
  tap("=");
  tap("=");
  CHECK_EQ(LS(2).gate, g + 1);
  up("3");
  R->run(100);
}

int main() {
  static uih::Rig rig;
  R = &rig;
  testLegatoLongNotes("HANDS");
  testLegatoLongNotes(nullptr);
  testHeldPastLoop("HANDS");
  testHeldPastLoop(nullptr);
  testLegatoAcrossLoop("HANDS");
  testLegatoAcrossLoop(nullptr);
  testHandChord("HANDS");
  testHandChord(nullptr);
  testChordMode("HANDS");
  testChordMode(nullptr);
  // the same on a lead SAMPLER instrument (CLOUD: a held note loops, so a long gate sounds long)
  testLegatoLongNotes("CLOUD");
  testHeldPastLoop("CLOUD");
  testLegatoAcrossLoop("CLOUD");
  testHandChord("CLOUD");
  testChordMode("CLOUD");
  testUpcomingAndQueued();
  testStepEditLongGate();
  std::printf("itest_live_record: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
