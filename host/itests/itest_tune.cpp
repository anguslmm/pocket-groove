// The track's tune (params.h kMixTune / kMixTuneCents; UX.md 10.3 "Tune"), through
// the real engine, voices and project files:
//   store     a new project is untuned; the bytes survive save / load / sanitize byte
//             for byte; out of range they are clamped; on a track that cannot host a
//             sampler they are zeroed; an older file (version 1 / 2: reserved bytes)
//             reads untuned whatever the bytes held
//   untouched choosing an instrument, moving a macro, the macros' evaluation and
//             combineMixer never write them
//   pitch     PERC on TAPE and LEAD on PLUCK with a 441 Hz sine in the slot: a note
//             tuned +12, -12, +7, +50 and -50 cents plays at that ratio to the untuned
//             one (within 0.2 %); a change while the note sounds moves it there
//             without a jump
//   ignored   a synth lead, the native perc voice and the bass with the bytes set
//             render bit for bit what they render without
#include <cmath>
#include <cstring>
#include <vector>

#include "common/memfs.h"
#include "itests/itest_util.h"
#include "pg/io/project_io.h"
#include "pg/macro/macro.h"
#include "pg/sampler/sample_pool.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;
sampler::MemoryPool g_pool;
Project g_a, g_b;

const pm::MacroSet* named(int track, const char* name) {
  for (int i = 0; i < pm::instrumentCount(track); ++i)
    if (std::strcmp(pm::instrumentAt(track, i)->name, name) == 0) return pm::instrumentAt(track, i);
  return nullptr;
}

void post(const Command& c) { g_inst.engine().post(c); }
void postCmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x{};
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  x.step = kDefaultStep;
  post(x);
}

void setInst(int t, const pm::MacroSet* s) {
  uint8_t p[4] = {0, 0, 0, 0};
  for (int m = 0; m < 4 && s; ++m) p[m] = m < s->numMacros ? pm::startByte(s->macros[m]) : 0;
  post(setInstrumentCommand(t, s ? s->id : 0, p));
}

void setTune(int t, int semis, int cents) {
  postCmd(CmdType::SetTrackParam, t, kMixTune, tuneByte(semis));
  postCmd(CmdType::SetTrackParam, t, kMixTuneCents, tuneByte(cents));
}

// Mono output of n quanta (L), as floats.
void render(int quanta, std::vector<float>& out) {
  int16_t buf[2 * kQuantum];
  for (int q = 0; q < quanta; ++q) {
    g_inst.process(buf, kQuantum);
    for (int i = 0; i < kQuantum; ++i) out.push_back(float(buf[2 * i]) * (1.0f / 32768.0f));
  }
}

double freq(const std::vector<float>& x, size_t from, size_t to) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = from + 1; i < to && i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]);
      if (first < 0) first = t;
      else ++n;
      last = t;
    }
  return n > 0 ? 44100.0 * n / (last - first) : 0.0;
}

