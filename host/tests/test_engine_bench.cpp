// Benchmark: engine cost per output sample (sequencer + mixer + ducker + send
// FX + master), 8 stub voices that only copy a precomputed buffer. The voices'
// own copy cost is measured separately and subtracted. Prints ns/sample on this
// box and a rough ESP32-S3 estimate (x86 is ~15-25x faster per the
// architecture doc). Fails only on gross regressions.
#include <chrono>
#include <cstdlib>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

namespace {

float g_table[4096];

class BenchVoice : public Voice {
 public:
  int pos = 0;
  void init(float) override {}
  const ParamInfo* paramInfo(int index) const override {
    static const ParamInfo info = {"P0", "Param 0", 0, 127, 64, ParamFormat::Number, nullptr};
    return index == 0 ? &info : nullptr;
  }
  void setParams(const uint8_t* p) override { last = p[0]; }
  void noteOn(const NoteEvent&) override {}
  void noteOff(uint8_t) override {}
  void choke() override {}
  void render(float* l, float* r, int frames) override {
    for (int i = 0; i < frames; ++i) { l[i] = g_table[(pos + i) & 4095]; r[i] = l[i]; }
    pos = (pos + frames + 37) & 4095;
  }
  bool active() const override { return true; }
  uint8_t last = 0;
};

BenchVoice g_voices[kNumTracks];
Project g_proj;

struct Scene {
  const char* name;
  bool delay, reverb, filter, drive, playing;
};

double g_seconds = 4.0;
int g_reps = 5;

double runScene(const Scene& sc, float sr) {
  Engine& eng = engine();
  for (int t = 0; t < kNumTracks; ++t) eng.setVoice(t, &g_voices[t]);
  eng.initProject(g_proj);
  for (int t = 0; t < kNumTracks; ++t) {
    g_proj.tracks[t].params[kMixDelaySend] = sc.delay ? 60 : 0;
    g_proj.tracks[t].params[kMixReverbSend] = sc.reverb ? 60 : 0;
    for (int s = 0; s < 16; s += (t == kTrackKick ? 4 : 2)) g_proj.patterns[0].tracks[t].steps[s].flags = kStepOn;
  }
  g_proj.globals[kGMasterFilter] = sc.filter ? 30 : 64;
  g_proj.globals[kGMasterReso] = 90;
  g_proj.globals[kGMasterDrive] = sc.drive ? 90 : 0;
  g_proj.globals[kGReverbSize] = 100;
  eng.init(sr, &g_proj);
  Command c{};
  c.type = CmdType::Play;
  if (sc.playing) eng.post(c);
  static int16_t buf[2 * 256];
  for (int i = 0; i < 2000; ++i) eng.process(buf, 256);   // warm up, let FX fill
  const int blocks = int(sr * float(g_seconds)) / 256;
  double best = 1e30;
  for (int rep = 0; rep < g_reps; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) eng.process(buf, 256);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(blocks * 256);
    if (ns < best) best = ns;
  }
  return best;
}

double voiceCopyCost(float sr) {
  float l[kQuantum], r[kQuantum];
  const int quanta = int(sr * 4.0f) / kQuantum;
  double best = 1e30;
  volatile float sink = 0;
  for (int rep = 0; rep < 5; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int q = 0; q < quanta; ++q)
      for (int t = 0; t < kNumTracks; ++t) { g_voices[t].render(l, r, kQuantum); sink = sink + l[3]; }
    const auto t1 = std::chrono::steady_clock::now();
    best = std::fmin(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / double(quanta * kQuantum));
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t seed = 99;
  for (float& x : g_table) { seed = seed * 1664525u + 1013904223u; x = 0.3f * float(int32_t(seed)) / 2147483648.0f; }
  const Scene scenes[] = {
      {"idle (stopped, no sends)", false, false, false, false, false},
      {"mixer + ducker only", false, false, false, false, true},
      {"+ delay", true, false, false, false, true},
      {"+ reverb", false, true, false, false, true},
      {"+ DJ filter + drive", false, false, true, true, true},
      {"everything (default-ish mix)", true, true, false, false, true},
      {"everything, filter + drive on", true, true, true, true, true},
  };
  // Profiling mode: test_engine_bench <scene> <seconds> runs one scene once
  // (e.g. under valgrind --tool=callgrind for deterministic instruction counts).
  if (argc >= 3) {
    g_seconds = std::atof(argv[2]);
    g_reps = 1;
    const int i = std::atoi(argv[1]);
    if (i < 0 || i >= int(sizeof(scenes) / sizeof(scenes[0]))) return 2;
    std::printf("%s: %.1f ns/sample\n", scenes[i].name, runScene(scenes[i], 44100.0f));
    return 0;
  }
  for (float sr : {44100.0f, 32000.0f}) {
    const double voices = voiceCopyCost(sr);
    std::printf("  sr %.0f (stub voice copy cost %.1f ns/sample subtracted)\n", double(sr), voices);
    double mixOnly = 0, worst = 0;
    for (const Scene& sc : scenes) {
      const double ns = runScene(sc, sr) - voices;
      if (&sc == &scenes[1]) mixOnly = ns;
      worst = std::fmax(worst, ns);
      // ARCHITECTURE.md: x86 is ~15-25x faster than the target in TIME, so
      // ESP32-S3 cycles ~= ns * (15..25) * 0.24 GHz.
      const double lo = ns * 15.0 * 0.24, hi = ns * 25.0 * 0.24;
      std::printf("    %-32s %6.1f ns/sample   ESP32 est. %4.0f..%4.0f cycles/sample (%4.1f..%4.1f %% of a core at 44.1k)\n", sc.name, ns, lo, hi,
                  100.0 * lo / 5442.0, 100.0 * hi / 5442.0);
    }
    (void)mixOnly;
    CHECKF(worst < 400.0, "engine overhead %.1f ns/sample is a gross regression", worst);
  }
  std::printf("  budget (ARCHITECTURE.md): mixer+ducker 100 + delay 80 + reverb 450 + master 150 = 780 ESP32 cycles/sample\n");
  std::printf("  (wall-clock on a shared VM is noisy; for a deterministic per-function breakdown run\n"
              "   valgrind --tool=callgrind test_engine_bench <scene 0..6> 2)\n");
  return finish("test_engine_bench");
}
