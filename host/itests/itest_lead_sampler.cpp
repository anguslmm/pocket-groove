// The melodic sampler: sampler instruments in the LEAD slot (`voice lead sampler`,
// UX.md 10.3 "Sampler instruments", MANUAL chapter 20), through the real UI, engine
// and voices, without and with the phantom-key filter:
//   list        the lead steps through its instruments; PLUCK / CLOUD / REEL are
//               samplers: the picture is the sample's waveform (the window in the
//               track colour), the footer the slot and L
//   keys        GROOVE: the note keys play the lead's scale at the track's octave
//               (z = C3 = the sample as recorded, the melodic sampler sounds, the
//               synth lead does not); chord mode plays chords (up to four notes);
//               STEP EDIT: the note keys set the lead's note and chord
//   slot        SHIFT+. / ; step the lead's slot, PERC's is its own; a note plays
//               the lead's slot
//   mic         L on the lead's page records into the lead's slot; the lead plays it
//   resample    SHIFT+L on the lead's page records into the lead's slot; the lead's
//               own notes on that slot are silent meanwhile (the silent-slot rule)
//   engine      chords from steps (MIN7: four notes), a slide glides without a new
//               note; switching the lead between a synth and a sampler instrument
//               mid-pattern, back and forth: each plays only while chosen, no faults
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/host_resample.h"
#include "common/sim_mic.h"
#include "common/ui_harness.h"
#include "pg/engine/engine_hooks.h"
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
static const char* INST(int t) {
  const macro::MacroSet* s = R->ui.instrumentOf(t);
  return s ? s->name : "-";
}
static const sampler::MelodicSampler& MEL() { return R->inst.melodic(); }
static int boxPixels(uint8_t c) {
  int n = 0;
  for (int y = kScopeY0; y <= kScopeY1; ++y)
    for (int x = kScopeX0; x < kScopeX0 + 232; ++x) n += R->fb.px[y * kScreenW + x] == c ? 1 : 0;
  return n;
}
static bool slotFilled(int slot) { return P->header(slot) != nullptr; }
static void post(const Command& c) { R->eng.post(c); }
static void clearAll() {
  for (int s = 0; s < sampler::kPoolSlots; ++s) {
    P->clear(s);
    M->erasedFlag[s] = true;
  }
}
// The notes sounding on the melodic sampler, and on which slot.
static int melNotes() { return MEL().soundingNotes(); }
static bool melOnSlot(int slot) {
  for (int k = 0; k < sampler::mel::kMaxNotes; ++k)
    if (sampler::MelodicSampler::playheadSlot(MEL().playhead(k)) == slot) return true;
  return false;
}
static int synthVoices() { return static_cast<const lead::LeadVoice*>(R->inst.voice(kTrackLead))->soundingVoices(); }

// A new project, the LEAD selected, SOUND open, stepped to `name`.
static void onLead(const char* name) {
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
  shift("k");
  tap("9");
  R->run(50);
  for (int i = 0; i < 12 && std::strcmp(INST(kTrackLead), name) != 0; ++i) {
    tap(".");
    R->run(60);
  }
  // chord mode NONE (a new UI starts on AUTO): one key, one note
  tap("`");
  for (int i = 0; i < kNumChordModes && !toastHas("CHORD NONE"); ++i) tap("l");
  tap("9");
  R->run(1700);
}

static void testList() {
  std::printf("list, picture\n");
  clearAll();
  onLead("HANDS");
  CHECK(!R->ui.samplerOn(kTrackLead));
  int samplers = 0;
  for (int i = 0; i < macro::instrumentCount(kTrackLead); ++i)
    samplers += macro::instrumentAt(kTrackLead, i)->voice == kVoiceSampler ? 1 : 0;
  CHECK_EQ(samplers, 3);
  for (const char* n : {"PLUCK", "CLOUD", "REEL"}) {
    for (int i = 0; i < 12 && std::strcmp(INST(kTrackLead), n) != 0; ++i) tap(".");
    R->run(100);
    CHECK(std::strcmp(INST(kTrackLead), n) == 0);
    CHECK(R->ui.samplerOn(kTrackLead));
    R->frame();
    CHECK(boxPixels(tc(kTrackLead)) > 200);   // the waveform in the track colour
  }
  CHECK(!R->ui.samplerOn(kTrackPerc));       // PERC is its own (TAP)
}

