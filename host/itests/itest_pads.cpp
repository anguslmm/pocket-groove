// System test: the pad input (pg/ui/pads.h; docs/UX.md 10.3 "Pads"), through the UI
// rig (host/common/ui_harness.h). A test set laid over the library: PT on the bass,
// `vel OPEN 0.5`, `press BITE`; tilt left at its default (X: OPEN, Y: BITE).
//
//   drums       the drums job: pads 1-4 play KICK SNARE CHH PERC (pads 5-6 CLAP OHH,
//               further pads nothing), note 60, the hit's velocity; the selected track
//               stays
//   notes       the notes job: pad i plays degree i of the selected track (synth: the
//               note keys' note; a drum: 60 + the degree); the track is latched at the
//               hit (selecting another track before the release releases the right one);
//               a second pad held on a synth slides
//   velocity    0..1 to 1..127, linear
//   record      live record: pad hits land in the pattern with their velocity, drums
//               and notes, one undo unit with the pass
//   pressure    the largest pressure of the track's held pads moves its press macro
//               as an offset (dead zone, range), the last pad up glides back; not
//               stored, not recorded; CLASSIC: nothing; per track in the drums job
//   with tilt   tilt and pressure at once, on two macros and on one (they add);
//               releasing either leaves the other's part
//   velocity    a pad hit's velocity moves OPEN for the note (the vel line)
//   rate        a sensor read every millisecond sends a bounded number of offsets,
//               the last value always arrives
//   job         togglePadJob says which job in the toast
#include <cmath>
#include <cstring>

#include "common/ui_harness.h"
#include "itests/itest_util.h"
#include "pg/macro/macro.h"
#include "pg/ui/keys.h"
#include "pg/ui/pads.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

struct Overlay final : pm::LibraryOverlay {
  pm::MacroSet set;
  const pm::MacroSet* find(uint16_t id) const override { return id == set.id ? &set : nullptr; }
  int extraCount(int track) const override { return track == set.track ? 1 : 0; }
  const pm::MacroSet* extra(int track, int i) const override { return track == set.track && i == 0 ? &set : nullptr; }
};
Overlay g_ov;

const char kPT[] =
    "voice bass\nname PT\n"
    "vel OPEN 0.5\n"
    "press BITE\n"
    "macro OPEN\n  cutoff 0:10 1:120\n"
    "macro BITE\n  resonance 0:0 1:120\n";

uih::Rig& rig() {
  static uih::Rig r;
  return r;
}
int heard(int t, int m) { return rig().eng.status().macroPos[t][m]; }

// The rig with an empty project, PT on the bass at OPEN 100, BITE 60; the bass selected.
void start() {
  uih::Rig& r = rig();
  r.devMode = false;
  r.start(false);
  pm::ParamTable pt;
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) pt.info[t][p] = r.eng.trackParamInfo(t, p);
  pm::ParseError e;
  it::check(pm::parse(kPT, sizeof(kPT) - 1, pt, &g_ov.set, &e), "PT parses (%s)", e.msg);
  pm::setLibraryOverlay(&g_ov);
  r.run(50);
  r.ui.setInstrument(kTrackBass, g_ov.set.id);
  r.run(100);
  r.ui.setMacro(kTrackBass, 0, 100);
  r.ui.setMacro(kTrackBass, 1, 60);
  r.run(300);
  it::check(r.ui.selTrack() == kTrackBass, "the bass is selected at power-on");
}

// Selects track t silently (SHIFT + its track key).
void select(int t) {
  uih::Rig& r = rig();
  r.down(ui::kKShift);
  r.tap(ui::kKPad0 + t);
  r.up(ui::kKShift);
  r.run(40);
}

void hit(int pad, float v, uint32_t holdMs = 60) {
  uih::Rig& r = rig();
  r.ui.padDown(pad, v, r.now());
  r.run(holdMs);
  r.ui.padUp(pad, r.now());
  r.run(30);
}

