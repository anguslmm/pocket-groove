// bench_motion: the CPU cost of macro motion (engine.h "MACRO MOTION").
//
//   bench_motion [--runs N] [--seconds S] [--no-callgrind]
//
// The full instrument plays the busy pattern (grooves::programBusy: all 8
// tracks, ratchets, every send up, DJ filter + drive + ducker on) with the
// bass on ACID and the lead on HANDS, twice: "still" with every macro standing,
// and "sweep" with two macros of each of those two tracks moving all the time
// (OPEN and BITE, OPEN and PUSH: triangles over the whole range, a new target
// every 33 ms UI frame with the default glide, so the tracks never leave the
// fractional path). Reported per scene (stereo 44.1 kHz = the USB build, mono
// 44.1 kHz = the jack build): wall clock (median of N runs), and with valgrind
// the x86 instructions per frame of the measured loop (deterministic), the
// sweep / still ratio and the extra cost as an ESP32-S3 estimate (cycles =
// instructions x 1.3, as bench_instrument; a share of one core at 240 MHz).
//
// Internal mode (the callgrind child): --scene K --sweep 0|1 --seconds S.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/grooves.h"
#include "pg/instrument/instrument.h"
#include "pg/macro/macro.h"

namespace {

pg::Instrument g_inst;

struct Scene {
  const char* name;
  float sr;
  bool mono;
};
const Scene kScenes[] = {{"stereo 44.1k", 44100.0f, false}, {"mono   44.1k", 44100.0f, true}};
constexpr int kNumScenes = 2;
constexpr int kBlock = 256;
int16_t g_buf[2 * kBlock];
volatile int32_t g_sink;
bool g_sweep = false;
long g_frame = 0, g_nextUi = 0;
int g_phase = 0;

const pg::macro::MacroSet* byName(int track, const char* name) {
  for (int i = 0; i < pg::macro::instrumentCount(track); ++i)
    if (std::strcmp(pg::macro::instrumentAt(track, i)->name, name) == 0) return pg::macro::instrumentAt(track, i);
  return nullptr;
}

void setup(const Scene& sc, bool sweep) {
  g_inst.init(sc.sr, sc.mono);
  grooves::programBusy(g_inst.project());
  const uint8_t bassPos[4] = {102, 60, 0, 128}, leadPos[4] = {77, 60, 0, 128};
  g_inst.engine().post(pg::setInstrumentCommand(pg::kTrackBass, byName(pg::kTrackBass, "ACID")->id, bassPos));
  g_inst.engine().post(pg::setInstrumentCommand(pg::kTrackLead, byName(pg::kTrackLead, "HANDS")->id, leadPos));
  pg::Command c{};
  c.type = pg::CmdType::Play;
  g_inst.engine().post(c);
  g_sweep = sweep;
  g_frame = 0;
  g_nextUi = 0;
  g_phase = 0;
  const int warm = int(2.0f * sc.sr) / kBlock;
  for (int i = 0; i < warm; ++i) g_inst.process(g_buf, kBlock);
}

// One UI frame's worth of macro moves: four triangles, out of phase.
void uiFrame() {
  ++g_phase;
  for (int k = 0; k < 4; ++k) {
    const int ph = (g_phase * 4 + k * 70) % 510;
    const int v = ph < 255 ? ph : 509 - ph;
    g_inst.engine().post(pg::setMacroCommand(k < 2 ? pg::kTrackBass : pg::kTrackLead, k & 1, uint8_t(v)));
  }
}

__attribute__((noinline)) void measureLoop(int blocks, float sr) {
  int32_t acc = 0;
  const long ui = long(0.033f * sr);
  for (int i = 0; i < blocks; ++i) {
    if (g_sweep && g_frame >= g_nextUi) {
      uiFrame();
      g_nextUi += ui;
    }
    g_inst.process(g_buf, kBlock);
    g_frame += kBlock;
    acc += g_buf[0];
  }
  g_sink = acc;
}

double wallNsPerFrame(const Scene& sc, bool sweep, double seconds) {
  setup(sc, sweep);
  const int blocks = int(seconds * double(sc.sr)) / kBlock;
  const auto t0 = std::chrono::steady_clock::now();
  measureLoop(blocks, sc.sr);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(blocks * kBlock);
}

double callgrindIrPerFrame(const char* self, int scene, bool sweep, double seconds) {
  const std::string out = "build/bench_motion.callgrind." + std::to_string(scene) + (sweep ? ".sweep" : ".still");
  char cmd[1024];
  std::snprintf(cmd, sizeof cmd,
                "valgrind --tool=callgrind --toggle-collect='*measureLoop*' --callgrind-out-file=%s "
                "%s --scene %d --sweep %d --seconds %.3f > /dev/null 2>&1",
                out.c_str(), self, scene, sweep ? 1 : 0, seconds);
  if (std::system(cmd) != 0) return -1.0;
  FILE* f = std::fopen(out.c_str(), "r");
  if (!f) return -1.0;
  char line[4096];
  double ir = -1.0;
  while (std::fgets(line, sizeof line, f))
    if (!std::strncmp(line, "summary:", 8) || !std::strncmp(line, "totals:", 7)) ir = std::atof(std::strchr(line, ':') + 1);
  std::fclose(f);
  return ir / double(int(seconds * double(kScenes[scene].sr)) / kBlock * kBlock);
}

constexpr double kCpi = 1.3, kCpiLo = 1.0, kCpiHi = 1.6;   // as bench_instrument

}  // namespace