static void testKeys() {
  std::printf("note keys, chords, step edit\n");
  clearAll();
  onLead("PLUCK");
  tap("`");   // GROOVE
  R->run(50);
  CHECK(R->ui.view() == View::Groove && R->ui.selTrack() == kTrackLead);
  down("z");
  R->run(30);
  CHECK_EQ(melNotes(), 1);
  CHECK_EQ(MEL().view(0).note + MEL().view(1).note + MEL().view(2).note + MEL().view(3).note, 48);   // C3: as recorded
  CHECK_EQ(synthVoices(), 0);
  up("z");
  R->run(1500);
  down("c");   // the third degree of the scale
  R->run(30);
  int note = 0;
  for (int k = 0; k < 4; ++k)
    if (MEL().view(k).on && MEL().view(k).gate) note = MEL().view(k).note;
  CHECK_EQ(note, R->ui.noteForKey(2));
  up("c");
  R->run(1500);
  // chord mode AUTO: one key, a chord
  tap("l");
  CHECK(toastHas("CHORD AUTO"));
  down("z");
  R->run(30);
  CHECK(melNotes() >= 3);
  up("z");
  R->run(1500);
  for (int i = 0; i < kNumChordModes - 1; ++i) tap("l");   // back to NONE
  // STEP EDIT: hold step 3, the note keys set its note (and the chord mode's chord)
  down("3");
  R->run(30);
  tap("x");
  up("3");
  const Step s = R->ui.stepAt(kTrackLead, 2);
  CHECK(s.on());
  CHECK_EQ(s.note, R->ui.noteForKey(1));
}

static void testSlot() {
  std::printf("slot\n");
  clearAll();
  onLead("CLOUD");
  const int percSlot = R->ui.sampleSlotOf(kTrackPerc);
  shift(".");
  shift(".");
  shift(".");
  CHECK_EQ(R->ui.sampleSlotOf(kTrackLead), 3);
  R->run(100);
  CHECK_EQ(R->proj->tracks[kTrackLead].sampleSlot, 3);
  CHECK_EQ(R->ui.sampleSlotOf(kTrackPerc), percSlot);
  CHECK(toastHas("SLOT 4"));
  shift(";");
  CHECK_EQ(R->ui.sampleSlotOf(kTrackLead), 2);
  tap("`");
  down("z");
  R->run(40);
  CHECK(melOnSlot(2));
  up("z");
}

static void testMic() {
  std::printf("mic into the lead's slot\n");
  clearAll();
  onLead("PLUCK");
  shift(".");   // slot 2
  shift(".");   // slot 3
  R->run(100);
  CHECK_EQ(R->ui.sampleSlotOf(kTrackLead), 2);
  down("l");
  R->run(400);
  CHECK(R->ui.micBusy() && !R->ui.micView().resample);
  M->amp = 9000.0;
  R->run(600);
  CHECK(R->ui.micView().state == sampler::RecState::Recording);
  M->amp = 0.0;
  R->run(300);
  up("l");
  R->run(100);
  CHECK(!R->ui.micBusy());
  CHECK(slotFilled(2) && !slotFilled(0));
  CHECK(toastHas("SLOT 3 REC03"));   // named after its slot
  // the lead plays it
  R->run(1500);
  tap("`");
  down("z");
  R->run(40);
  CHECK(melOnSlot(2) && melNotes() == 1);
  up("z");
}