void testDrums() {
  std::printf("drums job\n");
  start();
  uih::Rig& r = rig();
  it::check(r.ui.padJob() == ui::PadJob::Drums, "the drums job after init");
  const int want[6] = {kTrackKick, kTrackSnare, kTrackHatClosed, kTrackPerc, kTrackClap, kTrackHatOpen};
  for (int p = 0; p < 8; ++p) {
    const int t = r.ui.padTrack(p);
    it::check(t == (p < 6 ? want[p] : -1), "drums: pad %d plays track %d (%d)", p + 1, p < 6 ? want[p] : -1, t);
    if (t >= 0) it::check(r.ui.padNote(p) == 60, "drums: pad %d plays note 60", p + 1);
  }
  for (int p = 0; p < 4; ++p) {
    uint8_t before[kNumTracks];
    std::memcpy(before, r.eng.status().trig, kNumTracks);
    hit(p, 0.8f);
    const EngineStatus st = r.eng.status();
    for (int t = 0; t < kNumTracks; ++t)
      it::check((st.trig[t] != before[t]) == (t == want[p]), "drums: pad %d fires track %d only (track %d)", p + 1, want[p], t);
  }
  it::check(r.ui.selTrack() == kTrackBass, "drums: the selected track stays");
  hit(7, 1.0f);   // nothing
  it::check(!r.ui.padHeld(7), "drums: a pad with no track is not held");
  r.ui.padDown(40, 1.0f, r.now());   // out of range
  r.ui.padUp(-1, r.now());
  it::check(true, "out-of-range pads are ignored");
}

void testVelocityMap() {
  std::printf("velocity mapping\n");
  it::check(ui::padVelocity(0.0f) == 1 && ui::padVelocity(1.0f) == 127 && ui::padVelocity(0.5f) == 64,
            "0 -> 1, 0.5 -> 64, 1 -> 127");
  it::check(ui::padVelocity(-1.0f) == 1 && ui::padVelocity(2.0f) == 127 && ui::padVelocity(std::nanf("")) == 1,
            "out of range and NaN are clamped");
  for (int i = 1; i <= 100; ++i)
    it::check(ui::padVelocity(float(i) / 100.0f) >= ui::padVelocity(float(i - 1) / 100.0f), "monotonic at %d", i);
}

// Live record on, playing: the pads' notes land in the pattern.
void testRecord() {
  std::printf("live record\n");
  start();
  uih::Rig& r = rig();
  r.tap(ui::kKEnter);            // record on
  r.tap(ui::kKSpace);            // play
  r.run(200);
  it::check(r.ui.recOn() && r.ui.playing(), "recording and playing");
  hit(0, 1.0f);                  // kick, 127
  r.run(300);
  hit(1, 0.25f);                 // snare, 33
  r.run(300);
  r.ui.setPadJob(ui::PadJob::Notes);
  hit(2, 0.5f, 120);             // bass, degree 2, 64
  r.run(200);
  r.tap(ui::kKSpace);            // stop
  r.run(100);
  int kick = 0, snare = 0, bass = 0;
  for (int i = 0; i < 16; ++i) {
    const Step& k = r.step(kTrackKick, i);
    const Step& s = r.step(kTrackSnare, i);
    const Step& b = r.step(kTrackBass, i);
    if (k.on() && k.vel == 127) ++kick;
    if (s.on() && s.vel == 33) ++snare;
    if (b.on() && b.vel == 64 && b.note == r.ui.noteForKey(2)) ++bass;
  }
  it::check(kick == 1, "record: the kick pad's hit is a step at velocity 127 (%d)", kick);
  it::check(snare == 1, "record: the snare pad's hit at velocity 33 (%d)", snare);
  it::check(bass == 1, "record: the notes job's pad 3 on the bass: degree 2 at velocity 64 (%d)", bass);
  it::check(r.ui.undoAvailable(), "record: the pass is one undo unit");
  r.tap(ui::kKEnter);            // record off
  r.run(50);
}

