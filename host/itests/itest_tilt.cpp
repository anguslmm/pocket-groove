// System test: tilt (docs/UX.md 10.3 "Tilt"; engine.h "TILT OFFSET").
//
// Engine (SetMacroOffset):
//   additive    an offset over a still base: heard = base + offset, published to the
//               page, the voice gets the instrument evaluated there; the Project (the
//               byte, the derived parameters) is untouched; clamped at 0 and 255
//   glide back  the offset's ramp back to 0 arrives when it says, the voice is on the
//               stored bytes again (the 7-bit path)
//   motion      over stage C motion: heard = clamp(motion + offset) at every quantum
//               of two bars, the Project byte for byte as without the offset
//   recording   record on, playing, an offset with the hold flag: the macro is live,
//               the notes that start take the heard value; the release ends live at
//               once (not a bar later) and the recorded motion plays; over existing
//               motion the hand takes the macro where the motion has it
//   unused      an instrument project with motion, key moves and a recording pass
//               renders bit for bit what the engine before tilt rendered (a hash
//               computed with that engine, see kBaselineHash); offsets of 0 change
//               nothing
// UI (the rig's G0 is the firmware's: host/common/ui_harness.h):
//   tap         a G0 tap with the device tilted moves nothing, no toast
//   hold        after the tap time the macros follow; the stored positions stay;
//               the release glides back
//   sensitivity full range = 160 of 255: from the middle an end at 0.8
//   cancel      G0 + - (volume): glides back, nothing until the next press, the key
//               never reaches the Ui
//   classic     a CLASSIC track: nothing moves, one toast per hold
//   track       the track is taken when the tilt engages
//   record      record on and playing: the notes take the tilted value, one undo unit
//   explain     in explain mode G0 explains itself
// The built-in instruments: every tilt axis names a macro that moves a continuous
// parameter, and the table lists what each set's axes do.
#include <cmath>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "pg/macro/macro.h"
#ifndef PG_TILT_BASELINE
#include "common/ui_harness.h"
#include "pg/ui/keys.h"
#endif

using namespace pg;
namespace pm = pg::macro;

namespace {

// The hash of referenceRender() with the engine before tilt (git 25eaa9f, built with
// -DPG_TILT_BASELINE against that tree's libraries). Same compiler and flags as ctest.
constexpr uint64_t kBaselineHash = 0xd62fc9a71d872dd7ull;

// One engine per process (its state is static): the UI rig's instrument for everything.
Instrument& inst() {
#ifdef PG_TILT_BASELINE
  static Instrument i;   // ~70 KB: static
  return i;
#else
  return uih::Rig::instrument();
#endif
}
Engine& eng() { return inst().engine(); }
void post(const Command& c) {
  while (!eng().post(c)) {
    int16_t b[2 * kQuantum];
    inst().process(b, kQuantum);
  }
}
void quanta(int n, it::Stats* st = nullptr) {
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) {
    inst().process(buf, kQuantum);
    if (st) st->add(buf, kQuantum);
  }
}
int msQ(float ms) { return int(ms * 0.001f * 44100.0f / float(kQuantum) + 0.5f); }

const pm::MacroSet* findSet(int t, const char* name) {
  for (int i = 0; i < pm::instrumentCount(t); ++i)
    if (std::strcmp(pm::instrumentAt(t, i)->name, name) == 0) return pm::instrumentAt(t, i);
  return nullptr;
}

Command step(int pat, int t, int idx, int note, int m0 = -1, int m1 = -1) {
  Command c;
  std::memset(&c, 0, sizeof(c));
  c.type = CmdType::SetStep;
  c.a = uint8_t(pat);
  c.b = uint8_t(t);
  c.c = uint8_t(idx);
  c.step = kDefaultStep;
  c.step.setFlags(kStepOn);
  c.step.note = uint8_t(note);
  c.step.gate = 3;
  if (m0 >= 0) c.step.setMacroValue(0, m0);
  if (m1 >= 0) c.step.setMacroValue(1, m1);
  return c;
}
Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}

