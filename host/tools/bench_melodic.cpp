// The melodic sampler's cost against the PERC sampler's, for the device estimate
// (docs/STATE.md "MELODIC SAMPLER"). Renders `quanta` quanta of one configuration and
// nothing else, so instruction counts can be compared under callgrind:
//   valgrind --tool=callgrind --callgrind-out-file=/dev/null build/host/bench_melodic perc 0
//   valgrind --tool=callgrind ... build/host/bench_melodic perc 20000
// (the difference of the two totals over 20000 x 16 frames = instructions per frame).
// Without valgrind it prints host ns per frame.
//   modes: perc perc-all mel1 mel4 mel1-all mel4-all lead1 lead4
//   perc      the PERC sampler, a plain looping chop (the F1 estimate's "plain chop")
//   perc-all  the same with filter, crush, hold and drive on
//   mel1/4    the melodic sampler, one / four PONG notes, plain chain
//   mel1/4-all  the same with filter + filter envelope, crush, hold, drive and drift on
//   lead1/4   the synth lead it replaces (mono output, as on the device), its defaults,
//             one note / a MIN7 chord
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pg/lead/lead.h"
#include "pg/sampler/melodic.h"
#include "pg/sampler/sampler.h"

using namespace pg;
using namespace pg::sampler;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: bench_melodic perc|perc-all|mel1|mel4|mel1-all|mel4-all|lead1|lead4 QUANTA\n");
    return 2;
  }
  const std::string mode = argv[1];
  const long quanta = std::atol(argv[2]);
  static std::vector<int16_t> s(kSlotMaxSamples);
  for (uint32_t i = 0; i < kSlotMaxSamples; ++i)
    s[i] = int16_t(std::lrint(26000.0 * std::sin(6.2831853 * 300.0 * double(i) / 22050.0)));
  static MemoryPool pool;
  pool.set(0, s.data(), kSlotMaxSamples, "s");
  const bool all = mode.size() > 4 && mode.compare(mode.size() - 4, 4, "-all") == 0;
  float L[kQuantum], R[kQuantum], acc = 0.0f;
  uint8_t p[kSoundParams] = {};
  Voice* v = nullptr;
  static SamplerVoice perc;
  static MelodicSampler mel;
  static lead::LeadVoice synth;
  if (mode.rfind("lead", 0) == 0) {
    synth.init(44100.0f);
    synth.setMono(true);
    for (int i = 0; i < kSoundParams; ++i) p[i] = synth.paramInfo(i)->def;
    p[lead::kPSustain] = 127;
    synth.setParams(p);
    synth.noteOn(NoteEvent{60, 127, false, false, uint8_t(mode[4] == '4' ? kChordMin7 : kChordNone)});
    v = &synth;
  } else if (mode.rfind("perc", 0) == 0) {
    perc.setPool(&pool);
    perc.init(44100.0f);
    for (int i = 0; i < sampler::kNumParams; ++i) p[i] = sampler::paramInfo(i)->def;
    p[sampler::kLoop] = 1;
    if (all) {
      p[sampler::kCutoff] = 70; p[sampler::kReso] = 60; p[sampler::kBits] = 60; p[sampler::kDown] = 40;
      p[sampler::kDrive] = 60; p[sampler::kPitch] = 70;
    }
    perc.setParams(p);
    perc.setSampleSlot(0);
    perc.noteOn(NoteEvent{60, 127, false, false, 0});
    v = &perc;
  } else {
    mel.setPool(&pool);
    mel.init(44100.0f);
    for (int i = 0; i < mel::kNumParams; ++i) p[i] = mel::paramInfo(i)->def;
    p[mel::kLoop] = mel::kPingPong;
    if (all) {
      p[mel::kCutoff] = 70; p[mel::kReso] = 60; p[mel::kEnvAmt] = 50; p[mel::kFltDecay] = 127; p[mel::kBits] = 60;
      p[mel::kDown] = 40; p[mel::kDrive] = 60; p[mel::kDrift] = 60;
    }
    mel.setParams(p);
    mel.setSampleSlot(0);
    const int n = mode[3] == '4' ? 4 : 1;
    mel.noteOn(NoteEvent{48, 127, false, false, uint8_t(n == 4 ? kChordMin7 : kChordNone)});
    v = &mel;
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (long q = 0; q < quanta; ++q) {
    v->setParams(p);
    v->render(L, R, kQuantum);
    acc += L[5];
  }
  const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  if (quanta > 0) std::printf("%s: %.2f ns per frame (%g)\n", mode.c_str(), ns / double(quanta * kQuantum), double(acc) * 0.0);
  return 0;
}