void testNotes() {
  std::printf("notes job\n");
  start();
  uih::Rig& r = rig();
  r.ui.togglePadJob();
  it::check(r.ui.padJob() == ui::PadJob::Notes && std::strcmp(r.ui.toastText(), "PADS: NOTES") == 0,
            "toggle: the notes job, the toast says so ('%s')", r.ui.toastText());
  for (int p = 0; p < 4; ++p) {
    it::check(r.ui.padTrack(p) == kTrackBass, "notes: pad %d plays the selected track", p + 1);
    it::check(r.ui.padNote(p) == r.ui.noteForKey(p), "notes: pad %d plays the note key %d's note (%d %d)", p + 1, p + 1,
              r.ui.padNote(p), r.ui.noteForKey(p));
  }
  // the track is latched: select the lead while holding, the release goes to the bass
  r.ui.padDown(0, 0.7f, r.now());
  r.run(40);
  select(kTrackLead);
  it::check(r.ui.selTrack() == kTrackLead, "notes: the lead is selected now");
  it::check(r.ui.padTrack(1) == kTrackLead, "notes: the next pad plays the lead");
  r.ui.padUp(0, r.now());
  r.run(40);
  it::check(!r.ui.padHeld(0), "notes: the latched pad released");
  // drums in the notes job: 60 + the scale degree
  select(kTrackKick);
  it::check(r.ui.selTrack() == kTrackKick, "notes: the kick selected");
  it::check(r.ui.padNote(0) == 60 && r.ui.padNote(1) > 60 && r.ui.padNote(3) > r.ui.padNote(1),
            "notes on a drum: 60 and up the scale (%d %d %d)", r.ui.padNote(0), r.ui.padNote(1), r.ui.padNote(3));
  r.ui.togglePadJob();
  it::check(r.ui.padJob() == ui::PadJob::Drums && std::strcmp(r.ui.toastText(), "PADS: DRUMS") == 0,
            "toggle: back to drums ('%s')", r.ui.toastText());
  // legato on a synth: record a slide
  select(kTrackBass);
  r.ui.setPadJob(ui::PadJob::Notes);
  r.tap(ui::kKEnter);
  r.tap(ui::kKSpace);
  r.run(300);
  r.ui.padDown(0, 0.8f, r.now());
  r.run(250);
  r.ui.padDown(2, 0.8f, r.now());   // the first still held
  r.run(60);
  r.ui.padUp(0, r.now());
  r.run(200);
  r.ui.padUp(2, r.now());
  r.run(60);
  r.tap(ui::kKSpace);
  r.tap(ui::kKEnter);
  r.run(60);
  int slid = 0;
  for (int i = 0; i < 16; ++i)
    if (r.step(kTrackBass, i).on() && (r.step(kTrackBass, i).flags() & kStepSlide)) ++slid;
  it::check(slid == 1, "notes: a second pad held on the bass slides into it (%d slide steps)", slid);
}

