// System test: velocity as a source inside an instrument (engine.h "VELOCITY";
// docs/MACROLAB.md "Velocity and pressure"). A test set laid over the library:
// VT on the bass, `vel OPEN 0.5` (OPEN: cutoff), BITE (resonance) without a vel line.
//
//   live        a live note's velocity moves OPEN for that note: heard = base +
//               velOffset (100: nothing, 127: +127.5, 1: clamped at 0), the voice plays
//               the instrument there (7-bit path), BITE and the Project untouched
//   steps       sequenced steps: at every note's quantum OPEN is base + its velocity's
//               offset; ratchet hits too; it holds until the next note
//   motion      over stage C motion: heard = clamp(motion + the last note's offset) at
//               every quantum of two bars, the motion's own values as without velocity
//   tilt        with a tilt offset at once: heard = base + tilt + velocity
//   moving      a note while a key move glides OPEN (the fractional path): the jump is
//               there in the note's own quantum
//   recording   motion recording writes what the hand did, not the velocity's offset;
//               the recorded step keeps the note's velocity
//   unused      the same set without its vel line renders bit for bit what VT renders
//               with every note at 100; a set without vel lines ignores velocity
//               (macro positions do not move)
#include <cmath>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "pg/macro/macro.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;
pm::ParamTable g_pt;
uint8_t g_def[kNumTracks][kParamsPerTrack];

struct Overlay final : pm::LibraryOverlay {
  pm::MacroSet set;
  const pm::MacroSet* find(uint16_t id) const override { return id == set.id ? &set : nullptr; }
  int extraCount(int track) const override { return track == set.track ? 1 : 0; }
  const pm::MacroSet* extra(int track, int i) const override { return track == set.track && i == 0 ? &set : nullptr; }
};
Overlay g_ov;

const char kVT[] =
    "voice bass\nname VT\n"
    "vel OPEN 0.5\n"
    "macro OPEN\n  cutoff 0:10 1:120\n"
    "macro BITE\n  resonance 0:0 1:120\n";
const char kVTPlain[] =   // the same without the vel line (same name: same id)
    "voice bass\nname VT\n"
    "macro OPEN\n  cutoff 0:10 1:120\n"
    "macro BITE\n  resonance 0:0 1:120\n";

Engine& eng() { return g_inst.engine(); }
void quanta(int n, it::Stats* st = nullptr) {
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) {
    g_inst.process(buf, kQuantum);
    if (st) st->add(buf, kQuantum);
  }
}
void post(const Command& c) {
  while (!eng().post(c)) quanta(1);
}
int msQ(float ms) { return int(ms * 0.001f * 44100.0f / float(kQuantum) + 0.5f); }
Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}
Command step(int t, int idx, int note, int vel, int m0 = -1, int ratchets = 1) {
  Command c = cmd(CmdType::SetStep, 0, t, idx);
  c.step = kDefaultStep;
  c.step.setFlags(kStepOn | ((ratchets - 1) << kStepRatchetShift));
  c.step.note = uint8_t(note);
  c.step.vel = uint8_t(vel);
  c.step.gate = 3;
  if (m0 >= 0) c.step.setMacroValue(0, m0);
  return c;
}
float heard(int m) { return engine_detail::macroPosition(kTrackBass, m); }
float clamp255(float v) { return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); }

bool load(const char* text) {
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) {
      g_pt.info[t][p] = eng().trackParamInfo(t, p);
      g_def[t][p] = g_pt.info[t][p] ? g_pt.info[t][p]->def : 0;
    }
  pm::ParseError e;
  if (!pm::parse(text, std::strlen(text), g_pt, &g_ov.set, &e)) {
    std::fprintf(stderr, "parse: line %d: %s\n", e.line, e.msg);
    return false;
  }
  pm::setLibraryOverlay(&g_ov);
  return true;
}

