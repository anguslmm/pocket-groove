// UI phase 2: clipboard, undo/redo of every destructive action, SONG chain
// editing and playback, HELP overlay and explain mode.
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
static void ctrl(const char* n, bool shift = false) {
  down("ctl");
  if (shift) down("shift");
  tap(n);
  if (shift) up("shift");
  up("ctl");
  R->run(30);
}
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static Pattern& PAT(int i) { return R->proj->patterns[i]; }
static bool samePat(const Pattern& a, const Pattern& b) { return std::memcmp(&a, &b, sizeof(Pattern)) == 0; }
static bool sameTrack(const TrackPattern& a, const TrackPattern& b) { return std::memcmp(&a, &b, sizeof(TrackPattern)) == 0; }

static void undoRedo(const Pattern& before, int pat, const char* what) {
  static Pattern after;
  after = PAT(pat);
  CHECK(!samePat(after, before));
  ctrl("z");
  CHECK(samePat(PAT(pat), before));
  if (!samePat(PAT(pat), before)) std::printf("  undo of %s failed\n", what);
  CHECK(toastHas("UNDO: "));
  ctrl("z");                                   // redo
  CHECK(samePat(PAT(pat), after));
  CHECK(toastHas("REDO: "));
  ctrl("z");
  CHECK(samePat(PAT(pat), before));
}

static void testClipboardUndo() {
  static Pattern before;
  R->start(true);
  CHECK(!R->ui.undoAvailable());
  ctrl("z");
  CHECK(toastHas("NOTHING TO UNDO"));
  // cut the bass track
  before = PAT(0);
  ctrl("x");
  CHECK(toastHas("BASS CLEARED  CTRL+Z UNDO"));
  for (int i = 0; i < 16; ++i) CHECK(!(R->step(6, i).flags() & kStepOn));
  CHECK_EQ(R->ui.clipKind(), 1);
  undoRedo(before, 0, "cut track");
  // copy kick, paste on snare (drum -> drum), undo
  down("shift"); tap("a"); up("shift");
  ctrl("c");
  CHECK(toastHas("KICK COPIED"));
  down("shift"); tap("s"); up("shift");
  before = PAT(0);
  ctrl("v");
  CHECK(sameTrack(PAT(0).tracks[1], PAT(0).tracks[0]));
  undoRedo(before, 0, "paste track");
  // drums on a synth: refused
  down("shift"); tap("j"); up("shift");
  before = PAT(0);
  ctrl("v");
  CHECK(toastHas("CAN'T PASTE DRUMS ON BASS"));
  CHECK(samePat(PAT(0), before));
  // lead -> bass: chords dropped, locks on sound params dropped, length follows the clip
  down("shift"); tap("k"); up("shift");
  down("ctl"); down("shift"); tap("8"); up("shift"); up("ctl");   // lead length 8
  ctrl("c");
  down("shift"); tap("j"); up("shift");
  ctrl("v");
  CHECK_EQ(PAT(0).tracks[6].length, 8);
  CHECK(PAT(0).tracks[6].steps[2].flags() & kStepOn);
  CHECK_EQ(PAT(0).tracks[6].steps[2].chord(), 0);
  // pattern copy / cut / paste (CTRL+SHIFT)
  before = PAT(0);
  ctrl("c", true);
  CHECK(toastHas("P01 COPIED"));
  CHECK_EQ(R->ui.clipKind(), 2);
  ctrl("x", true);
  CHECK(toastHas("P01 CLEARED"));
  CHECK_EQ(PAT(0).tracks[0].length, 16);
  int on = 0;
  for (int t = 0; t < 8; ++t) on += PAT(0).tracks[t].steps[0].flags() & kStepOn;
  CHECK_EQ(on, 0);
  ctrl("v");
  CHECK(samePat(PAT(0), before));
  // length change, add page, erase, copy-to-pattern: each undoable
  before = PAT(0);
  down("shift"); tap("8"); up("shift");
  CHECK(toastHas("LENGTH 8  CTRL+Z UNDO"));
  undoRedo(before, 0, "length");
  ctrl("z");   // back to the redo state (length 8) ...
  ctrl("z");   // ... and undone again: length 16
  before = PAT(0);
  down("shift"); tap("'"); up("shift");
  CHECK_EQ(PAT(0).length, 32);
  undoRedo(before, 0, "add page");
  before = PAT(0);
  tap("spc");
  down("del");
  R->run(2300);
  up("del");
  CHECK(toastHas("ERASED  CTRL+Z UNDO"));
  tap("spc");
  R->run(50);
  undoRedo(before, 0, "erase");
  static Pattern dest;
  dest = PAT(6);
  down("opt"); down("shift"); tap("7"); up("shift"); up("opt");
  R->run(50);
  CHECK(samePat(PAT(6), PAT(R->ui.editPattern())));
  undoRedo(dest, 6, "copy to pattern");
}