void testPressure() {
  std::printf("pressure\n");
  start();
  uih::Rig& r = rig();
  r.ui.setPadJob(ui::PadJob::Notes);
  const Project before = *r.proj;
  r.ui.padDown(0, 0.787f, r.now());   // velocity 100: OPEN stays
  r.run(60);
  it::check(heard(kTrackBass, 0) == 100 && heard(kTrackBass, 1) == 60, "pressure: none yet (%d %d)", heard(kTrackBass, 0),
            heard(kTrackBass, 1));
  it::check(r.ui.pressMacro(kTrackBass) == 1, "pressure: PT's press line names BITE (%d)", r.ui.pressMacro(kTrackBass));
  r.ui.padPressure(0, 0.04f, r.now());   // inside the dead zone
  r.run(80);
  it::check(heard(kTrackBass, 1) == 60, "pressure: the dead zone moves nothing (%d)", heard(kTrackBass, 1));
  r.ui.padPressure(0, 1.0f, r.now());
  r.run(80);
  it::check(heard(kTrackBass, 1) == 60 + 160, "pressure: full pressure moves BITE by 160 (%d)", heard(kTrackBass, 1));
  r.ui.padPressure(0, 0.53f, r.now());
  r.run(80);
  const int half = 60 + ui::pressOffset16(0.53f, 1) / kOffsetScale;
  it::check(std::abs(heard(kTrackBass, 1) - half) <= 1, "pressure: about half (%d, want %d)", heard(kTrackBass, 1), half);
  // a second pad of the track: the larger one counts
  r.ui.padDown(1, 0.787f, r.now());
  r.ui.padPressure(1, 0.2f, r.now());
  r.run(80);
  it::check(std::abs(heard(kTrackBass, 1) - half) <= 1, "pressure: two pads: the larger (%d)", heard(kTrackBass, 1));
  r.ui.padUp(0, r.now());
  r.run(80);
  const int fifth = 60 + ui::pressOffset16(0.2f, 1) / kOffsetScale;
  it::check(std::abs(heard(kTrackBass, 1) - fifth) <= 1, "pressure: the other pad's when the first goes up (%d, want %d)",
            heard(kTrackBass, 1), fifth);
  it::check(r.ui.macroValue(kTrackBass, 1) == 60 && std::memcmp(r.proj->tracks, before.tracks, sizeof(before.tracks)) == 0,
            "pressure: the stored position stays");
  r.ui.padUp(1, r.now());
  r.run(60);
  it::check(heard(kTrackBass, 1) > 60 && heard(kTrackBass, 1) < fifth, "pressure: the last pad up glides back (%d)",
            heard(kTrackBass, 1));
  r.run(150);
  it::check(heard(kTrackBass, 1) == 60 && r.ui.pressOffset(kTrackBass) == 0, "pressure: back at the base (%d)",
            heard(kTrackBass, 1));
  // CLASSIC: nothing
  select(kTrackPerc);   // CLASSIC in an empty project
  r.ui.padDown(0, 0.8f, r.now());
  r.ui.padPressure(0, 1.0f, r.now());
  r.run(80);
  it::check(r.ui.pressMacro(kTrackPerc) == -1 && r.ui.pressOffset(kTrackPerc) == 0, "pressure: a CLASSIC track: nothing");
  r.ui.padUp(0, r.now());
  r.run(40);
  // the drums job: per track (two pads, two tracks)
  r.ui.setInstrument(kTrackKick, pm::instrumentAt(kTrackKick, 0)->id);
  r.run(200);
  r.ui.setPadJob(ui::PadJob::Drums);
  const int km = pm::instrumentAt(kTrackKick, 0)->pressAssign().macro;
  const int kbase = heard(kTrackKick, km);
  r.ui.padDown(0, 0.8f, r.now());   // kick
  r.ui.padDown(1, 0.8f, r.now());   // snare (CLASSIC)
  r.ui.padPressure(0, 0.5f, r.now());
  r.ui.padPressure(1, 1.0f, r.now());
  r.run(100);
  it::check(km >= 0 && heard(kTrackKick, km) > kbase && r.ui.pressOffset(kTrackSnare) == 0,
            "pressure: drums: the kick pad moves the kick's press macro (%d -> %d), the snare's pad nothing", kbase,
            heard(kTrackKick, km));
  r.ui.padUp(0, r.now());
  r.ui.padUp(1, r.now());
  r.run(300);
  it::check(heard(kTrackKick, km) == kbase, "pressure: drums: back (%d)", heard(kTrackKick, km));
  // recording: pressure writes nothing into the notes
  r.ui.setPadJob(ui::PadJob::Notes);
  select(kTrackBass);
  r.run(40);
  r.tap(ui::kKEnter);
  r.tap(ui::kKSpace);
  r.run(100);
  r.ui.padDown(0, 0.787f, r.now());
  for (int i = 0; i < 40; ++i) {
    r.ui.padPressure(0, 0.9f, r.now());
    r.run(20);
  }
  r.ui.padUp(0, r.now());
  r.run(100);
  r.tap(ui::kKSpace);
  r.tap(ui::kKEnter);
  r.run(60);
  int own = 0, notes = 0;
  for (int i = 0; i < 16; ++i)
    if (r.step(kTrackBass, i).on()) {
      ++notes;
      if (r.step(kTrackBass, i).hasMacro(1)) ++own;
    }
  it::check(notes >= 1 && own == 0, "pressure: not recorded (%d notes, %d with a BITE value)", notes, own);
}

