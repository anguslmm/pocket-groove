// The SOUND view as the instrument page (UX.md 10.3), driven by key events
// through the real UI, engine and voices, once without the phantom-key filter
// (the desktop) and twice through the diode-less matrix model (the device,
// phantom last / first in its batch):
//   new project   every track starts with its first instrument, clean (no '*')
//   macro keys    z/x c/v b/n m/, = macro 1..4 down/up: tap, hold (accelerating
//                 sweep), SHIFT coarse, both keys of a pair = default, two
//                 macros at once with two fingers (also with SHIFT)
//   -/=           the focused macro, also from GROOVE; the focus chip
//   instruments   ; . 9 step through the list (audible: the voices' values
//                 change), the list overlay, positions remembered per
//                 instrument, undo / redo of a run of changes
//   CLASSIC       the page, the invitation, picking one, undo back to the
//                 exact classic values; developer mode shows the parameters
//   note values   hold a step + macro key / -/= = that note's own value of the
//                 macro, one per macro (both keys: the default), in whole levels
//                 of 63; DEL removes the focused macro's, then all; the STEP EDIT
//                 MOTION field (its pad again: the next macro)
//   motion        while playing the sliders follow the engine (the base stays);
//                 a move of a macro with motion only moves its base; record on +
//                 a held macro key = live (written into the notes passed, one
//                 undo unit); hold DEL + a macro key clears its motion (undo)
//   sweep         SHIFT+[ ] moves the bass's macro 1 (CLASSIC: the cutoff)
//   help          key labels and explain texts of the page and its SHIFT layer
#include <cstdio>
#include <cstring>

#include "common/ui_harness.h"
#include "pg/engine/engine_hooks.h"

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
static uih::GhostOrder g_order = uih::GhostOrder::Off;
static int K(const char* n) { return keyFromName(n); }
static void tap(const char* n, uint32_t ms = 40) { R->tap(K(n), ms); R->run(20); }
static void down(const char* n) { R->down(K(n)); }
static void up(const char* n) { R->up(K(n)); R->run(20); }
static void ctrl(const char* n) { down("ctl"); tap(n); up("ctl"); R->run(30); }
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static const TrackSettings& TS(int t) { return R->proj->tracks[t]; }
static int MAC(int t, int m) { return TS(t).macro[m]; }

static void start(bool mockup) {
  R->devMode = false;
  R->matrix.order = g_order;
  R->start(mockup);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
}

// A new project (CTRL+N) with the bass selected and SOUND open.
static void newProjectOnBass() {
  start(false);
  ctrl("n");
  R->run(100);
  down("shift"); tap("j"); up("shift");     // select the bass silently
  tap("9");
  R->run(50);
}

static const macro::MacroSet* first(int t) { return macro::instrumentAt(t, 0); }

static void testNewProject() {
  start(false);
  ctrl("n");
  R->run(200);
  CHECK(toastHas("NEW PROJECT"));
  for (int t = 0; t < kNumTracks; ++t) {
    const macro::MacroSet* s = first(t);
    CHECK(s && instrumentId(TS(t)) == s->id);
    for (int m = 0; s && m < s->numMacros; ++m) CHECK_EQ(MAC(t, m), macro::startByte(s->macros[m]));
  }
  CHECK(!R->ui.dirty());                    // derived values are part of the clean state
  CHECK(R->ui.focus().index == kMacroIndex0);   // kick selected: its macro 1 on -/=
  tap("9");
  CHECK(R->ui.view() == View::Sound && R->ui.instrumentPage());
  CHECK(R->ui.instrumentOf(0) == first(0));
}