// A fresh engine with ACID on the bass (OPEN, BITE at pos0 / pos1) and HANDS on the lead.
void setupInstruments(int pos0 = 102, int pos1 = 0) {
  inst().init(44100.0f, false);
  const uint8_t bp[4] = {uint8_t(pos0), uint8_t(pos1), 0, 255};
  post(setInstrumentCommand(kTrackBass, findSet(kTrackBass, "ACID")->id, bp));
  const uint8_t lp[4] = {80, 0, 0, 0};
  post(setInstrumentCommand(kTrackLead, findSet(kTrackLead, "HANDS")->id, lp));
  post(cmd(CmdType::SetGlobal, kGTempo, 0, 130));
  quanta(4);
}

// The reference render: motion on bass and lead, a key move of a base, a recording pass
// with a key, Stop, Play again. Only commands the engine had before tilt (and, with
// zeroOffsets, offsets of 0 to every macro of both tracks).
uint64_t referenceRender(bool zeroOffsets) {
  setupInstruments();
  for (int i = 0; i < 16; i += 2) post(step(0, kTrackBass, i, 33 + (i % 5) * 2, i == 0 ? 20 : (i == 8 ? 230 : -1)));
  post(step(0, kTrackLead, 0, 69, 40, 0));
  post(step(0, kTrackLead, 8, 72, 220, 90));
  for (int i = 0; i < 16; i += 4) post(step(0, kTrackKick, i, 36));
#ifndef PG_TILT_BASELINE
  if (zeroOffsets)
    for (int t : {int(kTrackBass), int(kTrackLead)})
      for (int m = 0; m < kTrackMacros; ++m) post(macroOffsetCommand(t, m, 0, m & 1 ? 0 : 30, false));
#else
  (void)zeroOffsets;
#endif
  it::Stats st;
  post(cmd(CmdType::Play));
  quanta(2000, &st);
  post(setMacroCommand(kTrackBass, 1, 180));   // a key move of BITE (no motion: heard)
  post(setMacroCommand(kTrackBass, 0, 60));    // OPEN has motion: a new base only
  quanta(2000, &st);
  post(cmd(CmdType::SetRecord, 0, 0, 1));
  for (int k = 0; k < 30; ++k) {               // a held key on the lead's PUSH, recorded
    post(setMacroCommand(kTrackLead, 1, uint8_t(100 + 4 * k)));
    quanta(80, &st);
  }
  post(cmd(CmdType::SetRecord, 0, 0, 0));
  quanta(3000, &st);
  post(cmd(CmdType::Stop));
  quanta(500, &st);
  post(cmd(CmdType::Play));
  quanta(2000, &st);
  return st.hash;
}

#ifndef PG_TILT_BASELINE

uint8_t g_def[kNumTracks][kParamsPerTrack];
pm::ParamTable g_pt;
void tables() {
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) {
      g_pt.info[t][p] = eng().trackParamInfo(t, p);
      g_def[t][p] = g_pt.info[t][p] ? g_pt.info[t][p]->def : 0;
    }
}
void derived(int t, const pm::MacroSet* s, const uint8_t* pos, uint8_t* out) {
  float f[4];
  for (int m = 0; m < 4; ++m) f[m] = pm::posFromByte(pos[m]);
  pm::evaluate(*s, g_pt, f, g_def[t], out);
}
float heard(int t, int m) { return engine_detail::macroPosition(t, m); }
Command offset(int t, int m, float units, int rampMs, bool hold = false) {
  return macroOffsetCommand(t, m, int(std::lround(units * float(kOffsetScale))), uint16_t(rampMs), hold);
}

