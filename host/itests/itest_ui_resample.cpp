// Resampling on a sampler instrument's page (UX.md 10.3 "Resampling"): SHIFT+L, through
// the real UI, engine and voices, the host's engine-output recorder
// (host/common/host_resample.h: the real Resampler), committing to a MemoryPool, with
// all three key-matrix models (no ghost filter; the phantom last; the phantom first):
//   in time    the groove plays; SHIFT+L held: the box counts the beats to the bar line
//              (a large digit, a hollow red dot, no WAITING), then REC from the bar
//              line; SHIFT let go, L still held: it goes on; L up: it goes on to the
//              next bar line and ends there: RES01 in the slot, whole bars long, the
//              bar lines marked in the picture while it recorded; the toast names it
//   stopped    nothing plays: WAITING; L still held, SHIFT+k selects the lead and a note
//              played by hand starts it; L up ends it; let go before a sound: NO SOUND
//   own slot   CHOP on the slot being recorded plays nothing meanwhile, the slot plays
//              again after
//   replace    a slot holding a sample asks REPLACE SLOT n? first (ENTER erases it,
//              then SHIFT+L again records)
//   mic        plain L is still the microphone: SHIFT+L never starts the mic, L never
//              the resampler
//   elsewhere  SHIFT+L on a native instrument does nothing; no resampler: NO
//              RESAMPLING; not ready: its reason (RESAMPLE: 44.1K ONLY)
//   footer     with SHIFT held the sampler page's footer is SHIFT's layer (RESAMPLE)
//   help       explain mode: L mentions SHIFT+L
#include <cstdio>
#include <cstring>

#include "common/host_resample.h"
#include "common/sim_mic.h"
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

static uih::Rig* R;
static hostrs::HostResample* RS;
static simmic::SimMic* M;
static sampler::MemoryPool* P;
static std::vector<int16_t> g_bufs[sampler::kPoolSlots];
static uih::GhostOrder g_order = uih::GhostOrder::Off;
static int K(const char* n) { return keyFromName(n); }
static void tap(const char* n, uint32_t ms = 40) { R->tap(K(n), ms); R->run(20); }
static void down(const char* n) { R->down(K(n)); }
static void up(const char* n) { R->up(K(n)); R->run(20); }
static void ctrl(const char* n) { down("ctl"); tap(n); up("ctl"); R->run(30); }
static void shift(const char* n) { down("shift"); tap(n); up("shift"); R->run(30); }
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static const char* INST() {
  const macro::MacroSet* s = R->ui.instrumentOf(kTrackPerc);
  return s ? s->name : "-";
}
static int boxPixels(uint8_t c) {
  int n = 0;
  for (int y = kScopeY0; y <= kScopeY1; ++y)
    for (int x = kScopeX0; x < kScopeX0 + 232; ++x) n += R->fb.px[y * kScreenW + x] == c ? 1 : 0;
  return n;
}
static int footerPixels(uint8_t c, int x0, int x1) {
  int n = 0;
  for (int y = 127; y < 135; ++y)
    for (int x = x0; x < x1; ++x) n += R->fb.px[y * kScreenW + x] == c ? 1 : 0;
  return n;
}
static bool slotFilled(int slot) { return P->header(slot) != nullptr; }
static void post(const Command& c) { R->eng.post(c); }
static void kickSteps() {
  for (int i = 0; i < 16; i += 4) {
    Step s = kDefaultStep;
    s.setFlags(kStepOn);
    s.vel = 120;
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackKick), uint8_t(i), s, nullptr});
  }
  Step h = kDefaultStep;
  h.setFlags(kStepOn);
  h.vel = 90;
  for (int i = 2; i < 16; i += 4) post(Command{CmdType::SetStep, 0, uint8_t(kTrackHatClosed), uint8_t(i), h, nullptr});
}
static void onPerc(const char* name) {
  R->devMode = false;
  R->matrix.order = g_order;
  R->start(false);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
  R->eng.suspend();
  R->inst.setSamplePool(P);
  R->eng.resume();
  R->ui.setMic(M);
  R->ui.setResample(RS);
  M->amp = 0.0;
  ctrl("n");
  R->run(100);
  shift("h");
  tap("9");
  R->run(50);
  for (int i = 0; i < 8 && std::strcmp(INST(), name) != 0; ++i) {
    tap(".");
    R->run(60);
  }
  R->run(1700);
}
static void clearAll() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) {
    P->clear(s);
    M->erasedFlag[s] = true;
  }
}