static void testMacroKeys() {
  newProjectOnBass();
  const macro::MacroSet* acid = first(kTrackBass);
  CHECK(acid && std::strcmp(acid->name, "ACID") == 0);
  CHECK_EQ(R->ui.selTrack(), kTrackBass);
  const int p0 = MAC(kTrackBass, 0), p1 = MAC(kTrackBass, 1);
  tap("x");
  CHECK_EQ(MAC(kTrackBass, 0), p0 + 2);
  CHECK(R->ui.focus().index == kMacroIndex0);
  tap("z"); tap("z");
  CHECK_EQ(MAC(kTrackBass, 0), p0 - 2);
  // the voice gets it: the bass's effective cutoff follows OPEN
  uint8_t effA[32], effB[32];
  engine_detail::effectiveParams(kTrackBass, effA);
  down("shift"); tap("x"); up("shift");     // coarse
  CHECK_EQ(MAC(kTrackBass, 0), p0 - 2 + 16);
  R->run(20);
  engine_detail::effectiveParams(kTrackBass, effB);
  CHECK(std::memcmp(effA, effB, kSoundParams) != 0);
  CHECK_EQ(effB[0], TS(kTrackBass).params[0]);
  // hold: a sweep that accelerates (300 ms, then 2 / 4 / 8 per 40 ms)
  const int before = MAC(kTrackBass, 1);
  down("v");
  R->run(1500);
  up("v");
  const int moved = MAC(kTrackBass, 1) - before;
  CHECK(moved > 60 && moved < 130);
  CHECK(R->ui.focus().index == kMacroIndex0 + 1);
  // both keys of a pair: the default
  down("c"); down("v"); up("v"); up("c");
  CHECK_EQ(MAC(kTrackBass, 1), macro::startByte(acid->macros[1]));
  CHECK(toastHas("DEFAULT"));
  (void)p1;
  // two macros at once, two fingers, opposite directions: both sweep
  const int a0 = MAC(kTrackBass, 0), a1 = MAC(kTrackBass, 1);
  down("x"); down("c");
  R->run(1000);
  up("c"); up("x");
  CHECK(MAC(kTrackBass, 0) > a0 + 30);
  CHECK(a1 == 0 || MAC(kTrackBass, 1) < a1);
  // ... and with SHIFT held as well (three keys: no phantom rectangle)
  const int b0 = MAC(kTrackBass, 0), b2 = MAC(kTrackBass, 2);
  down("shift"); down("z"); down("n"); R->run(400); up("n"); up("z"); up("shift");
  CHECK(MAC(kTrackBass, 0) <= b0 - 32);
  CHECK(MAC(kTrackBass, 2) >= (b2 + 32 > 255 ? 255 : b2 + 32));
  // limits
  for (int i = 0; i < 20; ++i) { down("shift"); tap("x", 10); up("shift"); }
  CHECK_EQ(MAC(kTrackBass, 0), 255);
  // a macro the set does not have: ACID has 4, SURGE 2
  R->ui.setInstrument(kTrackBass, macro::instrumentAt(kTrackBass, 2)->id);
  R->run(50);
  const int m3 = MAC(kTrackBass, 3);
  tap("m");
  CHECK_EQ(MAC(kTrackBass, 3), m3);
}

static void testValueKeys() {
  newProjectOnBass();
  tap("c");                                  // focus macro 2 (BITE)
  const int v = MAC(kTrackBass, 1);
  tap("`");                                  // GROOVE: -/= still move it
  CHECK(R->ui.view() == View::Groove);
  tap("=");
  CHECK_EQ(MAC(kTrackBass, 1), v - 2 + 2 + 2);
  CHECK(R->ui.popupVisible());
  down("shift"); tap("-"); up("shift");
  CHECK_EQ(MAC(kTrackBass, 1), v + 2 - 16 < 0 ? 0 : v + 2 - 16);
  down("-"); down("="); up("="); up("-");    // both: the default
  CHECK_EQ(MAC(kTrackBass, 1), macro::startByte(first(kTrackBass)->macros[1]));
  // selecting the track again puts its macro back on -/=
  tap("a");
  CHECK(R->ui.focus().track == 0 && R->ui.focus().index == kMacroIndex0);
  tap("j");
  CHECK(R->ui.focus().track == kTrackBass && R->ui.focus().index == kMacroIndex0 + 1);
}

