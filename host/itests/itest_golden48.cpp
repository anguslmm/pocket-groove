// Golden hashes at 48 kHz (the Daisy's rate; docs/ARCHITECTURE.md "Sample rates"):
// the same renders as itest_golden (render_demo's sequence: two loops of a pattern or
// the song, Stop, a 1 s tail, 256-frame blocks) of a few demos at 48 kHz, so the
// sound at that rate is pinned from now on. The CLASSIC cases render the frozen copy
// of the demos (classic_demos.inc), the others the firmware's demos (instruments,
// motion). Enforced only where they were taken (x86-64 Linux, GCC).
//
//   build/host/itest_golden48 --print     prints the current hashes
#include <cmath>
#include <cstring>

#include "common/memfs.h"
#include "itests/itest_util.h"
#include "pg/demo/demo.h"
#include "pg/io/project_io.h"
#include "pg/macro/macro.h"
#include "itests/classic_demos.inc"

using namespace pg;

namespace {

Instrument g_inst;

#if defined(__x86_64__) && defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
constexpr bool kEnforce = true;
#else
constexpr bool kEnforce = false;
#endif

struct Case {
  const char* name;
  int demo;
  int pattern;    // -1 = whole song chain
  bool mono;
  float sr;
  uint64_t hash;  // expected FNV-1a of the interleaved int16 bytes
  bool v1File;    // through a version-1 file (see the top)
  bool classic;   // the frozen CLASSIC copy (classic_demos.inc), else the firmware's demo
};

void buildClassic(const Engine& e, Project& p, int d) {
  if (d == 0) classic_demo::acid::buildAcidJam(e, p);
  else if (d == 1) classic_demo::haus::buildBassHaus(e, p);
  else classic_demo::ware::buildWarehous(e, p);
}

// Taken when 48 kHz became a first-class rate (host build, GCC x86-64). A deliberate
// change of the sound at 48 kHz moves them; update them with --print then.
Case g_cases[] = {
    {"classic acidjam p1 stereo 48k", 0, 0, false, 48000.0f, 0x00abef27284151cbull, false, true},
    {"classic warehous p1 mono 48k", 2, 0, true, 48000.0f, 0x87941762bbabc25full, false, true},
    {"acidjam p1 stereo 48k", 0, 0, false, 48000.0f, 0xea3d48443de0ee43ull, false, false},
    {"basshaus p1 stereo 48k", 1, 0, false, 48000.0f, 0x1ec9f2e264b74463ull, false, false},
    {"warehous p1 stereo 48k", 2, 0, false, 48000.0f, 0x693e795386d44ee1ull, false, false},
    {"acidjam song mono 48k", 0, -1, true, 48000.0f, 0x06f6a3a118b5b2fbull, false, false},
};

// Saves p as a version-1 project and loads it back (the bytes version 1 kept
// reserved carry an instrument id that must be ignored).
void throughV1File(Project& p) {
  p.version = 1;
  // The classic demos: their steps in the version 1 layout (StepV2).
  for (auto& pat : p.patterns)
    for (auto& tp : pat.tracks)
      for (Step& s : tp.steps) {
        const StepV2 o = {s.note, s.flags(), s.vel, s.gate, s.prob(), s.chord(), s.lockParam(), s.lockValue()};
        std::memcpy(&s, &o, sizeof(o));
      }
  for (int t = 0; t < kNumTracks; ++t) {
    setInstrumentId(p.tracks[t], macro::builtinAt(0).id);
    p.tracks[t].macro[0] = 200;
  }
  pgtest::MemFs fs;
  it::check(io::saveProject(fs, io::kMountInt, "V1", p) == io::Status::Ok, "v1 save");
  static Project q;
  io::HeaderInfo hi{};
  it::check(io::loadProject(fs, io::kMountInt, "V1", q, &hi) == io::Status::Ok, "v1 load");
  it::check(hi.projectVersion == 1, "v1 header says version %d", hi.projectVersion);
  p = q;
}

// Same sequence of calls as host/tools/render_demo.cpp (song or 2 loops of a
// pattern, Stop, 1 s tail, 256-frame blocks).
uint64_t render(const Case& c, long* framesOut) {
  g_inst.init(c.sr, c.mono);
  Engine& eng = g_inst.engine();
  Project& p = g_inst.project();
  if (c.classic) buildClassic(eng, p, c.demo);
  else demo::build(eng, p, c.demo);
  if (c.v1File) throughV1File(p);
  eng.sanitizeProject(p);
  for (int t = 0; t < kNumTracks; ++t) {
    if (c.classic)
      it::check(instrumentId(p.tracks[t]) == 0 && p.tracks[t].macro[0] == 0, "%s: track %d has an instrument", c.name,
                t);
    else
      it::check(macro::findBuiltin(instrumentId(p.tracks[t])) != nullptr, "%s: track %d has no built-in instrument",
                c.name, t);
  }
  long steps = 0;
  if (c.pattern < 0) {
    p.songMode = 1;
    for (int s = 0; s < kSongSlots && p.song[s].repeats; ++s)
      steps += long(p.patterns[p.song[s].pattern].length) * p.song[s].repeats;
  } else {
    p.songMode = 0;
    Command q{};
    q.type = CmdType::QueuePattern;
    q.a = uint8_t(c.pattern);
    eng.post(q);
    steps = long(p.patterns[c.pattern].length) * 2;
  }
  Command play{};
  play.type = CmdType::Play;
  eng.post(play);
  const int bpm = p.globals[kGTempo];
  const double stepSamples = double(c.sr) * 15.0 / double(bpm);
  const long playFrames = long(std::ceil(double(steps) * stepSamples / kQuantum)) * kQuantum;
  const long tailFrames = long(std::ceil(1.0 * double(c.sr) / kQuantum)) * kQuantum;
  it::Stats st;
  int16_t buf[2 * 256];
  long f = 0;
  while (f < playFrames) {
    const int n = int(std::min<long>(256, playFrames - f));
    g_inst.process(buf, n);
    st.add(buf, n);
    f += n;
  }
  Command stop{};
  stop.type = CmdType::Stop;
  eng.post(stop);
  while (f < playFrames + tailFrames) {
    const int n = int(std::min<long>(256, playFrames + tailFrames - f));
    g_inst.process(buf, n);
    st.add(buf, n);
    f += n;
  }
  *framesOut = f;
  return st.hash;
}

}  // namespace

int main(int argc, char** argv) {
  const bool print = argc > 1 && std::strcmp(argv[1], "--print") == 0;
  for (const Case& c : g_cases) {
    long frames = 0;
    const uint64_t h = render(c, &frames);
    if (print || !kEnforce)
      std::printf("    {\"%s\", %d, %d, %s, %.1ff, 0x%016llxull, %s, %s},   // %ld frames\n", c.name, c.demo,
                  c.pattern, c.mono ? "true" : "false", double(c.sr), (unsigned long long)h,
                  c.v1File ? "true" : "false", c.classic ? "true" : "false", frames);
    if (kEnforce && !print) it::check(h == c.hash, "%s: hash %016llx, golden %016llx", c.name, (unsigned long long)h,
                                      (unsigned long long)c.hash);
  }
  if (!kEnforce) std::printf("itest_golden48: not enforced on this platform (hashes printed above)\n");
  return it::finish("itest_golden48");
}