// A fresh engine, the overlay's set on the bass at OPEN = open, BITE = 40.
void setup(const char* text, int open = 100) {
  g_inst.init(44100.0f, false);
  it::check(load(text), "the test set parses");
  const uint8_t p[4] = {uint8_t(open), 40, 0, 0};
  post(setInstrumentCommand(kTrackBass, g_ov.set.id, p));
  post(cmd(CmdType::SetGlobal, kGTempo, 0, 120));
  quanta(8);
}
void derived(const uint8_t* pos, uint8_t* out) {
  float f[4];
  for (int m = 0; m < 4; ++m) f[m] = pm::posFromByte(pos[m]);
  pm::evaluate(g_ov.set, g_pt, f, g_def[kTrackBass], out);
}
float off(int vel) { return g_ov.set.velOffset(0, vel); }

void testLive() {
  std::printf("live notes\n");
  setup(kVT);
  const Project before = g_inst.project();
  struct Case { int vel; float want; } cases[] = {
      {100, 100.0f}, {127, 227.5f}, {64, 100.0f + off(64)}, {1, 0.0f}, {110, 100.0f + off(110)}, {100, 100.0f}};
  for (const Case& c : cases) {
    post(cmd(CmdType::NoteOn, kTrackBass, 45, c.vel));
    quanta(1);
    it::check(std::fabs(heard(0) - c.want) < 1e-3f, "live vel %d: OPEN heard at %.2f (want %.2f)", c.vel,
              double(heard(0)), double(c.want));
    it::check(heard(1) == 40.0f, "live vel %d: BITE has no vel line: stays (%.2f)", c.vel, double(heard(1)));
    quanta(20);
    it::check(std::fabs(heard(0) - c.want) < 1e-3f, "live vel %d: it holds while the note plays (%.2f)", c.vel,
              double(heard(0)));
    uint8_t eff[kParamsPerTrack], want[kParamsPerTrack];
    const uint8_t pos[4] = {uint8_t(int(c.want + 0.5f)), 40, 0, 0};
    derived(pos, want);
    engine_detail::effectiveParams(kTrackBass, eff);
    it::check(std::memcmp(eff, want, kSoundParams) == 0, "live vel %d: the voice plays the instrument at OPEN %d",
              c.vel, int(pos[0]));
    it::check(eng().status().macroPos[kTrackBass][0] == pos[0], "live vel %d: published to the page (%d)", c.vel,
              eng().status().macroPos[kTrackBass][0]);
    post(cmd(CmdType::NoteOff, kTrackBass, 45));
    quanta(4);
    it::check(std::fabs(heard(0) - c.want) < 1e-3f, "live vel %d: the note-off changes nothing", c.vel);
  }
  // back at 100: the stored bytes, exactly
  uint8_t eff[kParamsPerTrack];
  engine_detail::effectiveParams(kTrackBass, eff);
  it::check(std::memcmp(eff, g_inst.project().tracks[kTrackBass].params, kSoundParams) == 0,
            "live: a note at 100 plays the stored bytes again");
  it::check(std::memcmp(&before, &g_inst.project(), sizeof(Project)) == 0, "live: the Project is untouched");
}

void testSteps() {
  std::printf("sequenced steps\n");
  setup(kVT);
  const int vels[8] = {127, 40, 100, 90, 1, 127, 64, 110};
  for (int i = 0; i < 8; ++i) post(step(kTrackBass, 2 * i, 36, vels[i], -1, i == 3 ? 3 : 1));
  post(cmd(CmdType::Play));
  uint8_t trig = eng().status().trig[kTrackBass];
  int seen = 0, ok = 0, held = 0, heldOk = 0;
  for (int q = 0; q < 20000 && seen < 16; ++q) {
    quanta(1);
    const EngineStatus st = eng().status();
    const float want = clamp255(100.0f + off(vels[(st.step[kTrackBass] / 2) & 7]));
    if (st.trig[kTrackBass] != trig) {
      trig = st.trig[kTrackBass];
      ++seen;
      if (std::fabs(heard(0) - want) < 1e-3f) ++ok;
      else std::fprintf(stderr, "  step %d: heard %.2f want %.2f\n", st.step[kTrackBass] + 1, double(heard(0)), double(want));
    } else if (seen) {
      ++held;
      if (std::fabs(heard(0) - want) < 1e-3f) ++heldOk;
    }
  }
  it::check(seen >= 16 && ok == seen, "steps: OPEN is base + the note's offset in the note's quantum (%d of %d)", ok, seen);
  it::check(held > 100 && heldOk == held, "steps: and holds until the next note (%d of %d quanta)", heldOk, held);
  post(cmd(CmdType::Stop));
  quanta(4);
}

