// Golden hashes of the instrument's int16 output, taken before the stem tap
// (PG_STEM_TAP, engine/stem_tap.h) was added to the engine. They pin the audio
// path bit for bit: any change to what the device (mono) or a stereo host
// render plays fails here. The ACIDJAM song in device mode is rendered exactly
// as render_demo --demo 0 --mono does (whole chain, then Stop and a 1 s tail),
// plus the first pattern of each demo in stereo and mono at 44.1 and 32 kHz.
//
// The hashes depend on the compiler's float code generation (FMA contraction,
// libm), so they are only enforced on the platform they were taken on
// (x86-64 Linux, GCC); elsewhere the test prints the values and passes.
//
// THE CLASSIC PATH. Since stage G the factory demos use instruments; the
// demos as they were before, every track CLASSIC (bare parameters, one
// parameter lock per step), are kept as a frozen copy in classic_demos.inc,
// and the "classic" cases render that copy with the hashes taken before
// instruments existed: instruments, motion and the v3 step packing must not
// change a CLASSIC project's sound by one bit. The "v1 file" cases save the
// classic demo as a version-1 project through pg::io (with an instrument id
// planted in the bytes version 1 kept reserved), load it back and render it:
// it must load with no instrument and give the same hash as the demo built
// directly. The song cases of BASSHAUS and WAREHOUS were taken at ed5fe13,
// just before instruments were added.
//
// THE INSTRUMENT DEMOS (stage G): the firmware's demos as built now, every
// track an instrument, notes with their own macro values (motion). Their
// hashes were taken when the demos were converted; a change to an instrument
// (macros/*.pgm), the motion engine or the demos themselves moves them, and
// they are then updated deliberately (--print).
//
//   build/host/itest_golden --print     prints the current hashes
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

// The classic cases: taken at commit 1971ca6 (before the stem tap), host build, GCC x86-64.
Case g_cases[] = {
    {"classic acidjam song mono 44k", 0, -1, true, 44100.0f, 0x17a76a05b8e3e21bull, false, true},
    {"classic acidjam p1 stereo 44k", 0, 0, false, 44100.0f, 0x22d4f5d730c86fb9ull, false, true},
    {"classic basshaus p1 stereo 44k", 1, 0, false, 44100.0f, 0x6e8364d9b9018eefull, false, true},
    {"classic warehous p1 stereo 44k", 2, 0, false, 44100.0f, 0x454f74f33961ffb6ull, false, true},
    {"classic basshaus p1 mono 44k", 1, 0, true, 44100.0f, 0x7640829b2ba10f2bull, false, true},
    {"classic warehous p1 mono 32k", 2, 0, true, 32000.0f, 0x5b41d2c973aeb313ull, false, true},
    {"classic acidjam p2 stereo 32k", 0, 1, false, 32000.0f, 0xf829a6ffa02b67f9ull, false, true},
    {"classic basshaus song mono 44k", 1, -1, true, 44100.0f, 0xd59fda34bfb95bd7ull, false, true},
    {"classic warehous song stereo 44k", 2, -1, false, 44100.0f, 0x2a552b7e908ced43ull, false, true},
    {"classic acidjam song mono 44k, v1 file", 0, -1, true, 44100.0f, 0x17a76a05b8e3e21bull, true, true},
    {"classic basshaus song mono 44k, v1 file", 1, -1, true, 44100.0f, 0xd59fda34bfb95bd7ull, true, true},
    {"classic warehous song stereo 44k, v1 file", 2, -1, false, 44100.0f, 0x2a552b7e908ced43ull, true, true},
    // the firmware's demos with instruments and motion, taken at stage G (host build, GCC x86-64);
    // BASSHAUS's retaken when its PERC track moved from TRAIL to SHAKE (the shaker instrument)
    {"acidjam song mono 44k", 0, -1, true, 44100.0f, 0xa0d490e0860a3f1bull, false, false},
    {"acidjam p1 stereo 44k", 0, 0, false, 44100.0f, 0xc920f317890377bcull, false, false},
    {"acidjam p2 stereo 32k", 0, 1, false, 32000.0f, 0x5faae11e6319a8afull, false, false},
    {"basshaus p1 stereo 44k", 1, 0, false, 44100.0f, 0x2002406a5733af88ull, false, false},
    {"basshaus song mono 44k", 1, -1, true, 44100.0f, 0xbf179c3eb8d15c6full, false, false},
    {"warehous p1 mono 32k", 2, 0, true, 32000.0f, 0x767d8ad60152258full, false, false},
    {"warehous song stereo 44k", 2, -1, false, 44100.0f, 0x5269019f71438c47ull, false, false},
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
  if (!kEnforce) std::printf("itest_golden: not enforced on this platform (hashes printed above)\n");
  return it::finish("itest_golden");
}