void testAdditive() {
  std::printf("engine: additive, clamped, glide back\n");
  setupInstruments(102, 0);
  tables();
  const pm::MacroSet* acid = findSet(kTrackBass, "ACID");
  Command n = cmd(CmdType::NoteOn, kTrackBass, 45, 110);
  post(n);
  quanta(50);
  Project before = inst().project();
  post(offset(kTrackBass, 0, 40.0f, 30));
  quanta(msQ(15));
  const float mid = heard(kTrackBass, 0);
  it::check(mid > 105.0f && mid < 140.0f, "additive: half way along the 30 ms ramp (%.2f)", double(mid));
  quanta(msQ(200));
  it::check(heard(kTrackBass, 0) == 142.0f, "additive: heard = base + offset (%.3f)", double(heard(kTrackBass, 0)));
  it::check(eng().status().macroPos[kTrackBass][0] == 142, "additive: published to the page (%d)",
            eng().status().macroPos[kTrackBass][0]);
  it::check(std::memcmp(&before, &inst().project(), sizeof(Project)) == 0, "additive: the Project is untouched");
  uint8_t eff[kParamsPerTrack], want[kParamsPerTrack];
  const uint8_t at142[4] = {142, 0, 0, 255};
  derived(kTrackBass, acid, at142, want);
  engine_detail::effectiveParams(kTrackBass, eff);
  it::check(std::memcmp(eff, want, kSoundParams) == 0, "additive: the voice plays the instrument at 142 (7-bit, still)");
  // clamped
  post(offset(kTrackBass, 0, 200.0f, 0));
  quanta(4);
  it::check(heard(kTrackBass, 0) == 255.0f, "clamp: at most 255 (%.2f)", double(heard(kTrackBass, 0)));
  post(offset(kTrackBass, 0, -400.0f, 0));
  quanta(4);
  it::check(heard(kTrackBass, 0) == 0.0f, "clamp: at least 0 (%.2f)", double(heard(kTrackBass, 0)));
  post(offset(kTrackBass, 0, 40.0f, 0));
  quanta(100);
  // glide back in 150 ms
  post(offset(kTrackBass, 0, 0.0f, 150));
  quanta(msQ(75));
  const float half = heard(kTrackBass, 0);
  it::check(std::fabs(half - 122.0f) < 1.5f, "glide back: half way at half the time (%.2f)", double(half));
  quanta(msQ(76));
  it::check(heard(kTrackBass, 0) == 102.0f, "glide back: arrived at the base in 150 ms (%.3f)", double(heard(kTrackBass, 0)));
  quanta(msQ(120));
  engine_detail::effectiveParams(kTrackBass, eff);
  it::check(std::memcmp(eff, inst().project().tracks[kTrackBass].params, kSoundParams) == 0,
            "glide back: the voice plays the stored bytes again");
  it::check(std::memcmp(&before, &inst().project(), sizeof(Project)) == 0, "glide back: the Project is untouched");
  // a macro the set lacks / no instrument: nothing
  post(offset(kTrackKick, 0, 50.0f, 0));
  quanta(4);
  it::check(heard(kTrackKick, 0) == float(inst().project().tracks[kTrackKick].macro[0]),
            "an offset on a CLASSIC track is ignored (%.2f)", double(heard(kTrackKick, 0)));
}

std::vector<float> motionRun(float off) {
  setupInstruments(102, 0);
  for (int i = 0; i < 16; ++i) post(step(0, kTrackBass, i, 36, i == 0 ? 20 : (i == 8 ? 230 : -1)));
  if (off != 0.0f) post(offset(kTrackBass, 0, off, 0));
  post(cmd(CmdType::Play));
  std::vector<float> v;
  const int bar = int(16.0f * 60.0f / 130.0f / 4.0f * 44100.0f / float(kQuantum));
  for (int q = 0; q < 2 * bar; ++q) {
    quanta(1);
    v.push_back(heard(kTrackBass, 0));
  }
  return v;
}

void testOverMotion() {
  std::printf("engine: over stage C motion\n");
  const std::vector<float> a = motionRun(0.0f);
  const Project pa = inst().project();
  for (float off : {30.0f, -45.0f}) {
    const std::vector<float> b = motionRun(off);
    float worst = 0.0f;
    bool moved = false;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const float want = std::fmin(255.0f, std::fmax(0.0f, a[i] + off));
      worst = std::fmax(worst, std::fabs(b[i] - want));
      if (i && a[i] != a[i - 1]) moved = true;
    }
    it::check(moved && a.size() == b.size() && worst < 1e-3f, "motion %+.0f: heard = clamp(motion + offset) every quantum (worst %.5f)",
              double(off), double(worst));
    it::check(std::memcmp(&pa, &inst().project(), sizeof(Project)) == 0, "motion %+.0f: the Project as without the offset",
              double(off));
  }
}