int main(int argc, char** argv) {
  int runs = 5, scene = -1, sweep = 0;
  double seconds = 4.0;
  bool cg = true;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--runs") && i + 1 < argc) runs = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) scene = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--sweep") && i + 1 < argc) sweep = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--no-callgrind")) cg = false;
    else { std::fprintf(stderr, "usage: bench_motion [--runs N] [--seconds S] [--no-callgrind]\n"); return 2; }
  }
  if (scene >= 0) {   // callgrind child
    if (scene >= kNumScenes) return 2;
    setup(kScenes[scene], sweep != 0);
    measureLoop(int(seconds * double(kScenes[scene].sr)) / kBlock, kScenes[scene].sr);
    return 0;
  }
  std::printf("== busy pattern, bass ACID + lead HANDS; still vs two macros on each sweeping ==\n");
  std::printf("== wall clock, ns per frame (median of %d x %.1f s) ==\n", runs, seconds);
  for (int s = 0; s < kNumScenes; ++s) {
    double med[2];
    for (int w = 0; w < 2; ++w) {
      std::vector<double> v;
      for (int r = 0; r < runs; ++r) v.push_back(wallNsPerFrame(kScenes[s], w == 1, seconds));
      std::sort(v.begin(), v.end());
      med[w] = v[v.size() / 2];
    }
    std::printf("  %-13s still %7.1f   sweep %7.1f   ratio %.3f\n", kScenes[s].name, med[0], med[1], med[1] / med[0]);
  }
  if (cg && std::system("valgrind --version > /dev/null 2>&1") == 0) {
    std::printf("== callgrind: x86 instructions per frame in the measured loop (2 s of audio) ==\n");
    for (int s = 0; s < kNumScenes; ++s) {
      const double a = callgrindIrPerFrame(argv[0], s, false, 2.0), b = callgrindIrPerFrame(argv[0], s, true, 2.0);
      if (a < 0 || b < 0) { std::printf("  %-13s callgrind failed\n", kScenes[s].name); continue; }
      const double extra = b - a, perCore = 240.0e6 / double(kScenes[s].sr);
      std::printf("  %-13s still %6.0f   sweep %6.0f Ir/frame   ratio %.3f   extra %5.1f Ir/frame -> ESP32-S3 ~%.0f "
                  "cycles/frame (%.0f..%.0f) = %.2f %% of a core (%.2f..%.2f)\n",
                  kScenes[s].name, a, b, b / a, extra, extra * kCpi, extra * kCpiLo, extra * kCpiHi,
                  100.0 * extra * kCpi / perCore, 100.0 * extra * kCpiLo / perCore, 100.0 * extra * kCpiHi / perCore);
    }
    std::printf("  top functions of the sweep (stereo, inclusive):\n");
    std::fflush(stdout);
    if (std::system("callgrind_annotate --inclusive=yes build/bench_motion.callgrind.0.sweep 2>/dev/null | "
                    "grep -E '^ *[0-9,]+ \\(' | grep -E 'fine|Fine|evaluate|mapParams|curveAt|applyFine|updateFine|"
                    "measureLoop|quantum' | head -16") != 0)
      std::printf("  (callgrind_annotate not available)\n");
  } else {
    std::printf("== callgrind skipped ==\n");
  }
  std::printf("Not counted here: flash-cache misses on the device (the fractional path is meant to run from IRAM,\n"
              "tools/pio_audio_ram.py) and the reads of the set's table in flash.\n");
  return 0;
}