static void testInstrumentSteps() {
  newProjectOnBass();
  const int n = macro::instrumentCount(kTrackBass);
  CHECK(n >= 2);
  tap("x"); tap("x");                         // a tweak to remember
  const int tweaked = MAC(kTrackBass, 0);
  uint8_t effA[32], effB[32];
  engine_detail::effectiveParams(kTrackBass, effA);
  tap(".");
  const macro::MacroSet* s1 = macro::instrumentAt(kTrackBass, 1);
  CHECK(instrumentId(TS(kTrackBass)) == s1->id);
  CHECK(R->ui.listVisible());
  for (int m = 0; m < s1->numMacros; ++m) CHECK_EQ(MAC(kTrackBass, m), macro::startByte(s1->macros[m]));
  R->run(20);
  engine_detail::effectiveParams(kTrackBass, effB);
  CHECK(std::memcmp(effA, effB, kSoundParams) != 0);    // audible at once
  tap(";");                                   // back: the tweak is remembered
  CHECK(instrumentId(TS(kTrackBass)) == first(kTrackBass)->id);
  CHECK_EQ(MAC(kTrackBass, 0), tweaked);
  tap(";");                                   // wraps to the last
  CHECK(instrumentId(TS(kTrackBass)) == macro::instrumentAt(kTrackBass, n - 1)->id);
  tap("9");                                   // 9 again = next: wraps to the first
  CHECK(instrumentId(TS(kTrackBass)) == first(kTrackBass)->id);
  R->run(1700);
  CHECK(!R->ui.listVisible());
  // undo: a run of steps is one unit
  const uint8_t posBefore = TS(kTrackBass).macro[0];
  tap("."); tap("."); tap(".");
  const uint16_t after = instrumentId(TS(kTrackBass));
  CHECK(after == macro::instrumentAt(kTrackBass, 3)->id);
  ctrl("z");
  R->run(50);
  CHECK(instrumentId(TS(kTrackBass)) == first(kTrackBass)->id);
  CHECK_EQ(TS(kTrackBass).macro[0], posBefore);
  CHECK(toastHas("UNDO: BASS ACID"));
  ctrl("z");
  R->run(50);
  CHECK(instrumentId(TS(kTrackBass)) == after);
  CHECK(toastHas("REDO: "));
  // a step edit in between makes the pattern the undo unit again
  ctrl("z");
  R->run(50);
  tap("1");
  ctrl("z");
  R->run(50);
  CHECK(instrumentId(TS(kTrackBass)) == first(kTrackBass)->id);   // the instrument stayed
}

static void testClassic() {
  start(true);                                // the mockup project: CLASSIC everywhere
  down("shift"); tap("j"); up("shift");
  tap("9");
  CHECK(R->ui.instrumentPage() && R->ui.instrumentOf(kTrackBass) == nullptr);
  uint8_t classic[kParamsPerTrack];
  std::memcpy(classic, TS(kTrackBass).params, sizeof(classic));
  tap("x");
  CHECK(toastHas("PICK AN INSTRUMENT"));
  CHECK(std::memcmp(classic, TS(kTrackBass).params, sizeof(classic)) == 0);
  tap(".");
  R->run(50);
  CHECK(instrumentId(TS(kTrackBass)) == first(kTrackBass)->id);
  CHECK(std::memcmp(classic, TS(kTrackBass).params, kSoundParams) != 0);
  CHECK(std::memcmp(classic + kSoundParams, TS(kTrackBass).params + kSoundParams, 8) == 0);   // mixer kept
  R->run(1700);
  ctrl("z");
  R->run(100);
  CHECK(instrumentId(TS(kTrackBass)) == 0);
  CHECK(std::memcmp(classic, TS(kTrackBass).params, sizeof(classic)) == 0);   // exactly the classic sound
  CHECK(toastHas("UNDO: BASS CLASSIC"));
  // CLASSIC: ; picks the last
  tap(";");
  CHECK(instrumentId(TS(kTrackBass)) == macro::instrumentAt(kTrackBass, macro::instrumentCount(kTrackBass) - 1)->id);
  // developer mode: CLASSIC tracks show the parameter pages, `l` flips
  R->devMode = true;
  R->matrix.order = g_order;
  R->start(true);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
  down("shift"); tap("j"); up("shift");
  tap("9");
  CHECK(!R->ui.instrumentPage());
  tap("l");
  CHECK(R->ui.instrumentPage());
  tap("l");
  CHECK(!R->ui.instrumentPage());
}

