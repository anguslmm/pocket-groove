// bench_tape: the CPU cost of the tape (pg/tape/tape.h) in the audio path.
//
//   bench_tape [--seconds S] [--no-callgrind]
//
// The full instrument plays the busy pattern (grooves::programBusy: all 8 tracks,
// ratchets, every send up, DJ filter + drive + ducker on), stereo 48 kHz (the Daisy's
// rate) at 120 BPM with a 4-bar loop on tape, in these scenes:
//   no tape        no tape installed (Engine::setTape(nullptr))
//   empty          a tape installed, nothing on it, nothing recording
//   4 play         all four tape tracks hold audio (a different tone each, level, pan
//                  and both sends on two of them) and play: the block copy (fast path)
//   4 play + rec   the same, and track 1 recording MIX over itself (overdub: its base
//                  and take summed, guarded, and the take written)
//   4 play + HALF  the same as "4 play" with HALF held (the trick path: per-sample
//                  interpolated reads of both heads... one head while it holds)
// Reported: wall clock ns per frame (median of 5), and with valgrind the x86
// instructions per frame of the measured loop (deterministic), the extra over "no tape".
// Internal mode (the callgrind child): --scene K --seconds S.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/grooves.h"
#include "pg/instrument/instrument.h"
#include "pg/tape/tape.h"

namespace {

pg::Instrument g_inst;
pg::tape::Tape g_tape;
std::vector<uint8_t> g_mem;
constexpr float kSr = 48000.0f;
constexpr int kBlock = 256;
int16_t g_buf[2 * kBlock];
volatile int32_t g_sink;

const char* const kNames[] = {"no tape", "empty", "4 play", "4 play + rec", "4 play + HALF"};
constexpr int kNumScenes = 5;

void cmd(pg::Command c) { g_inst.engine().post(c); }

void setup(int scene) {
  g_inst.engine().setTape(nullptr);
  g_inst.init(kSr, false);
  grooves::programBusy(g_inst.project());
  g_inst.project().globals[pg::kGTempo] = 120;
  if (scene > 0) {
    g_mem.assign(pg::tape::Tape::bytesFor(300, uint32_t(kSr)), 0);
    g_tape.init(g_mem.data(), g_mem.size(), uint32_t(kSr));
    g_inst.engine().setTape(&g_tape);
  }
  if (scene >= 2) {
    const uint32_t loop = uint32_t(kSr * 8.0f);   // 4 bars at 120 BPM
    std::vector<int16_t> x(loop);
    for (int t = 0; t < pg::tape::kTracks; ++t) {
      for (uint32_t i = 0; i < loop; ++i)
        x[i] = int16_t(std::lround(5000.0 * std::sin(2.0 * M_PI * (110.0 * (t + 1)) * double(i) / double(kSr))));
      g_tape.write(t, 0, x.data(), loop);
    }
    g_tape.setBpm(120);
    cmd(pg::tape::tapeCommand(pg::tape::kOpParam, 1, 30, pg::tape::kPPan));
    cmd(pg::tape::tapeCommand(pg::tape::kOpParam, 1, 60, pg::tape::kPDelay));
    cmd(pg::tape::tapeCommand(pg::tape::kOpParam, 2, 90, pg::tape::kPReverb));
    cmd(pg::tape::tapeCommand(pg::tape::kOpParam, 2, 100, pg::tape::kPPan));
  }
  pg::Command play{};
  play.type = pg::CmdType::Play;
  cmd(play);
  for (int i = 0; i < int(kSr / kBlock); ++i) {   // a second in: the loop is running
    g_inst.process(g_buf, kBlock);
    if (scene > 0) g_tape.service();
  }
  if (scene == 3) {
    g_tape.prepareTake();
    cmd(pg::tape::tapeCommand(pg::tape::kOpArm, 0, pg::tape::kSrcMix, 0));
    for (int i = 0; i < int(2.5f * kSr / kBlock); ++i) {   // past the next bar line: recording
      g_inst.process(g_buf, kBlock);
      g_tape.service();
    }
  }
  if (scene == 4) {
    cmd(pg::tape::tapeCommand(pg::tape::kOpTrick, 0, pg::tape::kTrickHalf));
    for (int i = 0; i < 8; ++i) g_inst.process(g_buf, kBlock);
  }
}

__attribute__((noinline)) void measureLoop(int blocks, bool service) {
  int32_t acc = 0;
  for (int i = 0; i < blocks; ++i) {
    g_inst.process(g_buf, kBlock);
    acc += g_buf[0];
    if (service && (i & 7) == 7) g_tape.service();   // the main thread's share, every ~43 ms
  }
  g_sink = acc;
}

double wallNsPerFrame(int scene, double seconds) {
  setup(scene);
  const int blocks = int(seconds * double(kSr)) / kBlock;
  const auto t0 = std::chrono::steady_clock::now();
  measureLoop(blocks, scene > 0);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(blocks * kBlock);
}

double callgrindIrPerFrame(const char* self, int scene, double seconds) {
  const std::string out = "build/bench_tape.callgrind." + std::to_string(scene);
  char c[1024];
  std::snprintf(c, sizeof c,
                "valgrind --tool=callgrind --toggle-collect='*measureLoop*' --callgrind-out-file=%s "
                "%s --scene %d --seconds %.3f > /dev/null 2>&1",
                out.c_str(), self, scene, seconds);
  if (std::system(c) != 0) return -1.0;
  FILE* f = std::fopen(out.c_str(), "r");
  if (!f) return -1.0;
  char line[4096];
  double ir = -1.0;
  while (std::fgets(line, sizeof line, f))
    if (!std::strncmp(line, "summary:", 8) || !std::strncmp(line, "totals:", 7)) ir = std::atof(std::strchr(line, ':') + 1);
  std::fclose(f);
  return ir / double(int(seconds * double(kSr)) / kBlock * kBlock);
}

}  // namespace

