// Recording from the microphone on a sampler instrument's page (stage F2; UX.md 10.3
// "Recording"), through the real UI, engine and voices, a simulated microphone
// (host/common/sim_mic.h: the real Recorder on a synthetic room and voice), and all
// three key-matrix models (no ghost filter; the phantom last; the phantom first):
//   record     hold L on CHOP with an empty, erased slot: WAITING (the box), the voice
//              starts it (REC, the time, the wave growing, a red dot), letting go ends
//              it: the slot holds REC01 (a toast names it, the picture is the sample),
//              and the next note plays it
//   no sound   let go before any sound: NO SOUND, the slot stays empty
//   replace    a slot holding a sample asks REPLACE SLOT n? (ESC keeps it; ENTER: ERASING
//              is drawn, the next frame erases, SLOT n EMPTY), then hold L to record
//   dirty      an empty slot that is not erased: ERASING, then (L still held) WAITING;
//              let go during the erase: no recording after it
//   full       held past 6.2 s of sound: it stops by itself; letting go later is harmless
//   playing    recording while the transport plays works the same (no gap: the engine
//              runs every block)
//   elsewhere  L on a native instrument (TAP), with a step held, or on another view does
//              not record; no microphone: NO MIC; a microphone that is not ready: its reason
//   help       explain mode: L on a sampler page explains the microphone
#include <cstdio>
#include <cstring>

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
static simmic::SimMic* M;
static sampler::MemoryPool* P;
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
// pixels of colour c in the picture's box
static int boxPixels(uint8_t c) {
  int n = 0;
  for (int y = kScopeY0; y <= kScopeY1; ++y)
    for (int x = kScopeX0; x < kScopeX0 + 232; ++x) n += R->fb.px[y * kScreenW + x] == c ? 1 : 0;
  return n;
}
static bool slotFilled(int slot) { return P->header(slot) != nullptr; }

// A new project, PERC selected, SOUND open, stepped to `name`; the pool and the mic attached.
static void onPerc(const char* name) {
  R->devMode = false;
  R->matrix.order = g_order;
  R->start(false);
  if (g_order == uih::GhostOrder::Off) R->ui.setGhostFilter(false);
  R->eng.suspend();
  R->inst.setSamplePool(P);
  R->eng.resume();
  R->ui.setMic(M);
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
  R->run(1700);   // the list overlay goes
}

static void testRecord() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  onPerc("CHOP");
  CHECK(R->ui.samplerOn(kTrackPerc));
  const int starts = M->starts;
  down("l");
  R->run(100);
  CHECK(M->starts == starts + 1);
  CHECK(R->ui.micBusy());
  CHECK(R->ui.micView().state == sampler::RecState::Settling || R->ui.micView().state == sampler::RecState::Waiting);
  R->run(300);
  CHECK(R->ui.micView().state == sampler::RecState::Waiting);
  CHECK(R->ui.micView().threshold > 0);
  R->frame();
  CHECK(boxPixels(RED) > 0 && boxPixels(RED) < 30);   // the hollow dot
  // the voice comes in
  M->amp = 9000.0;
  R->run(600);
  CHECK(R->ui.micView().state == sampler::RecState::Recording);
  CHECK(R->ui.micView().samples > 10000);
  R->frame();
  CHECK(boxPixels(RED) > 25);                         // the filled dot and REC
  CHECK(boxPixels(tc(kTrackPerc)) > 60);              // the wave so far
  CHECK(R->ui.micView().colsUsed > 20 && R->ui.micView().colsUsed < 60);
  M->amp = 0.0;
  R->run(300);
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy());
  CHECK(slotFilled(0));
  CHECK(toastHas("SLOT 1 REC01"));
  sampler::SampleRef r;
  CHECK(P->user(0, &r) && std::strcmp(r.name, "REC01") == 0);
  CHECK(r.length > 12000 && r.length < 22050);        // ~0.6 s of voice + pre-roll + the room after, trimmed
  // the next note plays it: the sampler's playhead runs
  R->run(1800);
  down("1");
  R->run(30);
  up("1");
  bool played = false;
  CHECK(R->ui.stepAt(kTrackPerc, 0).on());
  R->down(K("space"));   // a 16th slice of 0.7 s is ~45 ms: watch from the key on
  for (int i = 0; i < 200 && !played; ++i) {
    R->run(2);
    played = R->inst.sampler().playhead() >= 0 && R->inst.sampler().playingSlot() == 0;
  }
  R->up(K("space"));
  CHECK(played);
  tap("space");
}

static void testNoSound() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  onPerc("CHOP");
  down("l");
  R->run(800);
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy());
  CHECK(toastHas("NO SOUND"));
  CHECK(!slotFilled(0));
}