static int code(int pos) { return macroPosToCode(pos); }
static int lvl(int c) { return macroCodeToPos(c); }

static void testLocks() {
  newProjectOnBass();
  const macro::MacroSet* acid = first(kTrackBass);
  const int p0 = MAC(kTrackBass, 0), p1 = MAC(kTrackBass, 1);
  // hold step 1 + macro key = that note's own value of the macro, one level up
  down("1");
  R->run(300);
  tap("x");
  CHECK(R->step(kTrackBass, 0).hasMacro(0));
  CHECK_EQ(R->step(kTrackBass, 0).macroValue(0), lvl(code(p0) + 1));
  CHECK_EQ(MAC(kTrackBass, 0), p0);              // the macro itself did not move
  down("shift"); tap("x"); up("shift");
  CHECK_EQ(R->step(kTrackBass, 0).macroValue(0), lvl(code(p0) + 5));
  tap("v");                                       // a second macro: its own value too, the first stays
  CHECK(R->step(kTrackBass, 0).hasMacro(1) && R->step(kTrackBass, 0).hasMacro(0));
  CHECK_EQ(R->step(kTrackBass, 0).macroValue(1), lvl(code(p1) + 1));
  CHECK_EQ(R->step(kTrackBass, 0).macroValue(0), lvl(code(p0) + 5));
  down("c"); down("v"); up("v"); up("c");        // both keys: the default on that step
  CHECK_EQ(R->step(kTrackBass, 0).macroValue(1), lvl(code(macro::startByte(acid->macros[1]))));
  tap("=");                                       // -/= edit the focused macro's value
  CHECK_EQ(R->step(kTrackBass, 0).macroValue(1), lvl(code(macro::startByte(acid->macros[1])) + 1));
  R->frame();                                     // drawn: both sliders in the held-step language
  tap("del");                                     // DEL: the focused macro's value first ...
  CHECK(!R->step(kTrackBass, 0).hasMacro(1) && R->step(kTrackBass, 0).hasMacro(0));
  tap("del");                                     // ... then the rest
  CHECK(!R->step(kTrackBass, 0).hasExt());
  tap("x");
  up("1");
  CHECK(R->step(kTrackBass, 0).on());             // the hold turned the step on, an edit keeps it
  CHECK(R->step(kTrackBass, 0).hasMacro(0));
  // the STEP EDIT MOTION field in GROOVE edits the focused macro's value
  tap("`");
  down("2");
  R->run(300);
  tap("k");                                       // MOTION field
  tap("=");
  CHECK(R->step(kTrackBass, 1).hasMacro(0) && !R->step(kTrackBass, 1).hasMacro(1));   // focus: macro 1 (x)
  CHECK_EQ(R->step(kTrackBass, 1).macroValue(0), lvl(code(MAC(kTrackBass, 0)) + 1));
  tap("k");                                       // the field's pad again: the next macro
  CHECK_EQ(R->ui.trackFocus(kTrackBass), kMacroIndex0 + 1);
  tap("=");
  CHECK(R->step(kTrackBass, 1).hasMacro(1) && R->step(kTrackBass, 1).hasMacro(0));
  CHECK_EQ(R->step(kTrackBass, 1).macroValue(1), lvl(code(MAC(kTrackBass, 1)) + 1));
  tap("del");                                     // the focused one (macro 2)
  CHECK(!R->step(kTrackBass, 1).hasMacro(1) && R->step(kTrackBass, 1).hasMacro(0));
  for (int i = 0; i < 4; ++i) tap("k");           // round the four and back to macro 2
  CHECK_EQ(R->ui.trackFocus(kTrackBass), kMacroIndex0 + 1);
  for (int i = 0; i < 6; ++i) { down("shift"); tap("="); up("shift"); }   // far enough to be heard
  up("2");
  // played: the lock reaches the voice at its step
  tap("space");
  CHECK(R->runUntilStep(kTrackBass, 1));
  R->run(5);
  uint8_t eff[32];
  engine_detail::effectiveParams(kTrackBass, eff);
  CHECK(std::memcmp(eff, TS(kTrackBass).params, kSoundParams) != 0);
  tap("space");
}