static void testUndoAfterPatternSwitch() {
  static Pattern before;
  R->start(true);
  before = PAT(0);
  ctrl("x");                                   // cut bass on P01
  down("opt"); tap("2"); up("opt");            // stopped: switch to P02 at once
  R->run(50);
  CHECK_EQ(R->ui.editPattern(), 1);
  ctrl("z");                                   // restores P01 although P02 is playing
  CHECK(samePat(PAT(0), before));
  CHECK_EQ(R->ui.editPattern(), 1);
  // a write in flight is shown at once (the step reads see the buffer)
  R->engineRuns = false;
  down("opt"); tap("1"); up("opt");
  R->engineRuns = true;
  R->run(30);
  R->engineRuns = false;
  ctrl("x");
  ctrl("z");
  CHECK(R->ui.writeInFlight());
  CHECK(R->ui.stepAt(6, 0).flags() & kStepOn);   // shown from the buffer before the engine applied it
  ctrl("z");                                   // still in flight: queued, not refused
  CHECK(!toastHas("BUSY"));
  CHECK(toastHas("UNDO: "));
  R->engineRuns = true;
  R->run(100);
  CHECK(!R->ui.writeInFlight());
  CHECK(toastHas("REDO: BASS CLEARED"));       // ran once the first write landed
  // two presses while in flight cancel out
  R->engineRuns = false;
  ctrl("z");
  CHECK(toastHas("UNDO: BASS CLEARED"));
  ctrl("z");
  ctrl("z");
  R->engineRuns = true;
  R->run(100);
  CHECK(!R->ui.writeInFlight());
  CHECK(toastHas("UNDO: BASS CLEARED"));
}

