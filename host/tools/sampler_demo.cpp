// sampler_demo: renders each built-in sampler instrument (CHOP, TAPE, STUTTER)
// playing a short pattern over a kick and hats, on the built-in sound an empty
// pool slot plays, to WAV files for listening (stage F1; not a test).
//
//   build/host/sampler_demo [OUTDIR]       default build/sampler: chop.wav tape.wav stutter.wav
//   build/host/sampler_demo --slot N ...   pool slot N (0..11; slot mod 3 picks the built-in sound)
//   build/host/sampler_demo --solo ...     the sampler alone (kick and hats muted)
//
// Four bars at 116 BPM each, device-style mono render written as stereo.
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/wav.h"
#include "pg/instrument/instrument.h"
#include "pg/macro/macro.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;
bool g_solo = false;   // --solo: the sampler alone

const pm::MacroSet* byName(int track, const char* name) {
  for (int i = 0; i < pm::instrumentCount(track); ++i)
    if (std::strcmp(pm::instrumentAt(track, i)->name, name) == 0) return pm::instrumentAt(track, i);
  return nullptr;
}

void post(const Command& c) { g_inst.engine().post(c); }

Step on(int note, int gate, int vel = 100) {
  Step s = kDefaultStep;
  s.setFlags(kStepOn);
  s.note = uint8_t(note);
  s.gate = uint8_t(gate);
  s.vel = uint8_t(vel);
  return s;
}

void set(int track, int i, const Step& s) { post(Command{CmdType::SetStep, 0, uint8_t(track), uint8_t(i), s, nullptr}); }

void render(const char* name, int slot, const std::string& dir) {
  g_inst.init(44100.0f, true);
  const pm::MacroSet* s = byName(kTrackPerc, name);
  if (!s) {
    std::fprintf(stderr, "no built-in %s\n", name);
    return;
  }
  uint8_t pos[4] = {0, 0, 0, 0};
  for (int m = 0; m < s->numMacros; ++m) pos[m] = pm::startByte(s->macros[m]);
  post(setInstrumentCommand(kTrackPerc, s->id, pos));
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, uint8_t(slot), kDefaultStep, nullptr});
  post(Command{CmdType::SetGlobal, uint8_t(kGTempo), 0, 116, kDefaultStep, nullptr});
  for (int i = 0; i < 16; ++i) {
    set(kTrackKick, i, i % 4 == 0 ? on(60, 2) : kDefaultStep);
    set(kTrackHatClosed, i, i % 4 == 2 ? on(60, 2, 80) : kDefaultStep);
  }
  if (!std::strcmp(name, "CHOP")) {
    // eight notes, each its own slice (SLICE = macro 0), a few pitched
    const int steps[8] = {0, 3, 6, 8, 10, 11, 13, 14}, slices[8] = {0, 2, 5, 1, 9, 9, 12, 7}, notes[8] = {60, 60, 60, 60, 63, 65, 60, 55};
    for (int k = 0; k < 8; ++k) {
      Step st = on(notes[k], 3);
      st.setMacroValue(0, (slices[k] * 255 + 8) / 15);
      set(kTrackPerc, steps[k], st);
    }
  } else if (!std::strcmp(name, "TAPE")) {
    // a long note that slows to a stop (SPEED glides 0.75 -> 0.45), then one that spins back
    Step a = on(60, 28);
    a.setMacroValue(0, 191);
    Step b = on(60, 8);
    b.setMacroValue(0, 115);
    Step c = on(60, 16);
    c.setMacroValue(0, 40);
    set(kTrackPerc, 0, a);
    set(kTrackPerc, 7, b);
    set(kTrackPerc, 10, c);
  } else {
    // two stuttered notes; the second one rises (RISE = macro 1) and repeats faster (RATE = macro 0)
    Step a = on(60, 4);
    a.setMacroValue(0, 128);
    a.setMacroValue(1, 70);
    Step b = on(64, 6);
    b.setMacroValue(0, 200);
    b.setMacroValue(1, 220);
    set(kTrackPerc, 0, a);
    set(kTrackPerc, 8, b);
  }
  if (g_solo) {
    post(Command{CmdType::SetMute, uint8_t(kTrackKick), 0, 1, kDefaultStep, nullptr});
    post(Command{CmdType::SetMute, uint8_t(kTrackHatClosed), 0, 1, kDefaultStep, nullptr});
  }
  post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr});
  const long frames = long(4 * 16 * 44100.0 * 15.0 / 116.0) + 44100;
  std::vector<int16_t> out;
  out.reserve(size_t(frames) * 2);
  int16_t buf[2 * kQuantum];
  for (long f = 0; f < frames; f += kQuantum) {
    if (f >= frames - 44100 && f < frames - 44100 + kQuantum) post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
    g_inst.process(buf, kQuantum);
    out.insert(out.end(), buf, buf + 2 * kQuantum);
  }
  std::string lower = name;
  for (char& ch : lower) ch = char(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
  const std::string path = dir + "/" + lower + ".wav";
  wav::write(path.c_str(), out, 44100, 2);
  int peak = 0;
  for (int16_t v : out) peak = std::max(peak, std::abs(int(v)));
  std::printf("%s  peak %d\n", path.c_str(), peak);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "build/sampler";
  int slot = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--solo")) g_solo = true;
    else if (!std::strcmp(argv[i], "--slot") && i + 1 < argc) slot = std::atoi(argv[++i]) % kSampleSlots;
    else dir = argv[i];
  }
  mkdir(dir.c_str(), 0755);
  render("CHOP", slot, dir);
  render("TAPE", slot, dir);
  render("STUTTER", slot, dir);
  return 0;
}