int main(int argc, char** argv) {
  int scene = -1;
  double seconds = 2.0;
  bool cg = true;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) scene = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--no-callgrind")) cg = false;
    else { std::fprintf(stderr, "usage: bench_tape [--seconds S] [--no-callgrind]\n"); return 2; }
  }
  if (scene >= 0) {   // the callgrind child
    if (scene >= kNumScenes) return 2;
    setup(scene);
    measureLoop(int(seconds * double(kSr)) / kBlock, scene > 0);
    return 0;
  }
  std::printf("== busy pattern, stereo 48 kHz, 120 BPM; the tape's scenes ==\n");
  std::printf("== wall clock, ns per frame (median of 5 x %.1f s) ==\n", seconds);
  for (int s = 0; s < kNumScenes; ++s) {
    std::vector<double> v;
    for (int r = 0; r < 5; ++r) v.push_back(wallNsPerFrame(s, seconds));
    std::sort(v.begin(), v.end());
    std::printf("  %-14s %7.1f\n", kNames[s], v[2]);
  }
  if (cg && std::system("valgrind --version > /dev/null 2>&1") == 0) {
    std::printf("== callgrind: x86 instructions per frame in the measured loop ==\n");
    double base = -1.0;
    for (int s = 0; s < kNumScenes; ++s) {
      const double ir = callgrindIrPerFrame(argv[0], s, seconds);
      if (ir < 0) { std::printf("  %-14s callgrind failed\n", kNames[s]); continue; }
      if (s == 0) base = ir;
      std::printf("  %-14s %7.0f Ir/frame   %+6.0f over no tape (%+.1f %%)\n", kNames[s], ir, ir - base,
                  base > 0 ? 100.0 * (ir - base) / base : 0.0);
    }
  } else {
    std::printf("== callgrind skipped ==\n");
  }
  return 0;
}