static void testInTime() {
  clearAll();
  onPerc("CHOP");
  kickSteps();
  R->run(50);
  CHECK(R->ui.samplerOn(kTrackPerc));
  tap("space");
  R->run(300);
  CHECK(R->ui.playing());
  // wait for a moment just after a bar line, so the count-in shows 4
  R->runUntilStep(kTrackKick, 1);
  const int starts = RS->starts, micStarts = M->starts;
  down("shift");
  down("l");
  R->run(40);
  up("shift");   // L alone holds it now
  CHECK(RS->starts == starts + 1 && M->starts == micStarts);
  CHECK(R->ui.micBusy());
  CHECK(R->ui.micView().resample && R->ui.micView().sync);
  CHECK(R->ui.micView().state == sampler::RecState::Waiting);
  R->frame();
  CHECK(boxPixels(RED) > 0 && boxPixels(RED) < 30);   // the hollow dot, no REC
  const int whiteCount = boxPixels(WHITE);
  CHECK(whiteCount > 40);                              // the large beat count
  CHECK(R->ui.beatsToBar() >= 3);
  // count down to the bar line: it starts there
  for (int i = 0; i < 200 && R->ui.micView().state == sampler::RecState::Waiting; ++i) R->run(20);
  CHECK(R->ui.micView().state == sampler::RecState::Recording);
  CHECK(RS->resampler().started());
  R->run(2500);   // into the second bar (124 BPM? the empty project's tempo: a bar ~2 s)
  R->frame();
  CHECK(boxPixels(RED) > 25);                          // the filled dot and REC
  CHECK(R->ui.micView().bars >= 1);                    // a bar line passed
  // let go: it records on to the next bar line, then the slot holds it
  up("l");
  const uint32_t relMs = R->now();
  CHECK(R->ui.micBusy());
  for (int i = 0; i < 300 && R->ui.micBusy(); ++i) R->run(20);
  CHECK(!R->ui.micBusy());
  CHECK(R->now() - relMs < 2600);                      // within a bar of the release
  CHECK(slotFilled(0));
  CHECK(toastHas("SLOT 1 RES01"));
  sampler::SampleRef r;
  CHECK(P->user(0, &r) && std::strcmp(r.name, "RES01") == 0);
  const double barSamples = 22050.0 * 60.0 / double(R->proj->globals[kGTempo]) * 4.0;
  const double bars = double(r.length) / barSamples;
  CHECK(bars > 1.9 && std::fabs(bars - std::round(bars)) * barSamples <= 8.0);   // whole bars
  CHECK(RS->lastState == sampler::RecState::Done);
  CHECK(R->ui.playing());
  tap("space");
}

static void testStopped() {
  clearAll();
  onPerc("CHOP");
  CHECK(!R->ui.playing());
  R->run(300);
  down("shift");
  down("l");
  up("shift");
  R->run(400);
  CHECK(R->ui.micBusy() && R->ui.micView().resample && !R->ui.micView().sync);
  CHECK(R->ui.micView().state == sampler::RecState::Waiting);
  R->frame();
  CHECK(boxPixels(TEXT) > 20);   // WAITING
  // the lead played by hand starts it: L still held, ` to GROOVE, SHIFT+k selects the
  // lead, its note row plays
  tap("`");
  shift("k");
  CHECK(R->ui.micBusy() && R->ui.selTrack() == kTrackLead && R->ui.view() == View::Groove);
  CHECK(R->ui.micView().state == sampler::RecState::Waiting);
  tap("z", 150);
  R->run(500);
  CHECK(R->ui.micView().state == sampler::RecState::Recording);
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy());
  CHECK(slotFilled(0) && toastHas("SLOT 1 RES01"));
  const uint32_t len = slotFilled(0) ? P->header(0)->samples : 0;
  CHECK(len > 10000 && len < 22050);   // from the kick's first sample to the release (~0.6 s)
  // let go before any sound: NO SOUND
  shift("h");   // back to PERC (CHOP)'s page
  tap("9");
  R->run(3000);
  clearAll();
  down("shift");
  down("l");
  up("shift");
  R->run(300);
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy() && toastHas("NO SOUND") && !slotFilled(0));
}

static void testOwnSlot() {
  clearAll();
  onPerc("CHOP");
  kickSteps();
  // PERC (CHOP, slot 1's built-in sound) on every other step
  for (int i = 1; i < 16; i += 2) {
    Step s = kDefaultStep;
    s.setFlags(kStepOn);
    s.vel = 120;
    s.note = 60;
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackPerc), uint8_t(i), s, nullptr});
  }
  tap("space");
  R->run(600);
  bool before = false;
  for (int i = 0; i < 100 && !before; ++i) {
    R->run(10);
    before = R->inst.sampler().playhead() >= 0;
  }
  CHECK(before);   // it plays slot 1's built-in sound
  down("shift");
  down("l");
  up("shift");
  bool during = false;
  for (int i = 0; i < 300; ++i) {
    R->run(10);
    during |= R->inst.sampler().playhead() >= 0 && R->inst.sampler().playingSlot() == 0;
  }
  CHECK(!during);  // silent while its slot records
  up("l");
  for (int i = 0; i < 300 && R->ui.micBusy(); ++i) R->run(20);
  CHECK(slotFilled(0));
  bool after = false;
  for (int i = 0; i < 300 && !after; ++i) {
    R->run(10);
    after = R->inst.sampler().playhead() >= 0 && R->inst.sampler().playingSlot() == 0;
  }
  CHECK(after);    // and plays the new sample after
  tap("space");
}