void testWithTilt() {
  std::printf("pressure and tilt at once\n");
  start();
  uih::Rig& r = rig();
  r.ui.setPadJob(ui::PadJob::Notes);
  // two macros: tilt X moves OPEN, pressure BITE
  r.tiltX = 0.25f;   // +40
  r.down(ui::kKG0);
  r.run(300);
  r.ui.padDown(0, 0.787f, r.now());
  r.ui.padPressure(0, 1.0f, r.now());
  r.run(100);
  it::check(heard(kTrackBass, 0) == 140 && heard(kTrackBass, 1) == 220, "two macros: tilt OPEN +40, pressure BITE +160 (%d %d)",
            heard(kTrackBass, 0), heard(kTrackBass, 1));
  // one macro: tilt Y also moves BITE (the default Y): they add
  r.tiltY = -0.25f;   // -40
  r.run(100);
  it::check(heard(kTrackBass, 1) == 60 + 160 - 40, "one macro: tilt Y -40 + pressure +160 add (%d)", heard(kTrackBass, 1));
  r.ui.padUp(0, r.now());
  r.run(300);
  it::check(heard(kTrackBass, 1) == 20 && heard(kTrackBass, 0) == 140, "pressure released: tilt's part stays (%d %d)",
            heard(kTrackBass, 1), heard(kTrackBass, 0));
  r.ui.padDown(0, 0.787f, r.now());
  r.ui.padPressure(0, 1.0f, r.now());
  r.run(100);
  it::check(heard(kTrackBass, 1) == 180, "pressure again (%d)", heard(kTrackBass, 1));
  r.up(ui::kKG0);
  r.run(300);
  it::check(heard(kTrackBass, 1) == 220 && heard(kTrackBass, 0) == 100, "tilt released: pressure's part stays (%d %d)",
            heard(kTrackBass, 1), heard(kTrackBass, 0));
  r.ui.padUp(0, r.now());
  r.run(300);
  it::check(heard(kTrackBass, 1) == 60 && heard(kTrackBass, 0) == 100, "both released: the bases (%d %d)", heard(kTrackBass, 1),
            heard(kTrackBass, 0));
}

void testPadVelocity() {
  std::printf("a hit's velocity on the instrument\n");
  start();
  uih::Rig& r = rig();
  r.ui.setPadJob(ui::PadJob::Notes);
  r.ui.padDown(0, 1.0f, r.now());
  r.run(40);
  it::check(heard(kTrackBass, 0) == 228, "a full hit: OPEN 100 + 127.5 (%d)", heard(kTrackBass, 0));
  r.ui.padUp(0, r.now());
  r.run(40);
  r.ui.padDown(1, 0.3f, r.now());
  r.run(40);
  const int want = int(100.0f + g_ov.set.velOffset(0, ui::padVelocity(0.3f)) + 0.5f);
  it::check(heard(kTrackBass, 0) == want, "a soft hit: lower (%d, want %d)", heard(kTrackBass, 0), want);
  r.ui.padUp(1, r.now());
  r.run(40);
}

void testRate() {
  std::printf("rate\n");
  start();
  uih::Rig& r = rig();
  r.ui.setPadJob(ui::PadJob::Notes);
  r.ui.padDown(0, 0.787f, r.now());
  r.run(20);
  const uint32_t peak0 = r.eng.queuePeak();
  for (int i = 0; i < 400; ++i) {   // 1 kHz for 400 ms, a slow sweep up
    r.ui.padPressure(0, 0.1f + 0.8f * float(i) / 399.0f, r.now());
    r.run(1);
  }
  r.run(50);
  it::check(r.ui.droppedCommands() == 0, "rate: nothing dropped");
  it::check(r.eng.queuePeak() <= peak0 + 8, "rate: the queue never filled (peak %u)", unsigned(r.eng.queuePeak()));
  const int want = 60 + ui::pressOffset16(0.9f, 1) / kOffsetScale;
  it::check(heard(kTrackBass, 1) == want, "rate: the last value arrived (%d, want %d)", heard(kTrackBass, 1), want);
  r.ui.padUp(0, r.now());
  r.run(300);
}

}  // namespace

int main() {
  testDrums();
  testVelocityMap();
  testRecord();
  testNotes();
  testPressure();
  testWithTilt();
  testPadVelocity();
  testRate();
  pm::setLibraryOverlay(nullptr);
  return it::finish("itest_pads");
}