// OPEN's heard position per quantum over two bars, with motion on OPEN (keyframes at 1 and 9).
std::vector<float> motionRun(bool velocities, std::vector<int>* velAt) {
  setup(kVT);
  const int vels[4] = {127, 30, 100, 80};
  for (int i = 0; i < 16; i += 2)
    post(step(kTrackBass, i, 36, velocities ? vels[(i / 2) & 3] : 100, i == 0 ? 30 : (i == 8 ? 220 : -1)));
  post(cmd(CmdType::Play));
  std::vector<float> v;
  const int bar = int(16.0f * 60.0f / 120.0f / 4.0f * 44100.0f / float(kQuantum));
  int lastVel = 100;
  for (int q = 0; q < 2 * bar; ++q) {
    const uint8_t tr = eng().status().trig[kTrackBass];
    quanta(1);
    const EngineStatus st = eng().status();
    if (st.trig[kTrackBass] != tr) lastVel = velocities ? vels[(st.step[kTrackBass] / 2) & 3] : 100;
    v.push_back(heard(0));
    if (velAt) velAt->push_back(lastVel);
  }
  return v;
}

void testOverMotion() {
  std::printf("over motion\n");
  const std::vector<float> a = motionRun(false, nullptr);
  const Project pa = g_inst.project();
  std::vector<int> vel;
  const std::vector<float> b = motionRun(true, &vel);
  float worst = 0.0f;
  bool moved = false, offs = false;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const float want = clamp255(a[i] + off(vel[i]));
    worst = std::fmax(worst, std::fabs(b[i] - want));
    if (i && a[i] != a[i - 1]) moved = true;
    if (vel[i] != 100) offs = true;
  }
  it::check(moved && offs && a.size() == b.size() && worst < 1e-3f,
            "motion: heard = clamp(motion + the note's offset) every quantum (worst %.5f)", double(worst));
  Project pb = g_inst.project();
  for (int i = 0; i < 16; i += 2) pb.patterns[0].tracks[kTrackBass].steps[i].vel = 100;
  it::check(std::memcmp(&pa, &pb, sizeof(Project)) == 0, "motion: the Project as without velocity (but the notes' vel)");
}

void testTilt() {
  std::printf("with a tilt offset\n");
  setup(kVT, 80);
  post(macroOffsetCommand(kTrackBass, 0, 30 * kOffsetScale, 0, false));
  quanta(4);
  post(cmd(CmdType::NoteOn, kTrackBass, 45, 127));
  quanta(2);
  it::check(std::fabs(heard(0) - (80.0f + 30.0f + 127.5f)) < 1e-3f, "tilt + velocity: they add (%.2f)", double(heard(0)));
  post(cmd(CmdType::NoteOn, kTrackBass, 47, 40));
  quanta(2);
  it::check(std::fabs(heard(0) - (110.0f + off(40))) < 1e-3f, "tilt + a soft note (%.2f)", double(heard(0)));
  post(macroOffsetCommand(kTrackBass, 0, 0, 0, false));
  quanta(2);
  it::check(std::fabs(heard(0) - (80.0f + off(40))) < 1e-3f, "tilt released: the note's offset stays (%.2f)",
            double(heard(0)));
}

void testMoving() {
  std::printf("while a key move glides\n");
  setup(kVT, 60);
  post(setMacroCommand(kTrackBass, 0, 160, uint16_t(msQ(400))));   // a 400 ms glide 60 -> 160
  quanta(msQ(100));
  const float before = heard(0);
  post(cmd(CmdType::NoteOn, kTrackBass, 45, 127));
  quanta(1);
  const float at = heard(0);
  it::check(before > 70.0f && before < 110.0f && std::fabs(at - (before + 127.5f)) < 1.0f,
            "moving: the note's offset is there in its own quantum (%.2f -> %.2f)", double(before), double(at));
  quanta(msQ(500));
  it::check(std::fabs(heard(0) - 255.0f) < 1e-3f, "moving: the glide arrives under it (clamped: %.2f)", double(heard(0)));
  post(cmd(CmdType::NoteOn, kTrackBass, 45, 100));
  quanta(msQ(100));
  it::check(heard(0) == 160.0f, "moving: a note at 100: the base again (%.2f)", double(heard(0)));
  uint8_t eff[kParamsPerTrack];
  engine_detail::effectiveParams(kTrackBass, eff);
  it::check(std::memcmp(eff, g_inst.project().tracks[kTrackBass].params, kSoundParams) == 0,
            "moving: back on the stored bytes (the 7-bit path)");
}