// Motion on the page: the sliders follow the engine while the pattern plays (the
// base stays), the clear gesture (hold DEL + a macro key), a recording pass of a
// moved macro (live: the chip in the record colour, written into the notes, one
// undo unit).
static void testMotion() {
  newProjectOnBass();
  const int p0 = MAC(kTrackBass, 0), p1 = MAC(kTrackBass, 1);
  // two notes with their own OPEN, set by hand (hold the step, tap the macro key)
  down("1"); R->run(300);
  for (int i = 0; i < 20; ++i) tap("z");
  up("1");
  down("5"); R->run(300);
  for (int i = 0; i < 20; ++i) tap("x");
  up("5");
  const int lo = R->step(kTrackBass, 0).macroValue(0), hi = R->step(kTrackBass, 4).macroValue(0);
  CHECK(lo < p0 && hi > p0);
  CHECK_EQ(R->ui.motionMask(kTrackBass), 1);
  CHECK_EQ(R->ui.shownMacro(kTrackBass, 0), p0);  // stopped: the page shows the base
  // playing: the slider follows the engine; the base does not move
  tap("space");
  int seenLo = 999, seenHi = -1, follow = 0, frames = 0;
  for (int f = 0; f < 120; ++f) {
    R->run(33);
    const int v = R->ui.shownMacro(kTrackBass, 0);
    seenLo = v < seenLo ? v : seenLo;
    seenHi = v > seenHi ? v : seenHi;
    ++frames;
    // (the page shows the status of its last frame: within one frame's movement, ~11)
    follow += std::abs(v - int(engine_detail::macroPosition(kTrackBass, 0) + 0.5f)) <= 16;
  }
  CHECK(seenLo <= lo + 2 && seenHi >= hi - 2);
  CHECK(follow >= frames - 2);
  CHECK_EQ(MAC(kTrackBass, 0), p0);
  CHECK_EQ(R->ui.shownMacro(kTrackBass, 1), p1);  // no motion: its position
  // a move of OPEN while the motion owns it: only its base
  tap("x");
  CHECK_EQ(MAC(kTrackBass, 0), p0 + 2);
  // record on: a moved macro is live (heard, written into the notes it passes)
  tap("ent");
  down("v");
  R->run(2500);                                   // more than a bar: every note passes
  CHECK(R->ui.macroLive(kTrackBass, 1));
  R->frame();                                     // drawn: BITE's chip in the record colour
  up("v");
  CHECK(R->step(kTrackBass, 0).hasMacro(1) && R->step(kTrackBass, 4).hasMacro(1));
  CHECK(!R->step(kTrackBass, 2).hasExt());        // no values on empty steps
  CHECK(R->ui.undoAvailable());
  tap("ent");                                     // record off: the pass ends, BITE plays its new motion
  R->run(100);
  CHECK(!R->ui.macroLive(kTrackBass, 1));
  CHECK_EQ(R->ui.motionMask(kTrackBass), 3);
  ctrl("z");                                      // the pass is one undo unit
  R->run(100);
  CHECK(!R->step(kTrackBass, 0).hasMacro(1) && !R->step(kTrackBass, 4).hasMacro(1));
  CHECK(R->step(kTrackBass, 0).hasMacro(0) && R->step(kTrackBass, 4).hasMacro(0));   // the hand-set ones stay
  // clear OPEN's motion: hold DEL, tap one of its keys
  down("del");
  tap("z");
  up("del");
  R->run(50);
  CHECK(toastHas("MOTION CLEARED"));
  CHECK_EQ(R->ui.motionMask(kTrackBass), 0);
  CHECK(!R->step(kTrackBass, 0).hasExt() && !R->step(kTrackBass, 4).hasExt());
  CHECK(R->step(kTrackBass, 0).on());             // the notes stay
  R->run(400);
  CHECK_EQ(R->ui.shownMacro(kTrackBass, 0), MAC(kTrackBass, 0));
  down("del"); tap("z"); up("del");
  CHECK(toastHas("NO MOTION"));
  ctrl("z");                                      // undo brings the values back
  R->run(100);
  CHECK(R->step(kTrackBass, 0).hasMacro(0) && R->step(kTrackBass, 4).hasMacro(0));
  tap("space");
  R->run(100);
  CHECK_EQ(R->ui.shownMacro(kTrackBass, 0), MAC(kTrackBass, 0));   // stopped: the base again
}