// Step edits (setStep) are undoable; one gesture = one unit (ui_edit.cpp).
static void testStepUndo() {
  static Pattern before, mid;
  R->start(true);
  down("shift"); tap("a"); up("shift");        // KICK (on at 1, 5, 9, 13)
  // place a note
  before = PAT(0);
  tap("2");
  CHECK(R->step(0, 1).flags() & kStepOn);
  ctrl("z");
  CHECK(toastHas("UNDO: KICK STEP 2"));
  CHECK(!(R->step(0, 1).flags() & kStepOn));
  CHECK(samePat(PAT(0), before));
  ctrl("z");
  CHECK(toastHas("REDO: KICK STEP 2"));
  CHECK(R->step(0, 1).flags() & kStepOn);
  ctrl("z");
  CHECK(samePat(PAT(0), before));
  tap("2");
  undoRedo(before, 0, "place note");
  // delete a note: back with all its fields (accent, velocity 110)
  before = PAT(0);
  tap("1");
  CHECK(!(R->step(0, 0).flags() & kStepOn));
  undoRedo(before, 0, "delete note");
  CHECK(R->step(0, 0).flags() & kStepAccent);
  CHECK_EQ(R->step(0, 0).vel, 110);
  // two taps are two units: undo takes back only the second
  before = PAT(0);
  tap("3");
  mid = PAT(0);
  tap("4");
  ctrl("z");
  CHECK(samePat(PAT(0), mid));
  CHECK(toastHas("UNDO: KICK STEP 4"));
  ctrl("z"); ctrl("z");                        // redo, undo: step 4 off again
  tap("3");                                    // remove step 3 again (its own unit)
  CHECK(samePat(PAT(0), before));
  // held step: many nudges, a held-key repeat, a copy = one unit
  down("shift"); tap("j"); up("shift");        // BASS: step 14 has a CUT lock
  before = PAT(0);
  down("y"); R->run(300);
  for (int i = 0; i < 20; ++i) tap("=");       // velocity +20
  down("="); R->run(1000); up("=");            // and a repeat
  tap(";");                                    // note +1
  tap("8");                                    // copy to step 8
  up("y");
  CHECK(R->step(6, 13).vel != before.tracks[6].steps[13].vel);
  CHECK_EQ(R->step(6, 13).note, before.tracks[6].steps[13].note + 1);
  CHECK(std::memcmp(&R->step(6, 7), &R->step(6, 13), sizeof(Step)) == 0);
  undoRedo(before, 0, "held step edits");
  CHECK(toastHas("UNDO: BASS STEP 14"));
  // copying an existing held step (no write at key-down): labelled as a copy
  before = PAT(0);
  down("y"); R->run(300);
  tap("2");
  up("y");
  ctrl("z");
  CHECK(toastHas("UNDO: BASS COPY STEP 14"));
  CHECK(samePat(PAT(0), before));
  // DEL on a held step (remove its lock), then the next hold is a new unit
  down("y"); R->run(300); tap("del"); up("y");
  CHECK_EQ(R->step(6, 13).lockParam(), 0);
  mid = PAT(0);
  down("y"); R->run(300); tap("="); up("y");
  ctrl("z");
  CHECK(samePat(PAT(0), mid));
  ctrl("z"); ctrl("z");                        // redo, undo
  CHECK(samePat(PAT(0), mid));
  // step-write: a note and the slide it sets on the previous step = one unit
  tap("ent");
  CHECK(R->ui.stepWriteActive());
  tap("z");
  down("c"); R->run(50);
  before = PAT(0);                             // the c note is written
  down("v"); R->run(50); up("c"); up("v");
  CHECK(R->step(6, 1).flags() & kStepSlide);
  undoRedo(before, 0, "step-write note + slide");   // takes back the v note AND the slide on c
  CHECK(!(R->step(6, 1).flags() & kStepSlide));
  CHECK(R->step(6, 1).flags() & kStepOn);
  tap("ent");
  CHECK(!R->ui.recOn());
}

// Undo straight after edits the engine has not applied yet.
static void testUndoPending() {
  static Pattern before, mid;
  R->start(true);
  down("shift"); tap("a"); up("shift");
  before = PAT(0);
  R->engineRuns = false;                       // the engine stalls: edits stay pending
  tap("3");
  tap("4");
  CHECK(R->ui.stepAt(0, 2).flags() & kStepOn);
  CHECK(R->ui.stepAt(0, 3).flags() & kStepOn);
  CHECK(!(PAT(0).tracks[0].steps[2].flags() & kStepOn));   // not in the project yet
  ctrl("z");                                   // undo the second; the first stays
  CHECK(toastHas("UNDO: KICK STEP 4"));
  CHECK(R->ui.stepAt(0, 2).flags() & kStepOn);
  CHECK(!(R->ui.stepAt(0, 3).flags() & kStepOn));
  R->engineRuns = true;
  R->run(100);
  CHECK(PAT(0).tracks[0].steps[2].flags() & kStepOn);
  CHECK(!(PAT(0).tracks[0].steps[3].flags() & kStepOn));
  // undo at once after a quick edit, engine running: never BUSY
  mid = PAT(0);
  tap("6");
  ctrl("z");
  CHECK(!toastHas("BUSY"));
  CHECK(samePat(PAT(0), mid));
  // undo + redo faster than a frame: the redo waits for the first write
  tap("6");
  R->down(K("ctl"));
  R->tap(K("z"), 1);
  R->tap(K("z"), 1);
  R->up(K("ctl"));
  CHECK(R->ui.writeInFlight());
  R->run(100);
  CHECK(toastHas("REDO: KICK STEP 6"));
  CHECK(PAT(0).tracks[0].steps[5].flags() & kStepOn);
  (void)before;
}

