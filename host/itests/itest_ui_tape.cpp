// The TAPE view (UX.md 10.11) through the real UI, engine and voices, with all three
// key-matrix models (no ghost filter; the phantom last; the phantom first):
//   view       p in GROOVE: SONG; p again: TAPE; p again: SONG; ` home
//   keys       z x c v arm a track, 1-4 mute it, ; . the field, - = its value, both:
//              the default (the tape's settings follow through the engine's queue)
//   record     L tapped from stop: a count-in bar, then REC from the top; L again: it
//              ends on the next bar line, the toast names it; the track has audio;
//              CTRL+Z undoes it, again redoes it; L held: records while held, its
//              release ends it on the next bar line; SHIFT+L: REPLACE; L with nothing
//              to record from (LINE without an input): its reason
//   tricks     b n m held: BRAKE HALF REVERSE while held, the last one held wins and a
//              release falls back to the one still held; a pad held with a trick: both
//   erase      del asks (ERASE T1 LOOP?), ENTER erases it, CTRL+Z brings it back;
//              SHIFT+del asks for all of it
//   lift       CTRL+C: the armed track's loop into PERC's slot (TAP01, whole bars that
//              fit), CHOP plays it; a slot holding a sample asks first
//   files      CTRL+S saves the project and its tape folder; the dirty mark follows the
//              tape; a new project empties the tape; loading the project brings it back
//              sample for sample; a project without a tape folder loads an empty tape
//   help       the TAPE key map and the explain texts
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/host_resample.h"
#include "common/host_tape.h"
#include "common/ui_harness.h"
#include "pg/io/project_io.h"
#include "pg/ui/format.h"

using namespace pg;
using namespace pg::ui;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_fail;                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

static uih::Rig* R;
static hosttape::HostTape* T;
static hosttape::Lift* L;
static sampler::MemoryPool* P;
static std::vector<int16_t> g_bufs[sampler::kPoolSlots];
static uih::GhostOrder g_order = uih::GhostOrder::Off;
static int K(const char* n) { return keyFromName(n); }
static void tap(const char* n, uint32_t ms = 40) { R->tap(K(n), ms); R->run(20); }
static void down(const char* n) { R->down(K(n)); R->run(20); }
static void up(const char* n) { R->up(K(n)); R->run(20); }
static void ctrl(const char* n) { down("ctl"); tap(n); up("ctl"); R->run(30); }
static void shift(const char* n) { down("shift"); tap(n); up("shift"); R->run(30); }
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static tape::View V() { return T->tape.view(); }
static uint32_t barMs() { return uint32_t(240000 / R->proj->globals[kGTempo]); }

static void fresh() {
  R->devMode = false;
  R->matrix.order = g_order;
  R->start(true);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
  R->eng.suspend();
  R->eng.setTape(nullptr);
  T->init(120, uint32_t(uih::Rig::kSr));
  R->inst.setSamplePool(P);
  R->eng.setTape(&T->tape);
  R->eng.resume();
  for (int i = 0; i < sampler::kPoolSlots; ++i) P->clear(i);
  R->ui.setTape(&T->tape, L, false);
  R->run(100);
}
static void toTape() {
  tap("`");
  tap("p");
  tap("p");
}
// A take of the mix, L tapped from stop: count-in, two bars, L again.
static void takeFromStop() {
  tap("l");
  R->run(barMs() + barMs() / 2);
  CHECK(V().rec == tape::Rec::Rec);
  R->run(barMs());
  tap("l");
  R->run(barMs() + 300);
}

static void testView() {
  fresh();
  tap("p");
  CHECK(R->ui.view() == View::Song);
  tap("p");
  CHECK(R->ui.view() == View::Tape);
  tap("p");
  CHECK(R->ui.view() == View::Song);
  tap("p");
  CHECK(R->ui.view() == View::Tape);
  tap("`");
  CHECK(R->ui.view() == View::Groove);
}

static void testKeys() {
  fresh();
  toTape();
  tap("x");
  CHECK(R->ui.tapeArmed() == 1);
  tap("v");
  CHECK(R->ui.tapeArmed() == 3);
  tap("z");
  CHECK(R->ui.tapeArmed() == 0);
  tap("2");
  R->run(100);
  CHECK(T->tape.param(1, tape::kPMute) == 1);
  tap("2");
  R->run(100);
  CHECK(T->tape.param(1, tape::kPMute) == 0);
  CHECK(R->ui.tapeField() == kTfLevel);
  tap("=");
  tap("=");
  R->run(100);
  CHECK(T->tape.param(0, tape::kPLevel) == 102);
  down("-");
  down("=");
  up("=");
  up("-");
  R->run(100);
  CHECK(T->tape.param(0, tape::kPLevel) == 100);
  tap(".");
  CHECK(R->ui.tapeField() == kTfPan);
  tap("-");
  R->run(100);
  CHECK(T->tape.param(0, tape::kPPan) == 63);
  tap(";");
  tap(";");
  CHECK(R->ui.tapeField() == kTfLoop);
  tap("=");
  R->run(100);
  CHECK(T->tape.loopBars() == 8);
  tap(";");
  CHECK(R->ui.tapeField() == kTfSource);
  tap("-");
  CHECK(R->ui.tapeSource() == tape::kSrcTrack);
  tap("=");
  tap("=");
  CHECK(R->ui.tapeSource() == tape::kSrcLine);
  tap("l");   // the platform has no line input here
  CHECK(toastHas("NO LINE INPUT"));
  tap("-");
  CHECK(R->ui.tapeSource() == tape::kSrcMix);
}