static void testReplace() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  static std::vector<int16_t> buf(sampler::kSlotMaxSamples, 0);
  for (int i = 0; i < 5000; ++i) buf[size_t(i)] = int16_t((i % 100) * 200 - 10000);
  P->set(0, buf.data(), 5000, "OLD");
  M->erasedFlag[0] = false;
  onPerc("CHOP");
  const int starts = M->starts, erases = M->erases;
  tap("l");
  R->run(50);
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("`");   // ESC: keep it
  R->run(50);
  CHECK(R->ui.dialog() == Dialog::None && slotFilled(0) && M->erases == erases);
  tap("l");
  R->run(50);
  CHECK(R->ui.dialog() == Dialog::Confirm);
  R->down(K("enter"));
  R->up(K("enter"));
  R->frame();
  CHECK(R->ui.micErasing());
  CHECK(boxPixels(AMBER) > 40);                       // ERASING on the screen first
  R->run(80);
  CHECK(!R->ui.micBusy() && M->erases == erases + 1);
  CHECK(!slotFilled(0) && M->starts == starts);       // erased, not recording (L was let go)
  CHECK(toastHas("SLOT 1 EMPTY"));
  // now hold L: it records
  down("l");
  R->run(400);
  M->amp = 8000.0;
  R->run(500);
  M->amp = 0.0;
  up("l");
  R->run(100);
  CHECK(slotFilled(0) && M->starts == starts + 1);
}

static void testDirty() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  M->erasedFlag[0] = false;   // empty, not erased (a transfer cut off)
  onPerc("CHOP");
  const int starts = M->starts, erases = M->erases;
  down("l");
  R->frame();
  CHECK(R->ui.dialog() == Dialog::None);             // nothing to lose: no question
  CHECK(R->ui.micErasing());
  R->run(100);
  CHECK(M->erases == erases + 1 && M->starts == starts + 1 && R->ui.micBusy());   // still held: listening
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy());
  // let go during the erase: no recording after it
  M->erasedFlag[0] = false;
  down("l");
  up("l");   // released before the next frame erases
  R->run(100);
  CHECK(M->erases == erases + 2 && M->starts == starts + 1 && !R->ui.micBusy());
}

static void testFull() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  onPerc("CHOP");
  down("l");
  R->run(300);
  M->amp = 9000.0;
  R->run(6700);
  CHECK(!R->ui.micBusy());
  CHECK(slotFilled(0) && P->header(0)->samples == sampler::kSlotMaxSamples);
  const int rel = M->releases;
  M->amp = 0.0;
  up("l");
  R->run(50);
  CHECK(slotFilled(0) && M->releases == rel);         // the late key-up does nothing
}

static void testPlaying() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  onPerc("CHOP");
  R->runUntilStep(0, 0, 100);
  tap("space");
  R->run(200);
  CHECK(R->ui.playing());
  const uint64_t s0 = R->samples;
  down("l");
  R->run(300);
  M->amp = 7000.0;
  R->run(700);
  M->amp = 0.0;
  up("l");
  R->run(100);
  CHECK(slotFilled(0));
  CHECK(R->ui.playing());
  CHECK(R->samples - s0 >= uint64_t(1.0 * 44100));   // the engine ran all along
  tap("space");
}

static void testElsewhere() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) { P->clear(s); M->erasedFlag[s] = true; }
  onPerc("TAP");
  int starts = M->starts;
  tap("l");
  R->run(100);
  CHECK(M->starts == starts && !R->ui.micBusy());   // a native instrument: L is not the mic
  onPerc("CHOP");
  starts = M->starts;
  down("3");
  R->run(400);
  tap("l");
  up("3");
  R->run(100);
  CHECK(M->starts == starts && !R->ui.micBusy());   // a step held
  tap("`");
  R->run(50);
  tap("l");
  R->run(50);
  CHECK(M->starts == starts);                        // GROOVE
  // no microphone; one that is not ready
  tap("9");
  R->run(1700);
  R->ui.setMic(nullptr);
  tap("l");
  CHECK(toastHas("NO MIC"));
  R->ui.setMic(M);
  M->isReady = false;
  M->notReady = "44.1K ONLY";
  R->run(1500);
  tap("l");
  CHECK(toastHas("44.1K ONLY"));
  M->isReady = true;
  CHECK(M->starts == starts);
}

static void testHelp() {
  onPerc("CHOP");
  tap("/");
  R->run(50);
  tap("l");
  char title[40];
  const char *l1 = "", *l2 = "";
  R->ui.explainText(title, &l1, &l2);
  CHECK(std::strstr(title, "MIC") != nullptr);
  CHECK(!R->ui.micBusy());
  tap("/");
}

int main() {
  static uih::Rig rig;
  static simmic::SimMic mic;
  static sampler::MemoryPool pool;
  R = &rig;
  M = &mic;
  P = &pool;
  mic.pool = &pool;
  mic.clockMs = [] { return R->now(); };
  for (uih::GhostOrder o : {uih::GhostOrder::Off, uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
    g_order = o;
    testRecord();
    testNoSound();
    testReplace();
    testDirty();
    testFull();
    testPlaying();
    testElsewhere();
    testHelp();
  }
  std::printf("itest_ui_mic: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
