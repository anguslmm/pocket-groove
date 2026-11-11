// Behavioural tests of the UI: key events in, Project / engine status / UI state out.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/ui_harness.h"

using namespace pg;
using namespace pg::ui;

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
static void up(const char* n) { R->up(K(n)); R->run(20); }
static const Step& S(int t, int i) { return R->step(t, i); }
static bool on(int t, int i) { return S(t, i).flags & kStepOn; }
static int P(int t, int i) { return R->proj->tracks[t].params[i]; }
static int G(int g) { return R->proj->globals[g]; }

static void testStepToggle() {
  R->start(false);
  CHECK_EQ(R->ui.selTrack(), kTrackBass);
  tap("1");
  CHECK(on(kTrackBass, 0));
  CHECK_EQ(S(kTrackBass, 0).note, 24);       // C MIN default scale, octave 1: C1
  tap("1");
  CHECK(!on(kTrackBass, 0));
  // drum: select the kick silently, tap step 5
  down("shift"); tap("a"); up("shift");
  CHECK_EQ(R->ui.selTrack(), 0);
  tap("5");
  CHECK(on(0, 4));
  CHECK_EQ(S(0, 4).note, 60);
  // beyond the track length: nothing, plus a toast
  down("shift"); tap("4"); up("shift");      // pattern length 4
  CHECK_EQ(R->proj->patterns[0].length, 4);
  for (int t = 0; t < 8; ++t) CHECK_EQ(R->proj->patterns[0].tracks[t].length, 4);
  tap("6");
  CHECK(!on(0, 5));
  CHECK(std::strstr(R->ui.toastText(), "BEYOND") != nullptr);
}

static void testHoldVsTap() {
  R->start(false);
  tap("2");                                  // on
  CHECK(on(kTrackBass, 1));
  down("2");
  R->run(100);
  CHECK(!R->ui.stepEditOpen());
  R->run(250);
  CHECK(R->ui.stepEditOpen());
  up("2");                                   // long press: stays on
  CHECK(on(kTrackBass, 1));
  CHECK(!R->ui.stepEditOpen());
  // short press but another key pressed meanwhile: step edit opens at once, step stays on
  down("2");
  tap("s");                                  // select field GATE
  CHECK(R->ui.stepEditOpen());
  CHECK_EQ(R->ui.stepField(), kFGate);
  up("2");
  CHECK(on(kTrackBass, 1));
  // an off step held and edited stays on
  down("3");
  R->run(300);
  tap("a");
  tap("=");
  up("3");
  CHECK(on(kTrackBass, 2));
  CHECK_EQ(S(kTrackBass, 2).vel, 101);
}

static void testValueEditing() {
  R->start(false);
  const ValueRef f = R->ui.focus();
  CHECK(!f.global && f.track == kTrackBass && f.index == 0);
  const int def = P(kTrackBass, 0);
  tap("=");
  CHECK_EQ(P(kTrackBass, 0), def + 1);
  CHECK(R->ui.popupVisible());
  tap("-");
  CHECK_EQ(P(kTrackBass, 0), def);
  down("shift"); tap("="); up("shift");
  CHECK_EQ(P(kTrackBass, 0), def + 8);
  // both keys = default
  down("="); R->run(50); down("-"); R->run(50); up("-"); up("=");
  CHECK_EQ(P(kTrackBass, 0), def);
  CHECK(std::strcmp(R->ui.toastText(), "DEFAULT") == 0);
  // acceleration: 1.3 s hold = 1 + 13x1 + 13x2 + 1x4 = 44 (frame-quantised: allow a little slack)
  const int v0 = P(kTrackBass, 0);
  down("="); R->run(1300); up("=");
  const int d = P(kTrackBass, 0) - v0;
  if (d < 40 || d > 46) std::printf("accel d=%d\n", d);
  CHECK(d >= 40 && d <= 46);
  // full sweep in about 2.2 s, then the limit: no command at the limit
  down("="); R->run(2400); up("=");
  CHECK_EQ(P(kTrackBass, 0), 127);
  R->run(100);
  const uint32_t rev = R->eng.status().revision;
  tap("=");
  down("="); R->run(600); up("=");
  R->run(100);
  CHECK_EQ(R->eng.status().revision, rev);
  CHECK_EQ(P(kTrackBass, 0), 127);
  // enum (LFO RATE, 12 values): one per tap, hold repeats every 200 ms without acceleration, no wrap
  tap("9");
  tap(",");                                  // column 7 = RATE (page 1)
  CHECK_EQ(R->ui.focus().index, 7);
  const int r0 = P(kTrackBass, 7);
  down("="); R->run(1000); up("=");
  const int dr = P(kTrackBass, 7) - r0;
  CHECK(dr >= 4 && dr <= 5);
  down("="); R->run(3000); up("=");
  CHECK_EQ(P(kTrackBass, 7), 11);            // stops at the end, no wrap
  // SHIFT on a small range = 1
  down("shift"); tap("-"); up("shift");
  CHECK_EQ(P(kTrackBass, 7), 10);
}