float maxStep(const std::vector<float>& x, size_t from, size_t to) {
  float m = 0.0f;
  for (size_t i = from + 1; i < to && i < x.size(); ++i) m = std::fmax(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

// A fresh instrument (no FX sends on the track, so the output is the voice), the
// pool with a sine in slot 1, track t on `inst`, its tune set; ready to play.
void fresh(int t, const char* inst, int semis, int cents) {
  g_inst.init(44100.0f, false);
  g_inst.engine().suspend();
  g_inst.setSamplePool(&g_pool);
  g_inst.engine().resume();
  setInst(t, inst ? named(t, inst) : nullptr);
  postCmd(CmdType::SetTrackParam, t, kMixDelaySend, 0);
  postCmd(CmdType::SetTrackParam, t, kMixReverbSend, 0);
  setTune(t, semis, cents);
  std::vector<float> o;
  render(20, o);
}

// ---- store -------------------------------------------------------------------------
void testStore() {
  const Engine& eng = g_inst.engine();
  g_inst.init(44100.0f, false);
  eng.initProject(g_a);
  bool zero = true;
  for (int t = 0; t < kNumTracks; ++t) zero &= g_a.tracks[t].params[kMixTune] == 0 && g_a.tracks[t].params[kMixTuneCents] == 0;
  it::check(zero, "a new project is untuned");
  g_a.tracks[kTrackPerc].params[kMixTune] = tuneByte(-7);
  g_a.tracks[kTrackPerc].params[kMixTuneCents] = tuneByte(35);
  g_a.tracks[kTrackLead].params[kMixTune] = tuneByte(24);
  g_a.tracks[kTrackLead].params[kMixTuneCents] = tuneByte(-50);
  std::memcpy(&g_b, &g_a, sizeof(Project));
  eng.sanitizeProject(g_b);
  it::check(std::memcmp(&g_a, &g_b, sizeof(Project)) == 0, "sanitize keeps a tune in range");
  pgtest::MemFs fs;
  it::check(io::saveProject(fs, io::kMountInt, "TUNED", g_a) == io::Status::Ok, "save");
  it::check(io::loadProject(fs, io::kMountInt, "TUNED", g_b) == io::Status::Ok, "load");
  eng.sanitizeProject(g_b);
  it::check(std::memcmp(&g_a, &g_b, sizeof(Project)) == 0, "save / load / sanitize: byte for byte");
  it::check(tuneValue(g_b.tracks[kTrackPerc].params[kMixTune]) == -7 && tuneValue(g_b.tracks[kTrackPerc].params[kMixTuneCents]) == 35 &&
                tuneValue(g_b.tracks[kTrackLead].params[kMixTune]) == 24 && tuneValue(g_b.tracks[kTrackLead].params[kMixTuneCents]) == -50,
            "the values read back");
  // out of range: clamped; a track that cannot host a sampler: zeroed
  g_b.tracks[kTrackPerc].params[kMixTune] = tuneByte(100);
  g_b.tracks[kTrackPerc].params[kMixTuneCents] = tuneByte(-120);
  g_b.tracks[kTrackLead].params[kMixTune] = tuneByte(-128);
  g_b.tracks[kTrackBass].params[kMixTune] = tuneByte(3);
  g_b.tracks[kTrackKick].params[kMixTuneCents] = tuneByte(10);
  eng.sanitizeProject(g_b);
  it::check(tuneValue(g_b.tracks[kTrackPerc].params[kMixTune]) == 24 && tuneValue(g_b.tracks[kTrackPerc].params[kMixTuneCents]) == -50 &&
                tuneValue(g_b.tracks[kTrackLead].params[kMixTune]) == -24,
            "out of range: clamped (%d %d %d)", tuneValue(g_b.tracks[kTrackPerc].params[kMixTune]),
            tuneValue(g_b.tracks[kTrackPerc].params[kMixTuneCents]), tuneValue(g_b.tracks[kTrackLead].params[kMixTune]));
  it::check(g_b.tracks[kTrackBass].params[kMixTune] == 0 && g_b.tracks[kTrackKick].params[kMixTuneCents] == 0,
            "not a sampler track: zeroed");
  it::check(g_b.tracks[kTrackPerc].params[kMixReserved2] == 0, "the last spare byte stays reserved");
  // older files: the bytes were reserved; whatever they hold, untuned
  for (int v = 1; v <= 2; ++v) {
    std::memcpy(&g_b, &g_a, sizeof(Project));
    g_b.version = uint8_t(v);
    eng.sanitizeProject(g_b);
    it::check(g_b.tracks[kTrackPerc].params[kMixTune] == 0 && g_b.tracks[kTrackPerc].params[kMixTuneCents] == 0 &&
                  g_b.tracks[kTrackLead].params[kMixTune] == 0 && g_b.tracks[kTrackLead].params[kMixTuneCents] == 0,
              "a version %d file reads untuned", v);
  }
}

// ---- untouched -----------------------------------------------------------------------
void testUntouched() {
  g_inst.init(44100.0f, false);
  Project& p = g_inst.project();
  setTune(kTrackPerc, 5, -20);
  setTune(kTrackLead, -3, 45);
  std::vector<float> o;
  render(4, o);
  const pm::MacroSet* sets[] = {named(kTrackPerc, "CHOP"), named(kTrackPerc, "TAP"), named(kTrackPerc, "STUTTER")};
  for (const pm::MacroSet* s : sets) {
    setInst(kTrackPerc, s);
    render(4, o);
    for (int m = 0; m < 4; ++m) postCmd(CmdType::SetMacro, kTrackPerc, m, 37 + 50 * m);
    render(40, o);
  }
  setInst(kTrackLead, named(kTrackLead, "PLUCK"));
  render(4, o);
  for (int m = 0; m < 4; ++m) postCmd(CmdType::SetMacro, kTrackLead, m, 200 - 40 * m);
  render(40, o);
  setInst(kTrackLead, pm::instrumentAt(kTrackLead, 0));
  render(40, o);
  it::check(tuneValue(p.tracks[kTrackPerc].params[kMixTune]) == 5 && tuneValue(p.tracks[kTrackPerc].params[kMixTuneCents]) == -20 &&
                tuneValue(p.tracks[kTrackLead].params[kMixTune]) == -3 && tuneValue(p.tracks[kTrackLead].params[kMixTuneCents]) == 45,
            "instruments, macros and the derivation leave the tune alone");
  // combineMixer: only the mixer rows
  uint8_t player[kParamsPerTrack], inst[kParamsPerTrack], def[kParamsPerTrack], out[kParamsPerTrack];
  for (int i = 0; i < kParamsPerTrack; ++i) player[i] = inst[i] = def[i] = 64;
  std::memset(out, 0x55, sizeof(out));
  player[kMixTune] = tuneByte(-2);
  player[kMixTuneCents] = tuneByte(10);
  pm::combineMixer(player, inst, def, out);
  it::check(out[kMixTune] == 0x55 && out[kMixTuneCents] == 0x55, "combineMixer does not write the tune bytes");
}

// ---- pitch -----------------------------------------------------------------------------
// The frequency of a held note on track t with instrument `inst`, tuned so; the
// window 60 ms .. 540 ms.
double noteHz(int t, const char* inst, int semis, int cents, int note) {
  fresh(t, inst, semis, cents);
  postCmd(CmdType::NoteOn, t, note, 100);
  std::vector<float> o;
  render(1500, o);
  postCmd(CmdType::NoteOff, t, note);
  render(400, o);
  return freq(o, 2646, 1500 * kQuantum);
}

void testPitch() {
  struct T { int track; const char* inst; int note; } ts[] = {{kTrackPerc, "TAPE", 60}, {kTrackLead, "PLUCK", 48}};
  struct C { int semis, cents; } cs[] = {{12, 0}, {-12, 0}, {7, 0}, {0, 50}, {0, -50}, {-2, 35}};
  for (const T& tt : ts) {
    if (!named(tt.track, tt.inst)) { it::check(false, "%s is in the list", tt.inst); continue; }
    const double base = noteHz(tt.track, tt.inst, 0, 0, tt.note);
    it::check(base > 50.0, "%s: the untuned note sounds (%.1f Hz)", tt.inst, base);
    for (const C& c : cs) {
      const double f = noteHz(tt.track, tt.inst, c.semis, c.cents, tt.note);
      const double want = base * std::pow(2.0, (c.semis + c.cents * 0.01) / 12.0);
      it::check(std::fabs(f / want - 1.0) < 0.002, "%s tuned %+d %+dC: %.2f Hz, want %.2f", tt.inst, c.semis, c.cents, f, want);
    }
    // a change while the note sounds: it moves there, without a jump
    fresh(tt.track, tt.inst, 0, 0);
    postCmd(CmdType::NoteOn, tt.track, tt.note, 100);
    std::vector<float> o;
    render(400, o);
    const float before = maxStep(o, 2646, o.size());
    const size_t at = o.size();
    setTune(tt.track, 2, 0);
    render(1200, o);
    const float w = maxStep(o, at - 64, at + 256);
    it::check(w < 1.3f * 1.13f * before, "%s: a tune change under the note does not jump (%.4f vs %.4f)", tt.inst,
              double(w), double(before));
    const double f = freq(o, at + 1000, o.size()), want = base * std::pow(2.0, 2.0 / 12.0);
    it::check(std::fabs(f / want - 1.0) < 0.002, "%s: the sounding note follows (%.2f vs %.2f)", tt.inst, f, want);
  }
}

// ---- ignored -----------------------------------------------------------------------------
void testIgnored() {
  struct T { int track; const char* inst; int note; } ts[] = {
      {kTrackLead, nullptr, 48}, {kTrackPerc, "TAP", 60}, {kTrackBass, nullptr, 36}};
  for (const T& tt : ts) {
    std::vector<float> a, b;
    for (int pass = 0; pass < 2; ++pass) {
      const char* inst = tt.inst ? tt.inst : pm::instrumentAt(tt.track, 0)->name;
      fresh(tt.track, inst, pass ? 7 : 0, pass ? 30 : 0);
      postCmd(CmdType::NoteOn, tt.track, tt.note, 100);
      std::vector<float>& o = pass ? b : a;
      render(300, o);
      postCmd(CmdType::NoteOff, tt.track, tt.note);
      render(200, o);
    }
    double e = 0.0;
    for (float x : a) e += double(x) * double(x);
    it::check(e > 1.0 && a == b, "track %d (%s): the tune changes nothing", tt.track,
              tt.inst ? tt.inst : pm::instrumentAt(tt.track, 0)->name);
  }
}

}  // namespace

int main() {
  // a 441 Hz sine in slot 1 (at the pool's 22,050 Hz)
  static std::vector<int16_t> s(sampler::kSlotMaxSamples, 0);
  for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(std::lrint(0.9f * 32767.0f * std::sin(6.2831853f * 441.0f * float(i) / 22050.0f)));
  g_pool.set(0, s.data(), uint32_t(s.size()), "sine");
  testStore();
  testUntouched();
  testPitch();
  testIgnored();
  return it::finish("itest_tune");
}