// A live recording pass is one unit, however many notes.
static void testRecordUndo() {
  static Pattern before, pass1;
  R->start(false);
  tap("spc");
  before = PAT(0);
  tap("ent");
  CHECK(R->eng.status().recording);
  for (int i = 0; i < 6; ++i) {                // clap
    tap("d", 30);
    R->run(170);
  }
  tap("a", 30);                                // and a kick
  R->run(100);
  int hits = 0;
  for (int i = 0; i < 16; ++i) hits += (PAT(0).tracks[kTrackClap].steps[i].flags() & kStepOn) ? 1 : 0;
  CHECK(hits >= 4);
  tap("ent");                                  // REC off: the pass ends
  R->run(50);
  undoRedo(before, 0, "recording pass");
  CHECK(toastHas("UNDO: RECORDED NOTES"));
  // a second pass is a new unit: undo keeps the first
  ctrl("z");                                   // redo the first pass
  pass1 = PAT(0);
  tap("ent");
  down("shift"); tap("j"); up("shift");        // bass, a held note (gate from the hold)
  tap("z", 200);
  R->run(100);
  tap("ent");
  R->run(50);
  CHECK(!samePat(PAT(0), pass1));
  ctrl("z");
  CHECK(samePat(PAT(0), pass1));
  tap("spc");
}

static void testSong() {
  R->start(true);
  tap("p");
  CHECK(R->ui.view() == View::Song);
  CHECK_EQ(R->ui.songLength(), 0);
  tap("ent");
  CHECK(toastHas("ADD A PATTERN FIRST"));
  CHECK(!R->proj->songMode);
  tap("1");                                    // append P01
  tap("2");                                    // append P02
  tap("3");
  CHECK_EQ(R->ui.songLength(), 3);
  CHECK_EQ(R->proj->song[1].pattern, 1);
  CHECK_EQ(R->proj->song[1].repeats, 1);
  CHECK_EQ(R->proj->song[3].repeats, 0);
  tap(";");                                    // cursor on slot 3 (P03)
  for (int i = 0; i < 70; ++i) tap("=");
  CHECK_EQ(R->proj->song[2].repeats, 64);     // 1..64
  down("shift"); tap("-"); up("shift");
  CHECK_EQ(R->proj->song[2].repeats, 56);
  for (int i = 0; i < 70; ++i) tap("-");
  CHECK_EQ(R->proj->song[2].repeats, 1);
  down("shift"); tap("5"); up("shift");        // insert P05 before slot 3
  CHECK_EQ(R->ui.songLength(), 4);
  CHECK_EQ(R->proj->song[2].pattern, 4);
  CHECK_EQ(R->proj->song[3].pattern, 2);
  tap("q");                                    // overwrite slot 3 with P09
  CHECK_EQ(R->proj->song[2].pattern, 8);
  tap("del");                                  // remove it
  CHECK_EQ(R->ui.songLength(), 3);
  CHECK_EQ(R->proj->song[2].pattern, 2);
  // chain P01 x1, P02 x1, P03 x1 -> playback follows it from the cursor slot
  tap(";"); tap(";");                          // slot 1
  tap("ent");
  R->run(50);
  CHECK(R->proj->songMode);
  tap("spc");
  R->run(100);
  CHECK_EQ(R->eng.status().pattern, 0);
  R->run(1935);                                // one bar at 124 BPM ~ 1935 ms
  R->run(200);
  CHECK_EQ(R->eng.status().pattern, 1);
  CHECK_EQ(R->eng.status().songSlot, 1);
  R->run(1935 * 2);
  CHECK_EQ(R->eng.status().pattern, 0);        // looped back to slot 1
  tap("ent");
  CHECK(!R->proj->songMode);
  tap("spc");
  // fill to 32: full
  for (int i = 0; i < 31; ++i) { while (R->ui.songCursor() < R->ui.songLength()) tap("."); tap("1"); }
  CHECK_EQ(R->ui.songLength(), 32);
  tap("2");
  CHECK(toastHas("CHAIN IS FULL"));
}

