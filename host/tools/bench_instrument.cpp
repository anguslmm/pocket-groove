// bench_instrument: CPU cost of the FULL instrument (8 real voices + engine)
// playing a busy pattern (grooves::programBusy: all 8 tracks, ratchets,
// p-locks, every send up, DJ filter + drive + ducker on).
//
//   bench_instrument [--runs N] [--seconds S] [--no-callgrind]
//
// For each scene (stereo / mono device x 44.1 / 32 kHz):
//   * wall clock: median of N runs of S seconds of audio, ns per output frame;
//   * callgrind (if valgrind is installed): x86 instructions per output frame
//     inside the measured loop only (--toggle-collect), which is deterministic
//     where wall clock on this VM is noisy, plus the top functions by
//     inclusive cost for the mono 44.1 kHz scene.
// Then an ESP32-S3 cycle estimate, see estimateNote below.
//
// Internal mode (used for the callgrind child): --scene K --seconds S.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/grooves.h"
#include "pg/engine/engine_hooks.h"
#include "pg/instrument/instrument.h"

namespace {

pg::Instrument g_inst;

struct Scene {
  const char* name;
  float sr;
  bool mono;
};
const Scene kScenes[] = {
    {"stereo 44.1k", 44100.0f, false},
    {"mono   44.1k", 44100.0f, true},
    {"stereo 32k", 32000.0f, false},
    {"mono   32k", 32000.0f, true},
};
constexpr int kNumScenes = int(sizeof(kScenes) / sizeof(kScenes[0]));
constexpr int kBlock = 256;     // frames per process() call (the firmware's DMA block size class)
int16_t g_buf[2 * kBlock];
volatile int32_t g_sink;

void setup(const Scene& sc) {
  g_inst.init(sc.sr, sc.mono);
  grooves::programBusy(g_inst.project());
  pg::Command c{};
  c.type = pg::CmdType::Play;
  g_inst.engine().post(c);
  // Warm up 2 s: FX lines fill, every voice has sounded, caches warm.
  const int warm = int(2.0f * sc.sr) / kBlock;
  for (int i = 0; i < warm; ++i) g_inst.process(g_buf, kBlock);
}

// The measured loop. Not inlined so callgrind can collect exactly this.
__attribute__((noinline)) void measureLoop(int blocks) {
  int32_t acc = 0;
  for (int i = 0; i < blocks; ++i) {
    g_inst.process(g_buf, kBlock);
    acc += g_buf[0];
  }
  g_sink = acc;
}

double wallNsPerFrame(const Scene& sc, double seconds) {
  setup(sc);
  const int blocks = int(seconds * double(sc.sr)) / kBlock;
  const auto t0 = std::chrono::steady_clock::now();
  measureLoop(blocks);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(blocks * kBlock);
}

bool haveValgrind() { return std::system("valgrind --version > /dev/null 2>&1") == 0; }

// Run ourselves under callgrind for one scene; returns instructions per frame.
double callgrindIrPerFrame(const char* self, int scene, double seconds, const std::string& outFile) {
  char cmd[1024];
  std::snprintf(cmd, sizeof cmd,
                "valgrind --tool=callgrind --toggle-collect='*measureLoop*' --callgrind-out-file=%s "
                "%s --scene %d --seconds %.3f > /dev/null 2>&1",
                outFile.c_str(), self, scene, seconds);
  if (std::system(cmd) != 0) return -1.0;
  FILE* f = std::fopen(outFile.c_str(), "r");
  if (!f) return -1.0;
  char line[4096];
  double ir = -1.0;
  while (std::fgets(line, sizeof line, f)) {
    if (!std::strncmp(line, "summary:", 8) || !std::strncmp(line, "totals:", 7)) ir = std::atof(std::strchr(line, ':') + 1);
  }
  std::fclose(f);
  const int blocks = int(seconds * double(kScenes[scene].sr)) / kBlock;
  return ir / double(blocks * kBlock);
}

// x86 -> ESP32-S3 (Xtensa LX7 @ 240 MHz, single-precision FPU, in-order).
//  * Instruction-count route: for this float DSP code an x86-64 scalar SSE
//    instruction does about the work of one Xtensa instruction (x86 folds
//    loads into arithmetic, Xtensa has fused multiply-add: roughly a wash).
//    The LX7 is in order with FPU latencies of ~3-4 cycles that the
//    compiler only partly hides, and IRAM/DRAM fetches, so we take
//    ~1.3 cycles per instruction (range 1.0..1.6).
//  * Wall-clock route (ARCHITECTURE.md): x86 is 15..25x faster for this kind
//    of code -> cycles = ns * (15..25) * 0.24 cycles/ns.
constexpr double kCpi = 1.3, kCpiLo = 1.0, kCpiHi = 1.6;
constexpr double kBudget = 0.55 * 5442.0;   // cycles per frame at 44.1 kHz, 55 % of one core

}  // namespace