// How the page explains motion: the footer while the focused macro has motion,
// one toast when a key move of such a macro is not heard (playing, not
// recording; at most one every few seconds), none when stopped or recording.
static void testMotionHelp() {
  newProjectOnBass();
  CHECK(!R->ui.motionFooter());
  down("1"); R->run(300);                         // a note with its own OPEN
  for (int i = 0; i < 10; ++i) tap("x");
  CHECK(!R->ui.motionFooter());                   // a step held: the held-step footer
  up("1");
  R->run(50);
  CHECK_EQ(R->ui.motionMask(kTrackBass), 1);
  CHECK(R->ui.motionFooter());                    // OPEN is focused and has motion
  tap("c");                                       // BITE (no motion) focused: the usual footer
  CHECK(!R->ui.motionFooter());
  // stopped: a move is heard, no toast
  tap("z");
  CHECK(R->ui.motionFooter());
  CHECK(!toastHas("FOLLOWS NOTES"));
  // playing: the move only sets the base: one toast (-/= as the keys)
  tap("space");
  R->run(200);
  const int base = MAC(kTrackBass, 0);
  tap("=");
  CHECK_EQ(MAC(kTrackBass, 0), base + 2);         // what the move does is unchanged
  CHECK(toastHas("OPEN FOLLOWS NOTES: HOLD STEP+KEY"));
  R->run(2600);                                   // gone ...
  CHECK(R->ui.toastText()[0] == 0);
  tap("x");                                       // ... and not again so soon
  CHECK_EQ(MAC(kTrackBass, 0), base + 4);
  CHECK(R->ui.toastText()[0] == 0);
  tap("v");                                       // BITE has no motion: never
  R->run(6000);
  tap("v");
  CHECK(R->ui.toastText()[0] == 0);
  tap("z");                                       // OPEN's key, a while later: once more
  CHECK(toastHas("OPEN FOLLOWS NOTES"));
  R->run(2600);
  // recording: the move is recorded, no toast
  R->run(6000);
  tap("ent");
  tap("x");
  CHECK(!toastHas("FOLLOWS NOTES"));
  tap("ent");
  R->run(100);
  tap("space");
  R->run(6000);
  // stopped again: no toast
  tap("x");
  CHECK(!toastHas("FOLLOWS NOTES"));
  // the explain text of a key of a macro with motion says so
  tap("/");
  tap("x");
  char title[40];
  const char *l1, *l2;
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(l1, "ITS NOTES HAVE VALUES") != nullptr);
  tap("/");
}