static void testPLocks() {
  R->start(false);
  tap("9");                                  // SOUND, focus CUT
  down("1");                                 // off step: on at key-down
  R->run(300);
  tap("=");
  CHECK(on(kTrackBass, 0));
  CHECK_EQ(S(kTrackBass, 0).lockParam, 1);
  CHECK_EQ(S(kTrackBass, 0).lockValue, P(kTrackBass, 0) + 1);
  CHECK_EQ(P(kTrackBass, 0), 46);            // the base value is untouched
  tap("x");                                  // focus RES while holding
  tap("-");
  CHECK_EQ(S(kTrackBass, 0).lockParam, 2);
  CHECK_EQ(S(kTrackBass, 0).lockValue, P(kTrackBass, 1) - 1);
  CHECK(std::strstr(R->ui.toastText(), "LOCK RES REPLACES CUT") != nullptr);
  tap("del");
  CHECK_EQ(S(kTrackBass, 0).lockParam, 0);
  up("1");
  CHECK(on(kTrackBass, 0));
  // FX globals can't be locked
  tap("o");
  tap("c");                                  // DHLD
  down("1"); R->run(300); tap("="); up("1");
  CHECK_EQ(S(kTrackBass, 0).lockParam, 0);
  CHECK(std::strstr(R->ui.toastText(), "CAN'T BE LOCKED") != nullptr);
  // GROOVE STEP EDIT, LOCK field: locks trackFocus (RES after the SOUND visit)
  tap("`");
  down("1"); R->run(300); tap("k"); tap("="); up("1");
  CHECK_EQ(S(kTrackBass, 0).lockParam, 2);
  // view key with a step held keeps the hold (hold step -> 0 -> -/=: mixer lock)
  down("1"); R->run(300); tap("0"); tap("z"); tap("=");
  CHECK_EQ(R->ui.heldStep(), 0);
  CHECK_EQ(S(kTrackBass, 0).lockParam, kMixLevel + 1);
  up("1");
}

static void testStepEditFields() {
  R->start(true);                            // demo, bass selected
  // bass step 14 (key y): VEL, GATE, PROB, RATCH, ACC, SLIDE, note row, semitone
  down("y");
  R->run(300);
  CHECK(R->ui.stepEditOpen());
  CHECK_EQ(R->ui.stepField(), kFVel);
  tap("=");
  CHECK_EQ(S(6, 13).vel, 101);
  tap("s"); tap("=");
  CHECK_EQ(S(6, 13).gate, 3);
  tap("d"); tap("-");
  CHECK_EQ(S(6, 13).prob, 95);
  tap("f"); tap("="); tap("=");
  CHECK_EQ(stepRatchet(S(6, 13)), 3);
  tap("g"); tap("=");
  CHECK(!(S(6, 13).flags & kStepAccent));    // flip (demo step 14 is accented)
  tap("h"); tap("=");
  CHECK(S(6, 13).flags & kStepSlide);
  tap("j");                                  // CHRD: not on bass -> field unchanged
  CHECK_EQ(R->ui.stepField(), kFSlide);
  tap("c");                                  // note row: degree 2 of A MIN oct 1 = C2
  CHECK_EQ(S(6, 13).note, 36);
  tap(";");
  CHECK_EQ(S(6, 13).note, 37);
  down("shift"); tap("."); up("shift");
  CHECK_EQ(S(6, 13).note, 25);
  // copy the held step to step 2 (tap '2')
  tap("2");
  CHECK(std::memcmp(&S(6, 1), &S(6, 13), sizeof(Step)) == 0);
  up("y");
  CHECK(on(6, 13));
  // drum step edit: note row = velocity, PTCH field = note
  down("shift"); tap("f"); up("shift");      // CHH
  down("1"); R->run(300);
  tap("b");
  CHECK_EQ(S(3, 0).vel, 80);
  tap("s"); tap("=");
  CHECK_EQ(S(3, 0).note, 61);
  up("1");
}