static void testRecord() {
  fresh();
  toTape();
  CHECK(T->tape.empty());
  tap("l");
  R->run(200);
  CHECK(V().rec == tape::Rec::CountIn);
  CHECK(V().countBeats >= 3 && V().countBeats <= 4);
  CHECK(!R->ui.playing());
  R->run(barMs());
  CHECK(V().rec == tape::Rec::Rec && R->ui.playing());
  R->run(barMs());
  tap("l");
  CHECK(V().rec == tape::Rec::Ending);
  R->run(barMs() + 300);
  CHECK(V().rec == tape::Rec::Idle);
  CHECK(toastHas("T1 ") && toastHas("CTRL+Z UNDO"));
  CHECK(!T->tape.trackEmpty(0));
  CHECK(R->ui.dirty());
  CHECK(T->tape.bpm() == 124);
  ctrl("z");
  CHECK(T->tape.trackEmpty(0) && toastHas("TAPE UNDO"));
  ctrl("z");
  CHECK(!T->tape.trackEmpty(0) && toastHas("TAPE REDO"));
  // held: records while held; the release ends it on the next bar line (transport running)
  tap("x");
  R->down(K("l"));
  R->run(barMs() * 2 + barMs() / 2);
  CHECK(V().rec == tape::Rec::Rec && V().armed == 1);
  R->up(K("l"));
  R->run(barMs() + 300);   // it ends on the bar line after the release
  CHECK(V().rec == tape::Rec::Idle && !T->tape.trackEmpty(1));
  CHECK(V().recorded >= uint32_t(uih::Rig::kSr * 1.5f * float(barMs()) / 1000.0f));
  // SHIFT+L: replace
  tap("c");
  shift("l");
  R->run(barMs() + 100);
  CHECK(V().rec == tape::Rec::Rec && V().replace);
  tap("l");
  R->run(barMs() + 300);
  CHECK(V().rec == tape::Rec::Idle);
  // a second L before the bar line: the take that has not started is cancelled
  tap("l");
  R->run(20);
  tap("l");
  R->run(barMs() + 300);
  CHECK(V().rec == tape::Rec::Idle);
}

static void testTricks() {
  fresh();
  toTape();
  takeFromStop();
  CHECK(R->ui.playing());
  down("b");
  R->run(60);
  CHECK(V().trick == tape::kTrickBrake);
  down("n");
  R->run(400);
  CHECK(V().trick == tape::kTrickHalf);
  up("n");
  R->run(400);
  CHECK(V().trick == tape::kTrickBrake);
  up("b");
  R->run(400);
  CHECK(V().trick == tape::kTrickNone);
  down("m");
  R->run(60);
  CHECK(V().trick == tape::kTrickReverse);
  up("m");
  R->run(400);
  CHECK(V().trick == tape::kTrickNone);
  // a pad held with a trick: both act (two keys)
  const uint8_t trig = R->eng.status().trig[kTrackKick];
  down("a");
  down("n");
  R->run(80);
  CHECK(V().trick == tape::kTrickHalf);
  CHECK(R->eng.status().trig[kTrackKick] != trig);
  up("n");
  up("a");
  R->run(400);
  CHECK(V().trick == tape::kTrickNone);
}

static void testErase() {
  fresh();
  toTape();
  takeFromStop();
  tap("space");
  R->run(200);
  tap("del");
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("enter");
  R->run(100);
  CHECK(T->tape.trackEmpty(0) && toastHas("T1 LOOP ERASED"));
  ctrl("z");
  CHECK(!T->tape.trackEmpty(0));
  shift("del");
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("`");
  CHECK(R->ui.dialog() == Dialog::None && !T->tape.trackEmpty(0));
  tap("x");
  tap("del");
  CHECK(R->ui.dialog() == Dialog::None && toastHas("T2 IS EMPTY"));
}