void testRecording() {
  std::printf("motion recording\n");
  setup(kVT, 100);
  for (int i = 0; i < 16; ++i) post(step(kTrackBass, i, 36, i & 1 ? 127 : 20));
  post(cmd(CmdType::SetRecord, 0, 0, 1));
  post(cmd(CmdType::Play));
  quanta(20);
  post(setMacroCommand(kTrackBass, 0, 150, kMacroJump));   // the hand: OPEN live at 150
  const int bar = int(16.0f * 60.0f / 120.0f / 4.0f * 44100.0f / float(kQuantum));
  quanta(bar / 2);
  int written = 0, right = 0;
  for (int i = 0; i < 16; ++i) {
    const Step& s = g_inst.project().patterns[0].tracks[kTrackBass].steps[i];
    if (!s.hasMacro(0)) continue;
    ++written;
    if (std::abs(int(s.macroValue(0)) - 150) <= 3) ++right;
  }
  it::check(written >= 6 && right == written, "recording: the notes took the hand's 150, not + their velocity (%d of %d)",
            right, written);
  // a live note recorded: its velocity goes into the step
  post(cmd(CmdType::NoteOn, kTrackBass, 50, 77));
  quanta(2);
  post(cmd(CmdType::NoteOff, kTrackBass, 50));
  quanta(2);
  bool found = false;
  for (int i = 0; i < 16; ++i) {
    const Step& s = g_inst.project().patterns[0].tracks[kTrackBass].steps[i];
    if (s.note == 50 && s.vel == 77) found = true;
  }
  it::check(found, "recording: a live note keeps its velocity in the step");
  post(cmd(CmdType::Stop));
  post(cmd(CmdType::SetRecord, 0, 0, 0));
  quanta(4);
}

uint64_t renderHash(const char* text, bool velocities) {
  setup(text);
  for (int i = 0; i < 16; ++i) post(step(kTrackBass, i, 36 + (i % 5), velocities ? 1 + (i * 37) % 127 : 100, i == 4 ? 200 : -1));
  for (int i = 0; i < 16; i += 4) post(step(kTrackKick, i, 60, 127));
  it::Stats st;
  post(cmd(CmdType::Play));
  quanta(3000, &st);
  post(setMacroCommand(kTrackBass, 1, 120));
  post(cmd(CmdType::NoteOn, kTrackBass, 40, velocities ? 20 : 100));
  quanta(2000, &st);
  post(cmd(CmdType::NoteOff, kTrackBass, 40));
  quanta(1000, &st);
  return st.hash;
}

void testUnused() {
  std::printf("bit-identical when unused\n");
  const uint64_t plain = renderHash(kVTPlain, false);
  const uint64_t vt100 = renderHash(kVT, false);
  it::check(plain == vt100, "VT with every note at 100 = VT without its vel line (0x%016llx 0x%016llx)",
            (unsigned long long)plain, (unsigned long long)vt100);
  const uint64_t plainVel = renderHash(kVTPlain, true);
  it::check(plainVel != plain, "(the velocities do change the sound: level)");
  // without vel lines velocity moves no macro
  setup(kVTPlain);
  post(cmd(CmdType::NoteOn, kTrackBass, 45, 127));
  quanta(4);
  it::check(heard(0) == 100.0f, "no vel line: OPEN stays at its base (%.2f)", double(heard(0)));
  const uint64_t vtVel = renderHash(kVT, true);
  it::check(vtVel != plainVel, "with the vel line the velocities move OPEN (another render)");
}

}  // namespace

int main() {
  testLive();
  testSteps();
  testOverMotion();
  testTilt();
  testMoving();
  testRecording();
  testUnused();
  pm::setLibraryOverlay(nullptr);
  return it::finish("itest_velocity");
}