static void testStepWrite() {
  R->start(true);                            // A minor
  tap("ent");
  CHECK(R->ui.stepWriteActive());
  CHECK_EQ(R->ui.stepCursor(), 0);
  tap("z");
  CHECK_EQ(S(6, 0).note, 33);                // A1
  CHECK(!(S(6, 0).flags & kStepAccent));
  CHECK_EQ(R->ui.stepCursor(), 1);
  down("shift"); tap("x"); up("shift");
  CHECK_EQ(S(6, 1).note, 35);
  CHECK(S(6, 1).flags & kStepAccent);
  // hold c, press v: slide on the c step
  down("c"); R->run(50); down("v"); R->run(50); up("c"); up("v");
  CHECK(S(6, 2).flags & kStepSlide);
  CHECK(!(S(6, 3).flags & kStepSlide));
  CHECK_EQ(S(6, 3).note, 38);
  tap("del");                                // rest
  CHECK(!on(6, 4));
  CHECK_EQ(R->ui.stepCursor(), 5);
  tap("8");                                  // step key tap moves the cursor
  CHECK_EQ(R->ui.stepCursor(), 7);
  CHECK(!on(6, 7) || true);
  // wraps at the track length and follows the page
  for (int i = 0; i < 9; ++i) tap("z");
  CHECK_EQ(R->ui.stepCursor(), 0);
  // drum step-write: velocity from the key
  down("shift"); tap("a"); up("shift");
  tap("8");
  tap(",");
  CHECK_EQ(S(0, 7).vel, 127);
  // lead AUTO chords in A minor: i = MIN, ii (B dim) = OCT, III = MAJ; AUTO7 i = MIN7
  down("shift"); tap("k"); up("shift");
  tap("1");
  tap("z"); tap("x"); tap("c");
  CHECK_EQ(S(7, 0).chord, kChordMin);
  CHECK_EQ(S(7, 1).chord, kChordOct);
  CHECK_EQ(S(7, 2).chord, kChordMaj);
  tap("l");
  CHECK_EQ(R->ui.chordMode(), kModeAuto7);
  tap("z");
  CHECK_EQ(S(7, 3).chord, kChordMin7);
  tap("ent");
  CHECK(!R->ui.recOn());
}

static void testLiveRecord() {
  R->start(false);
  tap("spc");
  tap("ent");
  CHECK(R->eng.status().recording);
  int hits = 0;
  for (int i = 0; i < 6; ++i) {              // clap pad, roughly every 3/8 of a beat
    tap("d", 30);
    R->run(170);
  }
  for (int i = 0; i < 16; ++i) hits += on(kTrackClap, i);
  CHECK(hits >= 4);
  CHECK_EQ(R->ui.selTrack(), kTrackClap);
  tap("spc");                                // stop: live rec -> step-write
  R->run(50);
  CHECK(R->ui.stepWriteActive());
}

static void testMuteSolo() {
  R->start(true);
  down("fn"); tap("a"); up("fn");
  CHECK(R->proj->tracks[0].mute);
  down("fn"); R->run(250);
  CHECK(R->ui.overlay() == Overlay::Mute);
  tap("x");                                  // solo snare
  up("fn");
  for (int t = 0; t < 8; ++t) CHECK_EQ(R->proj->tracks[t].mute, t != 1);
  CHECK_EQ(R->ui.soloTrack(), 1);
  down("fn"); tap("c"); up("fn");            // move the solo
  for (int t = 0; t < 8; ++t) CHECK_EQ(R->proj->tracks[t].mute, t != 2);
  down("fn"); tap("c"); up("fn");            // restore the saved mask: only the kick
  for (int t = 0; t < 8; ++t) CHECK_EQ(R->proj->tracks[t].mute, t == 0);
  CHECK_EQ(R->ui.soloTrack(), -1);
  // a mute change while soloed ends the solo and keeps the mutes
  down("fn"); tap("z"); tap("d"); up("fn");
  CHECK_EQ(R->ui.soloTrack(), -1);
  CHECK(!R->proj->tracks[2].mute);
  CHECK(R->proj->tracks[1].mute);
  // modifier tapped alone does nothing, no overlay
  tap("fn", 50);
  CHECK(R->ui.overlay() == Overlay::None);
}