int main(int argc, char** argv) {
  int runs = 7, scene = -1;
  double seconds = 4.0;
  bool cg = true;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--runs") && i + 1 < argc) runs = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) scene = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--no-callgrind")) cg = false;
    else { std::fprintf(stderr, "usage: bench_instrument [--runs N] [--seconds S] [--no-callgrind]\n"); return 2; }
  }
  if (scene >= 0) {   // callgrind child
    if (scene >= kNumScenes) return 2;
    setup(kScenes[scene]);
    measureLoop(int(seconds * double(kScenes[scene].sr)) / kBlock);
    return 0;
  }

  std::printf("== static RAM ==\n");
  std::printf("  sizeof(Instrument)        %7zu bytes (8 voices %zu, Project %zu)\n", sizeof(pg::Instrument),
              sizeof(pg::Instrument) - sizeof(pg::Project), sizeof(pg::Project));
  g_inst.init(44100.0f, true);
  std::printf("  engine state block        %7zu bytes (delay %zu, reverb %zu)\n", pg::engine_detail::staticRamBytes(),
              pg::engine_detail::delayRamBytes(), pg::engine_detail::reverbRamBytes());
  std::printf("  Instrument::staticRamBytes %6zu bytes (+ voice tables, see the integration report)\n",
              pg::Instrument::staticRamBytes());

  std::printf("== busy pattern, full instrument: wall clock (median of %d x %.1f s) ==\n", runs, seconds);
  double wall[kNumScenes];
  for (int s = 0; s < kNumScenes; ++s) {
    std::vector<double> v;
    for (int r = 0; r < runs; ++r) v.push_back(wallNsPerFrame(kScenes[s], seconds));
    std::sort(v.begin(), v.end());
    wall[s] = v[v.size() / 2];
    std::printf("  %-13s %7.1f ns/frame   (min %.1f, max %.1f)   ESP32-S3 by wall clock %4.0f..%4.0f cycles/frame\n",
                kScenes[s].name, wall[s], v.front(), v.back(), wall[s] * 15.0 * 0.24, wall[s] * 25.0 * 0.24);
  }

  if (cg && haveValgrind()) {
    std::printf("== callgrind: x86 instructions per output frame (measured loop only, 1.5 s of audio) ==\n");
    for (int s = 0; s < kNumScenes; ++s) {
      const std::string out = "build/bench_instrument.callgrind." + std::to_string(s);
      const double ir = callgrindIrPerFrame(argv[0], s, 1.5, out);
      if (ir < 0) { std::printf("  %-13s callgrind failed\n", kScenes[s].name); continue; }
      const double fs = double(kScenes[s].sr);
      const double budget = kBudget * 44100.0 / fs;   // 55 % of a core at this rate
      std::printf("  %-13s %6.0f Ir/frame -> ESP32-S3 ~%4.0f cycles/frame (%.0f..%.0f) = %4.1f %% of a core; budget %4.0f -> %s\n",
                  kScenes[s].name, ir, ir * kCpi, ir * kCpiLo, ir * kCpiHi, 100.0 * ir * kCpi / (240.0e6 / fs), budget,
                  ir * kCpi <= budget ? "fits" : (ir * kCpiLo <= budget ? "marginal" : "OVER"));
    }
    std::printf("  top functions (mono 44.1k, inclusive):\n");
    std::fflush(stdout);
    if (std::system("callgrind_annotate --inclusive=yes build/bench_instrument.callgrind.1 2>/dev/null | "
                    "grep -E '^ *[0-9,]+ \\(' | head -16") != 0)
      std::printf("  (callgrind_annotate not available)\n");
  } else {
    std::printf("== callgrind skipped (%s) ==\n", cg ? "valgrind not installed" : "--no-callgrind");
  }
  std::printf("ESP32 estimate: cycles = Ir x %.1f (in-order LX7, FPU latency; range x%.1f..x%.1f), "
              "budget %.0f cycles/frame at 44.1 kHz (55 %% of 5442)\n", kCpi, kCpiLo, kCpiHi, kBudget);
  return 0;
}
