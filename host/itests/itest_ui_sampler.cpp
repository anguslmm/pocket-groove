// The sampler instruments' screen and gestures (UX.md 10.3 "Sampler instruments"),
// through the real UI, engine and voices, without and with the phantom-key filter:
//   page        PERC on CHOP: the picture is the sample's waveform (the region in
//               the track colour, the slot number), not the scope
//   slot        SHIFT+. / SHIFT+; step the sample slot (wrapping 12 -> 1), a toast
//               names it, CTRL+Z takes a run of steps back in one go and again redoes
//               it; plain ; . still step the instrument; on a native instrument
//               (TAP) SHIFT+. steps the instrument, not the slot
//   step edit   on CHOP the note keys set the chop's pitch (60 + the scale degree)
//               and GATE is the gate; on TAP they set the velocity and the field is PTCH
//   help        the explain text of ; names the SHIFT layer
#include <cstdio>
#include <cstring>

#include "common/ui_harness.h"
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
#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    ++g_checks;                                                                               \
    long _a = long(a), _b = long(b);                                                          \
    if (_a != _b) {                                                                           \
      ++g_fail;                                                                               \
      std::printf("FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
    }                                                                                         \
  } while (0)

static uih::Rig* R;
static uih::GhostOrder g_order = uih::GhostOrder::Off;
static int K(const char* n) { return keyFromName(n); }
static void tap(const char* n, uint32_t ms = 40) { R->tap(K(n), ms); R->run(20); }
static void down(const char* n) { R->down(K(n)); }
static void up(const char* n) { R->up(K(n)); R->run(20); }
static void ctrl(const char* n) { down("ctl"); tap(n); up("ctl"); R->run(30); }
static void shift(const char* n) { down("shift"); tap(n); up("shift"); R->run(30); }
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static int SLOT() { return R->proj->tracks[kTrackPerc].sampleSlot; }
static const char* INST() {
  const macro::MacroSet* s = R->ui.instrumentOf(kTrackPerc);
  return s ? s->name : "-";
}

// A new project, PERC selected, SOUND open, stepped to `name`.
static void onPerc(const char* name) {
  R->devMode = false;
  R->matrix.order = g_order;
  R->start(false);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
  ctrl("n");
  R->run(100);
  shift("h");
  tap("9");
  R->run(50);
  for (int i = 0; i < 8 && std::strcmp(INST(), name) != 0; ++i) {
    tap(".");
    R->run(60);
  }
  R->run(1700);   // the list overlay goes
}

static void testPage() {
  onPerc("CHOP");
  CHECK(std::strcmp(INST(), "CHOP") == 0);
  CHECK(R->ui.samplerOn(kTrackPerc));
  R->frame();
  // the region (slice 1 of 16 at the start) is drawn in the track colour at the box's left
  int lit = 0, dimmed = 0;
  for (int x = kScopeX0; x < kScopeX0 + 232; ++x)
    for (int y = kScopeY0 + 12; y <= kScopeY1 - 2; ++y) {
      const uint8_t c = R->fb.px[y * kScreenW + x];
      if (c == tc(kTrackPerc)) ++lit;
      if (c == tc(kTrackPerc, kDim)) ++dimmed;
    }
  CHECK(lit > 20 && dimmed > 500);
}

static void testSlot() {
  onPerc("CHOP");
  CHECK_EQ(SLOT(), 0);
  shift(".");
  CHECK_EQ(SLOT(), 1);
  CHECK(toastHas("SLOT 2 BUILT-IN"));
  shift(".");
  shift(".");
  CHECK_EQ(SLOT(), 3);
  ctrl("z");
  CHECK_EQ(SLOT(), 0);   // the run is one unit
  CHECK(toastHas("UNDO"));
  ctrl("z");
  CHECK_EQ(SLOT(), 3);
  CHECK(toastHas("REDO"));
  R->run(2500);           // a new run
  shift(";");
  shift(";");
  shift(";");
  shift(";");
  CHECK_EQ(SLOT(), 11);   // wraps below slot 1
  CHECK(std::strcmp(INST(), "CHOP") == 0);
  tap(".");               // plain . still steps the instrument
  R->run(50);
  CHECK(std::strcmp(INST(), "TAPE") == 0);
  CHECK_EQ(SLOT(), 11);
  // a native instrument: SHIFT+. is the instrument, not the slot
  onPerc("TAP");
  shift(".");
  R->run(50);
  CHECK(std::strcmp(INST(), "TRAIL") == 0);
  CHECK_EQ(SLOT(), 0);
}

static void testStepEdit() {
  onPerc("CHOP");
  tap("`");               // GROOVE
  R->run(50);
  down("3");
  R->run(400);            // STEP EDIT on step 3
  CHECK(R->ui.stepEditOpen());
  tap("c");               // note key 3: the third scale degree above 60
  const int want = degreeNote(0, R->ui.globalValue(kGScaleType), 4, 2);
  Step s = R->ui.stepAt(kTrackPerc, 2);
  CHECK_EQ(s.note, want);
  CHECK_EQ(s.vel, kDefaultStep.vel);
  tap("s");               // the GATE field
  tap("=");
  s = R->ui.stepAt(kTrackPerc, 2);
  CHECK_EQ(s.gate, kDefaultStep.gate + 1);
  up("3");
  R->run(100);
  // TAP: the note keys set the velocity
  onPerc("TAP");
  tap("`");
  R->run(50);
  down("3");
  R->run(400);
  tap("c");
  s = R->ui.stepAt(kTrackPerc, 2);
  CHECK_EQ(s.note, 60);
  CHECK(s.vel != kDefaultStep.vel);
  up("3");
  R->run(100);
}

static void testHelp() {
  onPerc("CHOP");
  tap("/");
  R->run(50);
  tap(";");
  char title[40];
  const char *l1 = "", *l2 = "";
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(l2, "SAMPLE SLOT") != nullptr);
  tap("/");
}

int main() {
  static uih::Rig rig;
  R = &rig;
  const uih::GhostOrder orders[] = {uih::GhostOrder::Off, uih::GhostOrder::PhantomLast};
  for (uih::GhostOrder o : orders) {
    g_order = o;
    testPage();
    testSlot();
    testStepEdit();
    testHelp();
  }
  std::printf("itest_ui_sampler: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
