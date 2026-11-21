// The host's count for each bench scene (bench.h): x86-64 instructions per frame in
// the measured blocks only (callgrind), the same scene, the same blocks, the same
// float output at 48 kHz stereo, the core built PG_TAPE=0 as on the Daisy. Written by
//   build/host/daisy/daisy_bench_ref --header > daisy/app/bench_ref.h
// (host/daisy/daisy_bench_ref.cpp). The on-device bench divides its cycles per frame
// by these: "cycles per x86 instruction", the figure docs/DAISY.md section 11 had to
// guess (1.0-1.6). Regenerate when the engine or the voices change.
#pragma once

namespace pgd {

struct BenchRef {
  const char* scene;
  float irPerFrame;
};

constexpr const char* kBenchRefNote = "x86-64 13.3.0, -O2, callgrind";
constexpr BenchRef kBenchRef[] = {
    {"idle", 226.5f},
    {"ACIDJAM", 1076.3f},
    {"BASSHAUS", 1076.8f},
    {"WAREHOUS", 811.5f},
    {"busy", 2106.7f},
};

}  // namespace pgd
