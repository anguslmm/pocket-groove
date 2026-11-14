// The sampler instruments' tune gesture and readout (UX.md 10.3 "Tune"), through the
// real UI, engine and voices, on all three key-matrix models (diodes, a diode-less
// matrix with the phantom key last in its batch, and first):
//   gesture   on PERC's CHOP page CTRL+; / CTRL+. tune the track a semitone up / down
//             and CTRL+SHIFT+; / . by 5 cents (CTRL+SHIFT+; closes a rectangle with
//             `,`: the filter keeps `;`), a toast says it, the project holds it; the
//             instrument and the slot stay; clamped at +-24 and +-50 cents
//   undo      CTRL+Z takes a run of steps back in one go, again redoes it
//   readout   the waveform header shows "+3" / "-2 +15C" left of the length (WHITE),
//             nothing when untuned; CTRL held shows the tune footer
//   lead      the same on the LEAD's PLUCK page, the lead's own tune (PERC's stays)
//   native    on a native instrument (TAP) CTRL+; / . do nothing; with a step held on
//             the sampler page neither; in STEP EDIT CTRL+; is still the octave
//   explain   CTRL+; explains the tune on a sampler page
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
static void ctrlShift(const char* n) { down("ctl"); down("shift"); tap(n); up("shift"); up("ctl"); R->run(30); }
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static const char* INST(int t) {
  const macro::MacroSet* s = R->ui.instrumentOf(t);
  return s ? s->name : "-";
}
static int SEMIS(int t) { return tuneValue(R->proj->tracks[t].params[kMixTune]); }
static int CENTS(int t) { return tuneValue(R->proj->tracks[t].params[kMixTuneCents]); }
static bool readout(const char* want) {
  char b[16];
  R->ui.tuneText(R->ui.selTrack(), b);
  return std::strcmp(b, want) == 0;
}
// WHITE pixels in the waveform header between the name and the length (the readout)
static int headerWhite() {
  R->frame();
  int n = 0;
  for (int y = kScopeY0 + 2; y <= kScopeY0 + 10; ++y)
    for (int x = kScopeX0 + 70; x < kScopeX0 + 232 - 28; ++x) n += R->fb.px[y * kScreenW + x] == WHITE ? 1 : 0;
  return n;
}
static uint32_t footerHash() {
  R->frame();
  uint32_t h = 2166136261u;
  for (int y = 120; y < kScreenH; ++y)
    for (int x = 0; x < 200; ++x) h = (h ^ R->fb.px[y * kScreenW + x]) * 16777619u;
  return h;
}

// A new project, track t selected (its pad), SOUND open, stepped to `name`.
static void onTrack(int t, const char* name) {
  R->devMode = false;
  R->matrix.order = g_order;
  R->start(false);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
  ctrl("n");
  R->run(100);
  down("shift");
  tap(t == kTrackPerc ? "h" : "k");
  up("shift");
  tap("9");
  R->run(50);
  for (int i = 0; i < 12 && std::strcmp(INST(t), name) != 0; ++i) {
    tap(".");
    R->run(60);
  }
  R->run(1700);   // the list overlay goes
}

