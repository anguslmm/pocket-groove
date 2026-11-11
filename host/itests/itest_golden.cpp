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
//   build/host/itest_golden --print     prints the current hashes
#include <cmath>
#include <cstring>

#include "itests/itest_util.h"
#include "pg/demo/demo.h"

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
};

// Taken at commit 1971ca6 (before the stem tap), host build, GCC x86-64.
Case g_cases[] = {
    {"acidjam song mono 44k", 0, -1, true, 44100.0f, 0x17a76a05b8e3e21bull},
    {"acidjam p1 stereo 44k", 0, 0, false, 44100.0f, 0x22d4f5d730c86fb9ull},
    {"basshaus p1 stereo 44k", 1, 0, false, 44100.0f, 0x6e8364d9b9018eefull},
    {"warehous p1 stereo 44k", 2, 0, false, 44100.0f, 0x454f74f33961ffb6ull},
    {"basshaus p1 mono 44k", 1, 0, true, 44100.0f, 0x7640829b2ba10f2bull},
    {"warehous p1 mono 32k", 2, 0, true, 32000.0f, 0x5b41d2c973aeb313ull},
    {"acidjam p2 stereo 32k", 0, 1, false, 32000.0f, 0xf829a6ffa02b67f9ull},
};

// Same sequence of calls as host/tools/render_demo.cpp (song or 2 loops of a
// pattern, Stop, 1 s tail, 256-frame blocks).
uint64_t render(const Case& c, long* framesOut) {
  g_inst.init(c.sr, c.mono);
  Engine& eng = g_inst.engine();
  Project& p = g_inst.project();
  demo::build(eng, p, c.demo);
  eng.sanitizeProject(p);
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
      std::printf("    {\"%s\", %d, %d, %s, %.1ff, 0x%016llxull},   // %ld frames\n", c.name, c.demo, c.pattern,
                  c.mono ? "true" : "false", double(c.sr), (unsigned long long)h, frames);
    if (kEnforce && !print) it::check(h == c.hash, "%s: hash %016llx, golden %016llx", c.name, (unsigned long long)h,
                                      (unsigned long long)c.hash);
  }
  if (!kEnforce) std::printf("itest_golden: not enforced on this platform (hashes printed above)\n");
  return it::finish("itest_golden");
}