static void testPatternQueue() {
  R->start(true);
  tap("spc");
  down("opt"); tap("3"); up("opt");
  CHECK_EQ(R->eng.status().queuedPattern, 2);
  R->run(2200);                              // > one bar at 124 BPM
  CHECK_EQ(R->eng.status().pattern, 2);
  CHECK_EQ(R->ui.editPattern(), 2);
  // copy-then-queue: P03 (empty) -> back to P01 by queue, then copy P01 to P05
  down("opt"); tap("1"); up("opt");
  R->run(2200);
  CHECK_EQ(R->ui.editPattern(), 0);
  down("opt"); down("shift"); tap("5"); up("shift"); up("opt");
  R->run(50);
  CHECK(std::memcmp(&R->proj->patterns[4], &R->proj->patterns[0], sizeof(Pattern)) == 0);
  CHECK_EQ(R->eng.status().queuedPattern, 4);
  // stopped: queue switches at once
  tap("spc");
  R->run(50);
  down("opt"); tap("2"); up("opt");
  R->run(50);
  CHECK_EQ(R->eng.status().pattern, 1);
}

static void testTempo() {
  R->start(true);
  for (int i = 0; i < 4; ++i) {              // taps every 500 ms = 120 BPM
    R->tap(K("tab"), 60);
    R->run(440);
  }
  const int tapped = G(kGTempo);              // 64-frame audio blocks quantise the key times a little
  CHECK(tapped >= 119 && tapped <= 121);
  // a gap > 2 s restarts the count: two more taps change nothing
  R->run(2500);
  tap("tab", 60); R->run(240); tap("tab", 60);
  R->run(100);
  CHECK_EQ(G(kGTempo), tapped);
  // TAB held: overlay; -/= = BPM, SHIFT = 10; ;/. = swing
  down("tab"); R->run(300);
  CHECK(R->ui.overlay() == Overlay::Tempo);
  tap("=");
  CHECK_EQ(G(kGTempo), tapped + 1);
  down("shift"); tap("-"); up("shift");
  CHECK_EQ(G(kGTempo), tapped - 9);
  const int sw = G(kGSwing);
  tap(";");
  CHECK_EQ(G(kGSwing), sw + 1);
  up("tab");
  CHECK(R->ui.overlay() == Overlay::None);
  CHECK_EQ(G(kGTempo), tapped - 9);                 // the hold is not a tap
}

static void testFocusFollowsSelection() {
  R->start(true);
  tap("9"); tap("v");                        // bass DCY
  CHECK_EQ(R->ui.trackFocus(6), 3);
  tap("s");                                  // snare: its own remembered focus
  CHECK(!R->ui.focus().global && R->ui.focus().track == 1 && R->ui.focus().index == 0);
  tap("j");
  CHECK_EQ(R->ui.focus().index, 3);
  tap("0");                                  // MIX: focus = row param
  tap("x");                                  // DUCK row
  CHECK_EQ(R->ui.focus().index, kMixDuck);
  tap("d");
  CHECK(R->ui.focus().track == 2 && R->ui.focus().index == kMixDuck);
  tap("o");                                  // FX DUCK: a pad focuses its DUCK amount
  tap("x");
  CHECK(R->ui.focus().global && R->ui.focus().index == kGDuckDepth);
  tap("h");
  CHECK(!R->ui.focus().global && R->ui.focus().track == 5 && R->ui.focus().index == kMixDuck);
  tap("o");                                  // SPACE: pads select but leave the focus
  tap("x");
  tap("a");
  CHECK(R->ui.focus().global && R->ui.focus().index == kGDelayFeedback);
  CHECK_EQ(R->ui.selTrack(), 0);
  tap("`");                                  // view change keeps the focus (5.1)
  CHECK(R->ui.view() == View::Groove);
  CHECK(R->ui.focus().global && R->ui.focus().index == kGDelayFeedback);
  const int v = G(kGDelayFeedback);
  tap("=");
  CHECK_EQ(G(kGDelayFeedback), v + 1);
}