static void testReplace() {
  clearAll();
  static std::vector<int16_t> buf(sampler::kSlotMaxSamples, 0);
  for (int i = 0; i < 5000; ++i) buf[size_t(i)] = int16_t((i % 100) * 200 - 10000);
  P->set(0, buf.data(), 5000, "OLD");
  onPerc("CHOP");
  const int starts = RS->starts;
  down("shift");
  tap("l");
  up("shift");
  R->run(50);
  CHECK(R->ui.dialog() == Dialog::Confirm);
  R->down(K("enter"));
  R->up(K("enter"));
  R->frame();
  CHECK(R->ui.micErasing());
  R->run(80);
  CHECK(!R->ui.micBusy() && !slotFilled(0) && RS->starts == starts);
  CHECK(toastHas("SLOT 1 EMPTY"));
  down("shift");
  down("l");
  up("shift");
  R->run(300);
  tap("a", 60);
  R->run(400);
  up("l");
  R->run(100);
  CHECK(slotFilled(0) && RS->starts == starts + 1);
}

static void testMicStill() {
  clearAll();
  onPerc("CHOP");
  const int starts = RS->starts, micStarts = M->starts;
  down("l");
  R->run(300);
  CHECK(M->starts == micStarts + 1 && RS->starts == starts);
  CHECK(R->ui.micBusy() && !R->ui.micView().resample);
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy());
}

static void testElsewhere() {
  clearAll();
  onPerc("TAP");
  int starts = RS->starts;
  down("shift");
  tap("l");
  up("shift");
  R->run(100);
  CHECK(RS->starts == starts && !R->ui.micBusy());
  onPerc("CHOP");
  starts = RS->starts;
  R->ui.setResample(nullptr);
  down("shift");
  tap("l");
  up("shift");
  CHECK(toastHas("NO RESAMPLING"));
  R->ui.setResample(RS);
  R->run(1500);
  hostrs::HostResample slow;
  slow.init(&R->eng, &R->inst.sampler(), 32000.0f,
            [](int, const int16_t*, uint32_t, std::string*) { return true; });
  R->ui.setResample(&slow);
  down("shift");
  tap("l");
  up("shift");
  CHECK(toastHas("RESAMPLE: 44.1K ONLY"));
  R->ui.setResample(RS);
  CHECK(RS->starts == starts);
}

static void testFooterHelp() {
  onPerc("CHOP");
  R->frame();
  const int plain = footerPixels(WHITE, 4, 195);
  down("shift");
  R->run(60);
  R->frame();
  const int shifted = footerPixels(WHITE, 4, 195);
  up("shift");
  R->run(60);
  CHECK(plain > 0 && shifted > 0 && plain != shifted);   // a different footer while SHIFT is held
  tap("/");
  R->run(50);
  tap("l");
  char title[40];
  const char *l1 = "", *l2 = "";
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(title, "MIC") != nullptr && std::strstr(l1, "SHIFT+L") != nullptr);
  CHECK(!R->ui.micBusy());
  tap("/");
}

int main() {
  static uih::Rig rig;
  static simmic::SimMic mic;
  static sampler::MemoryPool pool;
  static hostrs::HostResample rs;
  R = &rig;
  M = &mic;
  P = &pool;
  RS = &rs;
  mic.pool = &pool;
  mic.clockMs = [] { return R->now(); };
  rs.init(&rig.eng, &rig.inst.sampler(), uih::Rig::kSr,
          [](int slot, const int16_t* d, uint32_t n, std::string* err) {
            return hostrs::commitToPool(P, g_bufs, slot, d, n, err);
          },
          [](int slot) {
            P->clear(slot);
            M->erasedFlag[slot] = true;
            return true;
          });
  for (uih::GhostOrder o : {uih::GhostOrder::Off, uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
    g_order = o;
    testInTime();
    testStopped();
    testOwnSlot();
    testReplace();
    testMicStill();
    testElsewhere();
    testFooterHelp();
  }
  std::printf("itest_ui_resample: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