static void testLift() {
  fresh();
  toTape();
  takeFromStop();
  tap("space");
  R->run(200);
  ctrl("c");
  sampler::SampleRef r;
  sampler::resolve(P, 0, &r);
  CHECK(!r.builtin && std::strcmp(r.name, "TAP01") == 0);
  CHECK(toastHas("SLOT 1 TAP01"));
  // 4 bars at 124 BPM (7.7 s) do not fit a 6.2 s slot: 3 whole bars at 22,050 Hz
  CHECK(r.length > 0 && r.rate == 22050);
  const uint32_t bar = uint32_t(22050.0 * 240.0 / 124.0);
  CHECK(r.length + 2 >= 3 * bar && r.length <= 3 * bar + 2);
  // again: the slot holds a sample now, so it asks
  ctrl("c");
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("enter");
  R->run(50);
  CHECK(toastHas("SLOT 1 TAP01"));
  // CHOP plays it: PERC on CHOP, its slot, a note
  shift("h");
  tap("9");
  for (int i = 0; i < 4; ++i) { tap("."); R->run(200); }
  CHECK(R->ui.samplerOn(kTrackPerc));
  uint8_t before = R->eng.status().trig[kTrackPerc];
  tap("h", 200);
  CHECK(R->eng.status().trig[kTrackPerc] != before);
}

static void testFiles() {
  fresh();
  toTape();
  takeFromStop();
  tap("space");
  R->run(300);
  std::vector<int16_t> was(200000);
  T->tape.read(0, 0, was.data(), uint32_t(was.size()));
  CHECK(R->ui.dirty());
  ctrl("s");
  R->run(100);
  if (R->ui.dialog() == Dialog::Name) tap("enter");
  R->run(400);
  CHECK(!R->ui.dirty());
  CHECK(R->fs.exists("/int/pg/GROOVE01.pgv"));
  CHECK(R->fs.exists("/int/pg/GROOVE01/tape.txt"));
  CHECK(R->fs.exists("/int/pg/GROOVE01/track1.wav"));
  CHECK(!R->fs.exists("/int/pg/GROOVE01/track2.wav"));
  // a change on tape makes it dirty
  toTape();
  tap("2");
  R->run(100);
  CHECK(R->ui.dirty());
  tap("2");
  ctrl("s");
  R->run(400);
  // a new project: an empty tape
  ctrl("n");
  if (R->ui.dialog() == Dialog::Confirm) tap("enter");
  R->run(300);
  CHECK(T->tape.empty());
  // load GROOVE01 again: the tape comes back sample for sample
  tap("\\");
  if (R->ui.projectTab() != 0) tap("\\");
  for (int i = 0; i < 6; ++i) tap(".");
  R->run(100);
  tap("enter");
  if (R->ui.dialog() == Dialog::Confirm) tap("enter");
  R->run(600);
  CHECK(std::strcmp(R->ui.projectName(), "GROOVE01") == 0);
  CHECK(!T->tape.trackEmpty(0));
  std::vector<int16_t> now(was.size());
  T->tape.read(0, 0, now.data(), uint32_t(now.size()));
  CHECK(now == was);
  CHECK(T->tape.bpm() == 124 && T->tape.param(1, tape::kPMute) == 0);
  CHECK(!R->ui.dirty());
  // a demo (no tape folder): an empty tape
  tap("\\");
  if (R->ui.projectTab() != 0) tap("\\");
  for (int i = 0; i < 8; ++i) tap(";");
  tap("enter");
  if (R->ui.dialog() == Dialog::Confirm) tap("enter");
  R->run(400);
  CHECK(T->tape.empty());
}

static void testHelp() {
  fresh();
  toTape();
  down("/");
  R->run(600);
  CHECK(R->ui.helpVisible());
  char lab[3];
  R->ui.helpKeyLabel(K("b"), lab);
  CHECK(std::strcmp(lab, "BK") == 0);
  R->ui.helpKeyLabel(K("l"), lab);
  CHECK(std::strcmp(lab, "TR") == 0);
  up("/");
  tap("/");   // explain mode
  tap("n");
  char title[40];
  const char *l1, *l2;
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strcmp(title, "HOLD: HALF SPEED") == 0);
  tap("l");
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strcmp(title, "TAPE RECORD") == 0);
  tap("/");
  CHECK(!R->ui.explainMode());
}

int main() {
  static uih::Rig rig;
  static hosttape::HostTape ht;
  static hosttape::Lift lift;
  static sampler::MemoryPool pool;
  R = &rig;
  T = &ht;
  L = &lift;
  P = &pool;
  lift.commit = [](int slot, const int16_t* d, uint32_t n, uint32_t rate, std::string* err) {
    return hostrs::commitToPool(P, g_bufs, slot, d, n, rate, err, "TAP");
  };
  for (uih::GhostOrder o : {uih::GhostOrder::Off, uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
    g_order = o;
    testView();
    testKeys();
    testRecord();
    testTricks();
    testErase();
    testLift();
    testFiles();
    testHelp();
  }
  rig.eng.setTape(nullptr);
  std::printf("itest_ui_tape: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