static void testResample() {
  std::printf("resample into the lead's slot\n");
  clearAll();
  onLead("CLOUD");
  CHECK(!R->ui.playing());
  // a kick, so there is something to record when the transport runs
  for (int i = 0; i < 16; i += 4) {
    Step s = kDefaultStep;
    s.setFlags(kStepOn);
    s.vel = 120;
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackKick), uint8_t(i), s, nullptr});
  }
  R->run(50);
  down("shift");
  down("l");
  up("shift");
  R->run(400);
  CHECK(R->ui.micBusy() && R->ui.micView().resample);
  CHECK(R->ui.micView().state == sampler::RecState::Waiting);
  // the lead's own slot is silent while it records: a note on the lead's page plays nothing
  down("z");
  R->run(200);
  CHECK_EQ(melNotes(), 0);
  CHECK(R->ui.micView().state == sampler::RecState::Waiting);
  up("z");
  // the groove starts it
  tap("space");
  R->run(2500);
  CHECK(R->ui.micView().state == sampler::RecState::Recording);
  up("l");
  for (int i = 0; i < 300 && R->ui.micBusy(); ++i) R->run(20);
  CHECK(!R->ui.micBusy());
  CHECK(slotFilled(0));
  CHECK(toastHas("SLOT 1 RES01"));
  tap("space");
  R->run(800);
  // after the commit the lead plays the take
  tap("`");
  down("z");
  R->run(60);
  CHECK(melOnSlot(0) && melNotes() == 1);
  up("z");
}

// Engine level: the lead's steps on a sampler instrument.
static void setLeadStep(int i, int note, uint8_t chord, bool slide, int gate = 2) {
  Step s = kDefaultStep;
  s.setFlags(uint8_t(kStepOn | (slide ? kStepSlide : 0)));
  s.note = uint8_t(note);
  s.vel = 110;
  s.gate = uint8_t(gate);
  s.setChord(chord);
  post(Command{CmdType::SetStep, 0, uint8_t(kTrackLead), uint8_t(i), s, nullptr});
}
static const macro::MacroSet* named(int t, const char* n) {
  for (int i = 0; i < macro::instrumentCount(t); ++i)
    if (std::strcmp(macro::instrumentAt(t, i)->name, n) == 0) return macro::instrumentAt(t, i);
  return nullptr;
}

static void testEngine() {
  std::printf("engine: chords, slide, switching mid-pattern\n");
  clearAll();
  onLead("PLUCK");
  for (int i = 0; i < 16; ++i) post(Command{CmdType::SetStep, 0, uint8_t(kTrackLead), uint8_t(i), kDefaultStep, nullptr});
  setLeadStep(0, 48, kChordMin7, false, 8);   // a MIN7 chord, two steps long
  setLeadStep(4, 50, kChordNone, true, 4);    // slides into step 8
  setLeadStep(8, 55, kChordNone, false, 4);
  setLeadStep(12, 53, kChordMaj, false, 4);
  R->run(50);
  tap("space");
  CHECK(R->runUntilStep(kTrackLead, 0));
  R->run(40);
  CHECK_EQ(melNotes(), 4);                     // MIN7: four notes
  CHECK(R->runUntilStep(kTrackLead, 4));
  R->run(30);
  CHECK(R->runUntilStep(kTrackLead, 8));
  R->run(10);
  int gated = 0;
  for (int k = 0; k < 4; ++k) gated += MEL().view(k).on && MEL().view(k).gate ? 1 : 0;
  CHECK_EQ(gated, 1);                          // the slide reused the note: one gated note
  // switching mid-pattern, back and forth, on odd moments
  const uint32_t faults0 = engine_detail::faultCount();
  const char* seq[6] = {"HANDS", "CLOUD", "KEYS", "REEL", "HANDS", "PLUCK"};
  for (int i = 0; i < 6; ++i) {
    R->run(370 + 130 * uint32_t(i));
    R->ui.setInstrument(kTrackLead, named(kTrackLead, seq[i])->id);
    const bool smp = named(kTrackLead, seq[i])->voice == kVoiceSampler;
    bool other = false, own = false;
    R->run(30);   // the switch and the old voice's fade
    for (int k = 0; k < 120; ++k) {   // ~1.5 s: a pass of notes
      R->run(12);
      other |= smp ? synthVoices() > 0 : melNotes() > 0;
      own |= smp ? melNotes() > 0 : synthVoices() > 0;
    }
    std::printf("  %s: own voice sounded %d, the other %d\n", seq[i], int(own), int(other));
    CHECK(own && !other);
  }
  CHECK_EQ(engine_detail::faultCount(), faults0);
  tap("space");
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
    testList();
    testKeys();
    testSlot();
    testMic();
    testResample();
    testEngine();
  }
  std::printf("itest_lead_sampler: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