// The label row of every instrument's macros with motion: the marker always has room
// and touches neither the end labels, the name (its chip) nor the tilt mark; where a
// tilt mark fits without the marker it still fits with it.
static void testMotionMarkerLayout() {
  int cells = 0;
  for (int t = 0; t < kNumTracks; ++t)
    for (int i = 0; i < macro::instrumentCount(t); ++i) {
      const macro::MacroSet* s = macro::instrumentAt(t, i);
      if (!s) continue;
      uint8_t tilt[kTrackMacros] = {};
      for (int a = 0; a < 2; ++a)
        if (s->tiltAssign(a).macro >= 0) tilt[s->tiltAssign(a).macro & 3] |= uint8_t(1u << a);
      for (int m = 0; m < s->numMacros; ++m)
        for (int c = 0; c < 2; ++c) {
          const MacroLabels plain = macroLabels(s->macros[m], kMacroCellX[c], false, tilt[m]);
          const MacroLabels L = macroLabels(s->macros[m], kMacroCellX[c], true, tilt[m]);
          const int chipL = L.nameX - L.nameW / 2, chipR = chipL + L.nameW - 1;
          ++cells;
          // at least one blank column between the low label's last glyph column
          // (loEnd - 2) and the marker, the marker and the chip, the chip and the
          // tilt mark, the tilt mark (or the chip) and the high label
          bool ok = L.markX >= L.loEnd && L.markX + 5 + 1 <= chipL && chipR <= L.hiStart - 2;
          if (L.tiltX >= 0) ok = ok && L.tiltX >= chipR + 2 && L.tiltX + (L.tiltAx ? 5 : 7) <= L.hiStart - 1;
          if (!ok) std::printf("  layout: %s macro %s (column %d): mark %d chip %d..%d tilt %d, labels end %d / start %d\n",
                               s->name, s->macros[m].name, c, L.markX, chipL, chipR, L.tiltX, L.loEnd, L.hiStart);
          CHECK(ok);
          if (plain.tiltX >= 0 && L.tiltX < 0)   // (no room for both: the marker wins)
            std::printf("  note: %s %s (column %d): the tilt mark gives way to the motion marker\n", s->name, s->macros[m].name, c);
        }
    }
  CHECK(cells > 100);
}

static void testSweep() {
  newProjectOnBass();
  tap("`");
  const int p0 = MAC(kTrackBass, 0);
  down("shift"); down("]"); R->run(1000);
  CHECK(MAC(kTrackBass, 0) > p0 + 60);
  down("[");                                      // both: back to where the sweep started
  R->run(20);
  CHECK_EQ(MAC(kTrackBass, 0), p0);
  up("["); up("]"); up("shift");
  // CLASSIC bass: the cutoff, as before
  start(true);
  const int cut = TS(kTrackBass).params[0];
  down("shift"); down("]"); R->run(500); up("]"); up("shift");
  CHECK(TS(kTrackBass).params[0] > cut + 10);
}

static void testHelp() {
  newProjectOnBass();
  char lab[3];
  R->ui.helpKeyLabel(K("x"), lab);
  CHECK(std::strcmp(lab, "1+") == 0);
  R->ui.helpKeyLabel(K(","), lab);
  CHECK(std::strcmp(lab, "4+") == 0);
  R->ui.helpKeyLabel(K("9"), lab);
  CHECK(std::strcmp(lab, "NI") == 0);
  tap("/");                                       // explain mode
  CHECK(R->ui.explainMode());
  char title[40];
  const char *l1, *l2;
  tap("x");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strcmp(title, "x: OPEN UP") == 0 || std::strstr(title, "OPEN UP") != nullptr);
  CHECK(std::strstr(l2, "NOTE VALUE") != nullptr);
  tap(".");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(title, "NEXT INSTRUMENT") != nullptr);
  down("shift"); tap("c"); up("shift");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(title, "SHIFT+") != nullptr && std::strstr(title, "-16") != nullptr);
  tap("]");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(l2, "MACRO 1") != nullptr);
  tap("/");
  CHECK(!R->ui.explainMode());
  CHECK(instrumentId(TS(kTrackBass)) == first(kTrackBass)->id);   // explain mode changed nothing
  // the SHIFT layer of the page
  down("shift");
  R->run(50);
  R->ui.helpKeyLabel(K("z"), lab);
  CHECK(std::strcmp(lab, "1-") == 0);
  up("shift");
}

int main() {
  static uih::Rig rig;
  R = &rig;
  for (uih::GhostOrder order : {uih::GhostOrder::Off, uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
    g_order = order;
    std::printf("-- key matrix model: %s%s\n", uih::ghostOrderName(order),
                order == uih::GhostOrder::Off ? " (phantom filter off, as on the desktop)" : "");
    testNewProject();
    testMacroKeys();
    testValueKeys();
    testInstrumentSteps();
    testClassic();
    testLocks();
    testMotion();
    testMotionHelp();
    testSweep();
    testHelp();
  }
  testMotionMarkerLayout();
  std::printf("itest_ui_instrument: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