static void testHelp() {
  R->start(true);
  const char* views[] = {"`", "9", "0", "o", "p", "\\"};
  for (const char* v : views) {
    tap(v);
    down("/");
    R->run(450);
    CHECK(R->ui.helpVisible());
    down("fn");                                // a held modifier shows its layer
    R->run(50);
    CHECK(R->ui.helpVisible());
    up("fn");
    up("/");
    CHECK(!R->ui.helpVisible());
  }
  tap("`");
  // explain mode: keys describe themselves, nothing happens; SPACE still plays
  tap("/");
  CHECK(R->ui.explainMode());
  CHECK(R->ui.helpVisible());
  const uint32_t rev = R->eng.status().revision;
  tap("1");
  tap("=");
  tap("9");
  R->run(50);
  CHECK(R->ui.view() == View::Groove);
  CHECK_EQ(R->eng.status().revision, rev);
  tap("spc");
  R->run(50);
  CHECK(R->eng.status().playing);
  tap("/");
  CHECK(!R->ui.explainMode());
  tap("/");
  tap("`");                                    // ` also leaves
  CHECK(!R->ui.explainMode());
  // a key pressed with a modifier held explains the combo, not the bare key
  // (owner report: FN + a key showed the key's text without FN)
  struct Combo { const char* mod; const char* key; const char* title; };
  const Combo combos[] = {
      {"fn", "a", "FN+PAD: MUTE KICK"},      {"fn", "z", "FN+NOTE: SOLO KICK"},
      {"fn", "3", "NOTHING WITH FN"},        {"fn", "9", "SOUND VIEW"},
      {"opt", "3", "OPT+STEP: QUEUE P03"},   {"alt", "s", "ALT+PAD: ROLL SNAR"},
      {"alt", ";", "ALT+UP: ROLL FASTER"},   {"ctl", "z", "CTRL+Z: UNDO"},
      {"ctl", "s", "CTRL+S: SAVE"},          {"tab", "=", "TAB+=: TEMPO +1 BPM"},
      {"tab", "a", "PAD A: KICK"},           {"sft", "3", "SHIFT+STEP: LENGTH 3"},
      {"sft", "x", "SHIFT+NOTE 2: ACCENT"},  {"sft", "a", "PAD A: KICK"},
  };
  char title[40];
  const char *l1, *l2;
  tap("/");
  for (const Combo& c : combos) {
    down(c.mod);
    tap(c.key);
    CHECK_EQ(R->ui.explainKey(), K(c.key));
    R->ui.explainText(title, &l1, &l2);
    if (std::strcmp(title, c.title) != 0) std::printf("  %s+%s explains \"%s\"\n", c.mod, c.key, title);
    CHECK(std::strcmp(title, c.title) == 0);
    up(c.mod);
    R->ui.explainText(title, &l1, &l2);         // still the gesture that was pressed
    CHECK(std::strcmp(title, c.title) == 0);
  }
  tap("/");
  // SHIFT+step inserts in SONG: the key map says so there, LENGTH elsewhere
  char lab[3];
  tap("p");
  down("sft");
  R->ui.helpKeyLabel(K("1"), lab);
  CHECK(std::strcmp(lab, "IN") == 0);
  R->ui.helpKeyLabel(K("'"), lab);           // SHIFT+' adds no page in SONG
  CHECK(lab[0] == 0);
  up("sft");
  tap("/");
  down("sft");
  tap("3");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strcmp(title, "SHIFT+STEP: INSERT P03") == 0);
  up("sft");
  tap("/");
  tap("`");
  down("sft");
  R->ui.helpKeyLabel(K("1"), lab);
  CHECK(std::strcmp(lab, "LN") == 0);
  up("sft");
  tap("/");
  tap("a");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strcmp(title, "PAD A: KICK") == 0);
  CHECK(!R->ui.muted(0));                      // explain mode never acts
  tap("/");
  // every key in every table explains without crashing
  for (const char* v : views) {
    tap(v);
    tap("/");
    for (int k = 0; k < kNumKeyIds; ++k) {
      if (k == kKSlash || k == kKBacktick || k == kKSpace) continue;
      R->tap(k, 5);
      R->run(5);
    }
    CHECK(R->ui.explainMode());
    tap("/");
  }
}

int main() {
  static uih::Rig rig;
  R = &rig;
  for (uih::GhostOrder order : {uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
  rig.matrix.order = order;
  std::printf("-- key matrix model: %s\n", uih::ghostOrderName(order));
  testClipboardUndo();
  testUndoAfterPatternSwitch();
  testStepUndo();
  testUndoPending();
  testRecordUndo();
  testSong();
  testHelp();
  }
  std::printf("itest_ui_edit: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