static void testGesture(int t, const char* name) {
  onTrack(t, name);
  CHECK(std::strcmp(INST(t), name) == 0 && R->ui.samplerOn(t));
  const int slot = R->proj->tracks[t].sampleSlot;
  CHECK_EQ(SEMIS(t), 0);
  CHECK_EQ(CENTS(t), 0);
  CHECK(readout(""));
  CHECK_EQ(headerWhite(), 0);
  ctrl(";");
  CHECK_EQ(SEMIS(t), 1);
  CHECK(toastHas("TUNE +1"));
  CHECK(readout("+1"));
  CHECK(headerWhite() > 0);
  ctrl(".");
  ctrl(".");
  ctrl(".");
  CHECK_EQ(SEMIS(t), -2);
  ctrlShift(";");
  ctrlShift(";");
  ctrlShift(";");
  CHECK_EQ(CENTS(t), 15);
  CHECK(toastHas("TUNE -2 +15C"));
  CHECK(readout("-2 +15C"));
  ctrlShift(".");
  CHECK_EQ(CENTS(t), 10);
  // the instrument, the slot and the sound stay
  CHECK(std::strcmp(INST(t), name) == 0);
  CHECK_EQ(R->proj->tracks[t].sampleSlot, slot);
  // undo: the whole run back, again: redo
  ctrl("z");
  CHECK_EQ(SEMIS(t), 0);
  CHECK_EQ(CENTS(t), 0);
  CHECK(toastHas("TUNE 0"));
  CHECK_EQ(headerWhite(), 0);
  ctrl("z");
  CHECK_EQ(SEMIS(t), -2);
  CHECK_EQ(CENTS(t), 10);
  // clamped at the ends
  R->run(2100);   // a new run
  for (int i = 0; i < 30; ++i) ctrl(";");
  CHECK_EQ(SEMIS(t), 24);
  for (int i = 0; i < 14; ++i) ctrlShift(";");
  CHECK_EQ(CENTS(t), 50);
  CHECK(readout("+24 +50C"));
  for (int i = 0; i < 60; ++i) ctrl(".");
  CHECK_EQ(SEMIS(t), -24);
  for (int i = 0; i < 25; ++i) ctrlShift(".");
  CHECK_EQ(CENTS(t), -50);
  CHECK(readout("-24 -50C"));
  // the CTRL footer
  const uint32_t plain = footerHash();
  down("ctl");
  R->run(40);
  const uint32_t held = footerHash();
  up("ctl");
  CHECK(plain != held);
  CHECK_EQ(footerHash(), plain);
  // the explain text
  tap("/");
  R->run(50);
  down("ctl");
  tap(";");
  char title[40];
  const char *l1 = "", *l2 = "";
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(title, "TUNE +1") != nullptr && std::strstr(l2, "5 CENTS") != nullptr);
  up("ctl");
  tap("/");
  CHECK_EQ(SEMIS(t), -24);   // explain mode only explains
}

static void testLeadOwn() {
  onTrack(kTrackLead, "PLUCK");
  ctrl(";");
  ctrl(";");
  ctrl(";");
  CHECK_EQ(SEMIS(kTrackLead), 3);
  CHECK_EQ(SEMIS(kTrackPerc), 0);
  CHECK(readout("+3"));
}

static void testNative() {
  onTrack(kTrackPerc, "TAP");
  CHECK(!R->ui.samplerOn(kTrackPerc));
  ctrl(";");
  ctrl(".");
  ctrlShift(";");
  CHECK_EQ(SEMIS(kTrackPerc), 0);
  CHECK_EQ(CENTS(kTrackPerc), 0);
  CHECK(std::strcmp(INST(kTrackPerc), "TAP") == 0);
  // a sampler page with a step held: no tune
  onTrack(kTrackPerc, "CHOP");
  down("3");
  R->run(400);
  ctrl(";");
  up("3");
  R->run(100);
  CHECK_EQ(SEMIS(kTrackPerc), 0);
  // STEP EDIT (GROOVE, a step held): CTRL+; is the octave, not the tune
  tap("`");
  tap("3");   // a step on
  R->run(100);
  const int before = R->step(kTrackPerc, 2).note;
  down("3");
  R->run(400);
  ctrl(";");
  up("3");
  R->run(100);
  CHECK_EQ(R->step(kTrackPerc, 2).note, before + 12);
  CHECK_EQ(SEMIS(kTrackPerc), 0);
}

int main() {
  static uih::Rig rig;
  R = &rig;
  for (uih::GhostOrder o : {uih::GhostOrder::Off, uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
    g_order = o;
    testGesture(kTrackPerc, "CHOP");
    testGesture(kTrackLead, "PLUCK");
    testLeadOwn();
    testNative();
  }
  std::printf("itest_ui_tune: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