void testRecording() {
  std::printf("engine: recording\n");
  // plain base (no motion): notes on every step
  setupInstruments(100, 0);
  for (int i = 0; i < 16; ++i) post(step(0, kTrackBass, i, 36));
  post(cmd(CmdType::SetRecord, 0, 0, 1));
  post(cmd(CmdType::Play));
  quanta(20);
  post(offset(kTrackBass, 0, 60.0f, 0, true));
  quanta(2);
  it::check((eng().status().live[kTrackBass] & 1u) != 0, "record: the held tilt makes OPEN live");
  const int bar = int(16.0f * 60.0f / 130.0f / 4.0f * 44100.0f / float(kQuantum));
  quanta(bar + 10);
  int written = 0, right = 0;
  for (int i = 0; i < 16; ++i) {
    const Step& s = inst().project().patterns[0].tracks[kTrackBass].steps[i];
    if (s.hasMacro(0)) {
      ++written;
      if (std::abs(int(s.macroValue(0)) - 160) <= 3) ++right;
    }
    it::check(!s.hasMacro(1), "record: only the tilted macro is written (step %d)", i + 1);
  }
  it::check(written == 16 && right == 16, "record: every note that started took base + offset (%d written, %d at 160)",
            written, right);
  it::check(inst().project().tracks[kTrackBass].macro[0] == 100, "record: the base byte stays");
  post(offset(kTrackBass, 0, 0.0f, 150, false));
  quanta(2);
  it::check((eng().status().live[kTrackBass] & 1u) == 0, "record: the release ends live at once");
  it::check((eng().status().motion[kTrackBass] & 1u) != 0, "record: the recorded motion plays");
  quanta(bar);
  it::check(std::fabs(heard(kTrackBass, 0) - 162.0f) < 3.0f, "record: after the release OPEN plays the notes (%.2f)",
            double(heard(kTrackBass, 0)));
  // over existing motion: the hand takes the macro where the motion has it
  setupInstruments(100, 0);
  for (int i = 0; i < 16; ++i) post(step(0, kTrackBass, i, 36, i == 0 ? 20 : (i == 8 ? 230 : -1)));
  post(cmd(CmdType::SetRecord, 0, 0, 1));
  post(cmd(CmdType::Play));
  quanta(bar + bar / 4);           // a quarter into the second bar: OPEN on its way 20 -> 230
  const float where = heard(kTrackBass, 0);
  post(offset(kTrackBass, 0, 10.0f, 0, true));
  quanta(bar / 2);
  const Step& s8 = inst().project().patterns[0].tracks[kTrackBass].steps[8];
  it::check(where > 60.0f && where < 190.0f && std::abs(int(s8.macroValue(0)) - int(where + 10.0f + 0.5f)) <= 3,
            "record over motion: step 9 took where the motion was + 10 (%d, motion was at %.1f)", int(s8.macroValue(0)),
            double(where));
  it::check(std::fabs(heard(kTrackBass, 0) - (where + 10.0f)) < 0.5f, "record over motion: heard stays there + offset (%.2f)",
            double(heard(kTrackBass, 0)));
  // a key move within the last bar keeps it live past the release (the keys' rule)
  post(setMacroCommand(kTrackBass, 0, 90));
  quanta(4);
  post(offset(kTrackBass, 0, 0.0f, 150, false));
  quanta(4);
  it::check((eng().status().live[kTrackBass] & 1u) != 0, "record: a key moved it within the bar: still live after the release");
  quanta(bar + 4);
  it::check((eng().status().live[kTrackBass] & 1u) == 0, "record: ... until the key's bar is over");
}