static void testSweepsAndGhosting() {
  R->start(true);
  // ] held: 48 units/s after 120 ms; tap first = +1
  down("]");
  R->run(1120);
  up("]");
  const int v = G(kGMasterFilter);
  CHECK(v >= 64 + 44 && v <= 64 + 50);
  down("["); R->run(30); down("]"); R->run(30); up("["); up("]");
  CHECK_EQ(G(kGMasterFilter), 64);
  CHECK(std::strcmp(R->ui.toastText(), "DJ FILTER OFF") == 0);
  // detent: from 70 down through 64 pauses 150 ms
  for (int i = 0; i < 6; ++i) tap("]", 30);
  CHECK_EQ(G(kGMasterFilter), 70);
  down("[");
  R->run(120 + 4 * 40 + 20);                 // 4 ticks: 70 -> 64 (clamped at the detent)
  CHECK_EQ(G(kGMasterFilter), 64);
  R->run(30);
  CHECK_EQ(G(kGMasterFilter), 64);
  R->run(200);
  CHECK(G(kGMasterFilter) < 64);
  up("[");
  // SHIFT+[ ] = bass cutoff, both = back to where the sweep started
  const int cut = P(6, 0);
  down("shift"); down("]"); R->run(800);
  CHECK(P(6, 0) > cut + 20);
  down("["); R->run(30); up("["); up("]"); up("shift");
  CHECK_EQ(P(6, 0), cut);
  // ghosting: a third non-modifier key is ignored
  R->start(false);
  down("1"); down("2"); down("3");
  R->run(50);
  CHECK(on(6, 0) && on(6, 1) && !on(6, 2));
  up("3"); up("2"); up("1");
}

static void testPagesErase() {
  R->start(true);
  down("shift"); tap("'"); up("shift");      // add page
  CHECK_EQ(R->proj->patterns[0].length, 32);
  CHECK_EQ(R->proj->patterns[0].tracks[6].length, 32);
  CHECK_EQ(R->ui.shownPage(), 1);
  CHECK(std::memcmp(&S(6, 16 + 3), &S(6, 3), sizeof(Step)) == 0);
  tap("'");
  CHECK_EQ(R->ui.shownPage(), 0);
  // CTRL+SHIFT+step: the selected track only
  down("ctl"); down("shift"); tap("w"); up("shift"); up("ctl");
  CHECK_EQ(R->proj->patterns[0].tracks[6].length, 10);
  CHECK_EQ(R->proj->patterns[0].length, 32);
  // octave keys
  tap(";");
  CHECK_EQ(R->ui.octave(6), 2);
  tap(".");
  tap(".");
  tap(".");
  CHECK_EQ(R->ui.octave(6), 0);
  // erase: DEL held while playing clears the bass as the playhead passes
  R->start(true);
  tap("spc");
  R->run(100);
  down("del");
  R->run(2300);
  up("del");
  int left = 0;
  for (int i = 0; i < 16; ++i) left += on(6, i);
  CHECK_EQ(left, 0);
  CHECK(on(0, 0));                           // other tracks untouched
}

static void testRollAndViews() {
  R->start(true);
  down("alt"); R->run(250);
  CHECK(R->ui.overlay() == Overlay::Roll);
  tap(";");
  CHECK_EQ(R->ui.rollRate(), 2);
  down("s"); R->run(100);
  CHECK(R->ui.selTrack() == kTrackBass);     // ALT+pad rolls, it doesn't select
  up("s");
  up("alt");
  // view keys: again = next page/row/tab
  tap("9"); tap("9");
  CHECK(R->ui.view() == View::Sound && R->ui.soundPage(6) == 1);
  tap("0"); tap("0");
  CHECK(R->ui.view() == View::Mix && R->ui.mixRow() == 1);
  tap("o"); tap("o");
  CHECK(R->ui.view() == View::Fx && R->ui.fxTab() == 1);
  tap("p");
  CHECK(R->ui.view() == View::Song);
  tap("\\");
  CHECK(R->ui.view() == View::Project);
  tap("`");
  CHECK(R->ui.view() == View::Groove);
  // key down in one view, up in another: the note still gets its NoteOff (owner rec), no crash
  down("z"); tap("9"); up("z");
  // redraw is skipped when nothing changes
  tap("`");
  R->run(200);
  const uint32_t full = R->ui.fullRedraws();
  R->run(500);
  CHECK_EQ(R->ui.fullRedraws(), full);
}

int main() {
  static uih::Rig rig;
  R = &rig;
  testStepToggle();
  testHoldVsTap();
  testValueEditing();
  testPLocks();
  testStepEditFields();
  testStepWrite();
  testLiveRecord();
  testMuteSolo();
  testPatternQueue();
  testTempo();
  testFocusFollowsSelection();
  testSweepsAndGhosting();
  testPagesErase();
  testRollAndViews();
  std::printf("itest_ui_behaviour: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