void testUnused() {
  std::printf("engine: bit-identical when unused\n");
  const uint64_t h = referenceRender(false);
  std::printf("  reference hash 0x%016llx (baseline 0x%016llx)\n", (unsigned long long)h, (unsigned long long)kBaselineHash);
  it::check(h == kBaselineHash, "the reference render is bit for bit the engine's before tilt");
  const uint64_t z = referenceRender(true);
  it::check(z == h, "offsets of 0 change nothing (0x%016llx)", (unsigned long long)z);
}

// ---- UI --------------------------------------------------------------------------------
uih::Rig& rig() {
  static uih::Rig r;
  return r;
}
int uiHeard(int m) { return rig().eng.status().macroPos[kTrackBass][m]; }

void startUi() {
  uih::Rig& r = rig();
  r.devMode = false;
  r.start(false);   // an empty project (CLASSIC): ACID on the bass, HANDS on the lead
  r.run(100);
  r.ui.setInstrument(kTrackBass, findSet(kTrackBass, "ACID")->id);
  r.ui.setInstrument(kTrackLead, findSet(kTrackLead, "HANDS")->id);
  r.run(100);
  r.ui.setMacro(kTrackBass, 0, 128);
  r.ui.setMacro(kTrackBass, 1, 64);
  r.run(300);
}

void testUiTap() {
  std::printf("ui: a tap\n");
  startUi();
  uih::Rig& r = rig();
  r.tiltX = 1.0f;
  r.tiltY = -1.0f;
  r.down(ui::kKG0);
  bool moved = false;
  for (int i = 0; i < 12; ++i) {   // 120 ms
    r.run(10);
    moved |= uiHeard(0) != 128 || uiHeard(1) != 64;
  }
  r.up(ui::kKG0);
  for (int i = 0; i < 30; ++i) {
    r.run(10);
    moved |= uiHeard(0) != 128 || uiHeard(1) != 64;
  }
  it::check(!moved, "tap: nothing moved (%d %d)", uiHeard(0), uiHeard(1));
  it::check(r.ui.toastText()[0] == 0, "tap: no toast ('%s')", r.ui.toastText());
  it::check(!r.ui.tiltHeld() && !r.ui.tiltEngaged(), "tap: over");
}

void testUiHold() {
  std::printf("ui: hold, sensitivity, release\n");
  startUi();
  uih::Rig& r = rig();
  const Project before = *r.proj;
  r.tiltX = 0.5f;
  r.tiltY = -0.25f;
  r.down(ui::kKG0);
  r.run(400);
  it::check(r.ui.tiltEngaged() && r.ui.tiltTrack() == kTrackBass, "hold: engaged on the bass");
  it::check(uiHeard(0) == 208 && uiHeard(1) == 24, "hold: OPEN 128 + 0.5 x 160, BITE 64 - 0.25 x 160 (%d %d)",
            uiHeard(0), uiHeard(1));
  it::check(r.ui.shownMacro(kTrackBass, 0) == 208, "hold: the page's handle is where it is heard (%d)",
            r.ui.shownMacro(kTrackBass, 0));
  it::check(r.ui.macroValue(kTrackBass, 0) == 128 && std::memcmp(&before, r.proj, sizeof(Project)) == 0,
            "hold: the stored positions and the Project stay");
  r.tiltX = 0.8f;
  r.run(100);
  it::check(uiHeard(0) == 255, "sensitivity: from the middle the top at 0.8 of the range (%d)", uiHeard(0));
  r.tiltX = -0.8f;
  r.run(100);
  it::check(uiHeard(0) == 0, "sensitivity: and the bottom (%d)", uiHeard(0));
  r.tiltX = -0.4f;
  r.run(100);
  it::check(uiHeard(0) == 64, "sensitivity: half of that, a quarter of the travel (%d)", uiHeard(0));
  r.up(ui::kKG0);
  r.run(80);
  it::check(uiHeard(0) > 64 && uiHeard(0) < 128, "release: gliding back (%d)", uiHeard(0));
  r.run(200);
  it::check(uiHeard(0) == 128 && uiHeard(1) == 64, "release: back at the base (%d %d)", uiHeard(0), uiHeard(1));
  it::check(r.ui.shownMacro(kTrackBass, 0) == 128, "release: the handle at the base");
  it::check(std::memcmp(&before, r.proj, sizeof(Project)) == 0, "release: nothing stored");
}

void testUiCancel() {
  std::printf("ui: volume cancel\n");
  startUi();
  uih::Rig& r = rig();
  const int focusVal = r.ui.paramValue(r.ui.focus().track, r.ui.focus().index);
  r.tiltX = 0.5f;
  r.down(ui::kKG0);
  r.run(300);
  it::check(uiHeard(0) == 208, "cancel: tilting first (%d)", uiHeard(0));
  r.down(ui::kKMinus);
  r.run(300);
  it::check(uiHeard(0) == 128 && !r.ui.tiltHeld() && !r.ui.tiltEngaged(), "cancel: the volume key glides it back (%d)",
            uiHeard(0));
  r.tiltX = -0.5f;
  r.run(200);
  it::check(uiHeard(0) == 128, "cancel: no tilt for the rest of the hold (%d)", uiHeard(0));
  r.up(ui::kKMinus);
  r.up(ui::kKG0);
  r.run(100);
  it::check(r.ui.paramValue(r.ui.focus().track, r.ui.focus().index) == focusVal, "cancel: '-' never reached the Ui");
  r.down(ui::kKG0);
  r.run(300);
  it::check(uiHeard(0) == 48, "cancel: the next press tilts again (%d)", uiHeard(0));
  r.up(ui::kKG0);
  r.run(300);
}

void testUiClassicTrackRecord() {
  std::printf("ui: CLASSIC, the track, recording, explain\n");
  uih::Rig& r = rig();
  r.devMode = false;
  r.start(true);   // the mockup project: CLASSIC tracks
  r.run(100);
  it::check(r.ui.instrumentOf(r.ui.selTrack()) == nullptr, "classic: the selected track is CLASSIC");
  const Project before = *r.proj;
  r.tiltX = 0.9f;
  r.down(ui::kKG0);
  r.run(250);
  it::check(std::strstr(r.ui.toastText(), "TILT") != nullptr, "classic: one toast ('%s')", r.ui.toastText());
  r.run(3200);
  it::check(r.ui.toastText()[0] == 0, "classic: only one per hold ('%s')", r.ui.toastText());
  r.up(ui::kKG0);
  r.run(100);
  it::check(std::memcmp(&before, r.proj, sizeof(Project)) == 0, "classic: nothing changed");

  // the track is taken when the tilt engages
  startUi();
  r.tiltX = 0.5f;
  r.down(ui::kKG0);
  r.run(300);
  r.tap(ui::kKPad0 + 7);   // select the lead while tilting
  r.run(200);
  it::check(r.ui.selTrack() == kTrackLead && uiHeard(0) == 208, "track: the bass keeps moving (%d)", uiHeard(0));
  it::check(r.eng.status().macroPos[kTrackLead][0] == r.ui.macroValue(kTrackLead, 0), "track: the lead is untouched");
  r.up(ui::kKG0);
  r.run(300);
  r.tap(ui::kKPad0 + 6);
  r.run(100);

  // recording through the UI: a bass line, record on, playing, tilt
  for (int i = 0; i < 16; ++i) {
    Command c = step(r.ui.editPattern(), kTrackBass, i, 36);
    r.eng.post(c);
  }
  r.run(50);
  r.tap(ui::kKSpace);
  r.tap(ui::kKEnter);
  r.run(200);
  r.tiltX = 0.25f;
  r.down(ui::kKG0);
  r.run(2500);
  r.up(ui::kKG0);
  r.run(100);
  int got = 0;
  for (int i = 0; i < 16; ++i)
    if (r.step(kTrackBass, i).hasMacro(0) && std::abs(int(r.step(kTrackBass, i).macroValue(0)) - 168) <= 3) ++got;
  it::check(got >= 12, "record: the notes took the tilted value (%d of 16 at 168)", got);
  it::check(r.ui.undoAvailable(), "record: one undo unit");
  r.tap(ui::kKEnter);
  r.tap(ui::kKSpace);
  r.run(200);

  // explain mode: G0 explains itself, no tilt
  r.tap(ui::kKSlash);
  r.run(100);
  r.down(ui::kKG0);
  r.run(300);
  char title[40];
  const char *l1, *l2;
  r.ui.explainText(title, &l1, &l2);
  it::check(r.ui.explainKey() == ui::kKG0 && std::strstr(title, "G0") && !r.ui.tiltEngaged(), "explain: '%s' / %s", title, l1);
  r.up(ui::kKG0);
  r.tap(ui::kKSlash);
  r.run(100);
}

// Every built-in instrument's tilt axes: the macro exists and moves a continuous
// parameter (what it moves on a held note); the table for docs/STATE.md.
void testBuiltins() {
  std::printf("built-in instruments: what tilt moves\n");
  tables();
  const char* rn[4] = {"fast", "smooth", "fine", "wide"};
  for (int i = 0; i < pm::builtinCount(); ++i) {
    const pm::MacroSet& s = pm::builtinAt(i);
    char line[160];
    int n = std::snprintf(line, sizeof(line), "  %-5s %-7s", pm::trackName(s.track), s.name);
    for (int a = 0; a < 2; ++a) {
      const pm::TiltAssign ta = s.tiltAssign(a);
      if (ta.macro < 0) {
        n += std::snprintf(line + n, sizeof(line) - size_t(n), " %c: -                ", a ? 'Y' : 'X');
        continue;
      }
      float pos[4];
      for (int m = 0; m < 4; ++m) pos[m] = m < s.numMacros ? s.macros[m].start() : 0.0f;
      // the list the set is evaluated against: its voice kind's (`voice perc sampler`)
      const ParamInfo* kindScratch[kParamsPerTrack];
      const pm::TrackParams kp = pm::paramsFor(s, g_pt, kindScratch);
      uint8_t under[kParamsPerTrack];
      for (int p = 0; p < kParamsPerTrack; ++p)
        under[p] = p < kSoundParams ? (kp[p] ? kp[p]->def : 0) : g_def[s.track][p];
      if (s.voice == kVoiceNative) std::memcpy(under, g_def[s.track], sizeof(under));
      float jp[8];
      uint8_t jq[8];
      const int jumps = pm::enumJumps(s, g_pt, ta.macro, pos, under, jp, jq, 8);
      // a continuous parameter it moves: compare the evaluation at both ends
      uint8_t lo[kParamsPerTrack], hi[kParamsPerTrack];
      pos[ta.macro] = 0.0f;
      pm::evaluate(s, g_pt, pos, under, lo);
      pos[ta.macro] = 1.0f;
      pm::evaluate(s, g_pt, pos, under, hi);
      int cont = 0;
      for (int p = 0; p < kParamsPerTrack; ++p)
        if (kp[p] && kp[p]->fmt != ParamFormat::Enum && lo[p] != hi[p]) ++cont;
      it::check(cont > 0, "%s %s: tilt %c moves %s, which changes a continuous parameter", pm::trackName(s.track), s.name,
                a ? 'Y' : 'X', s.macros[ta.macro].name);
      it::check(jumps <= 4, "%s %s: tilt %c's %s jumps %d times", pm::trackName(s.track), s.name, a ? 'Y' : 'X',
                s.macros[ta.macro].name, jumps);
      n += std::snprintf(line + n, sizeof(line) - size_t(n), " %c: %-6s %-6s j%d", a ? 'Y' : 'X', s.macros[ta.macro].name,
                         rn[ta.response & 3], jumps);
    }
    std::printf("%s\n", line);
  }
}

#endif  // PG_TILT_BASELINE

}  // namespace

int main() {
#ifdef PG_TILT_BASELINE
  std::printf("0x%016llx\n", (unsigned long long)referenceRender(false));
  return 0;
#else
  testAdditive();
  testOverMotion();
  testRecording();
  testUnused();
  testUiTap();
  testUiHold();
  testUiCancel();
  testUiClassicTrackRecord();
  testBuiltins();
  return it::finish("itest_tilt");
#endif
}
