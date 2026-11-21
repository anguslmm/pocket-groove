// daisy_bench_ref: the host's count for the Daisy's on-device bench (daisy/app/bench.h):
// the same scenes, the same warm-up and measured blocks (96 frames, 48 kHz stereo,
// Instrument::processFloat), the core built as on the Daisy (PG_TAPE=0: pg_notape),
// counted by callgrind (x86-64 instructions in the measured blocks only).
//
//   daisy_bench_ref                 the table: Ir per frame per scene
//   daisy_bench_ref --header        the same as daisy/app/bench_ref.h (the firmware
//                                   prints the ratio cycles / Ir with it)
//   daisy_bench_ref --profile FILE  self cost per function (mangled names), summed over
//                                   the scenes with each scene weighted by its frames:
//                                   the input of tools/daisy_itcm.py, which picks the
//                                   functions for the ITCM build (daisy/itcm_profile.txt)
// Internal: --scene K (the callgrind child).
// Needs valgrind (callgrind, callgrind_annotate); run it from the repository root.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "app/bench.h"
#include "pg/instrument/instrument.h"

namespace {

pg::Instrument g_inst;
float g_buf[2 * 96];
volatile float g_sink;

constexpr int kFrames = 96;
constexpr float kRate = 48000.0f;

__attribute__((noinline)) void measureBlocks(int n) {
  float acc = 0.0f;
  for (int i = 0; i < n; ++i) {
    g_inst.processFloat(g_buf, kFrames);
    acc += g_buf[0];
  }
  g_sink = acc;
}

void child(int scene) {
  pgd::benchSetup(g_inst, scene, kRate);
  for (int i = 0; i < pgd::kBenchWarm; ++i) g_inst.processFloat(g_buf, kFrames);
  measureBlocks(pgd::kBenchMeasure);
}

std::string g_self;

double irPerFrame(int scene, const std::string& out, bool mangled) {
  char cmd[1024];
  std::snprintf(cmd, sizeof cmd,
                "valgrind --tool=callgrind %s --toggle-collect='*measureBlocks*' --callgrind-out-file=%s %s --scene %d "
                "> /dev/null 2>&1",
                mangled ? "--demangle=no" : "", out.c_str(), g_self.c_str(), scene);
  if (std::system(cmd) != 0) return -1.0;
  FILE* f = std::fopen(out.c_str(), "r");
  if (!f) return -1.0;
  char line[4096];
  double ir = -1.0;
  while (std::fgets(line, sizeof line, f))
    if (!std::strncmp(line, "summary:", 8) || !std::strncmp(line, "totals:", 7)) ir = std::atof(std::strchr(line, ':') + 1);
  std::fclose(f);
  return ir / double(pgd::kBenchMeasure * kFrames);
}

// Self cost per function from callgrind_annotate (mangled names when the run had
// --demangle=no): "  1,234,567 (12.3%)  file:function [object]".
void addSelf(const std::string& out, double scale, std::map<std::string, double>& acc) {
  const std::string cmd = "callgrind_annotate --threshold=100 " + out + " 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return;
  char line[8192];
  while (std::fgets(line, sizeof line, p)) {
    const char* s = line;
    while (*s == ' ') ++s;
    if (*s < '0' || *s > '9') continue;
    std::string num;
    while ((*s >= '0' && *s <= '9') || *s == ',') {
      if (*s != ',') num += *s;
      ++s;
    }
    const char* paren = std::strstr(s, ")  ");
    if (!paren) continue;
    std::string rest = paren + 3;
    const size_t colon = rest.find(':');
    if (colon == std::string::npos) continue;
    std::string fn = rest.substr(colon + 1);
    const size_t sp = fn.find(' ');
    if (sp != std::string::npos) fn = fn.substr(0, sp);
    while (!fn.empty() && (fn.back() == '\n' || fn.back() == '\r')) fn.pop_back();
    if (fn.empty() || fn == "TOTALS") continue;
    acc[fn] += std::atof(num.c_str()) * scale;
  }
  pclose(p);
}

}  // namespace

int main(int argc, char** argv) {
  g_self = argv[0];
  bool header = false;
  const char* profile = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) {
      child(std::atoi(argv[++i]));
      return 0;
    } else if (!std::strcmp(argv[i], "--header")) {
      header = true;
    } else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) {
      profile = argv[++i];
    } else {
      std::fprintf(stderr, "usage: daisy_bench_ref [--header] [--profile FILE]\n");
      return 2;
    }
  }
  if (std::system("valgrind --version > /dev/null 2>&1") != 0) {
    std::fprintf(stderr, "daisy_bench_ref: valgrind is not installed\n");
    return 1;
  }
  const int n = pgd::benchScenes();
  std::vector<double> ir(size_t(n), -1.0);
  std::map<std::string, double> self;
  for (int s = 0; s < n; ++s) {
    const std::string out = "build/daisy_bench_ref.callgrind." + std::to_string(s);
    ir[size_t(s)] = irPerFrame(s, out, profile != nullptr);
    if (profile && ir[size_t(s)] > 0) addSelf(out, 1.0 / double(pgd::kBenchMeasure * kFrames), self);
  }
  if (header) {
    std::printf(
        "// The host's count for each bench scene (bench.h): x86-64 instructions per frame in\n"
        "// the measured blocks only (callgrind), the same scene, the same blocks, the same\n"
        "// float output at 48 kHz stereo, the core built PG_TAPE=0 as on the Daisy. Written by\n"
        "//   build/host/daisy/daisy_bench_ref --header > daisy/app/bench_ref.h\n"
        "// (host/daisy/daisy_bench_ref.cpp). The on-device bench divides its cycles per frame\n"
        "// by these: \"cycles per x86 instruction\", the figure docs/DAISY.md section 11 had to\n"
        "// guess (1.0-1.6). Regenerate when the engine or the voices change.\n"
        "#pragma once\n\nnamespace pgd {\n\nstruct BenchRef {\n  const char* scene;\n  float irPerFrame;\n};\n\n");
    std::printf("constexpr const char* kBenchRefNote = \"x86-64 %s, -O2, callgrind\";\n", __VERSION__);
    std::printf("constexpr BenchRef kBenchRef[] = {\n");
    for (int s = 0; s < n; ++s) std::printf("    {\"%s\", %.1ff},\n", pgd::benchSceneName(s), ir[size_t(s)]);
    std::printf("};\n\n}  // namespace pgd\n");
  } else {
    std::printf("scene      Ir/frame  (x86-64, callgrind, %d blocks after %d)\n", pgd::kBenchMeasure, pgd::kBenchWarm);
    for (int s = 0; s < n; ++s) std::printf("%-9s %9.1f\n", pgd::benchSceneName(s), ir[size_t(s)]);
  }
  if (profile) {
    FILE* f = std::fopen(profile, "w");
    if (!f) return 1;
    std::fprintf(f,
                 "# Self cost of each function in x86-64 instructions per frame, summed over the %d bench scenes\n"
                 "# (host/daisy/daisy_bench_ref --profile; tools/daisy_itcm.py reads it). Mangled names.\n",
                 n);
    std::vector<std::pair<double, std::string>> v;
    for (auto& kv : self) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& e : v)
      if (e.first >= 0.05) std::fprintf(f, "%.2f %s\n", e.first, e.second.c_str());
    std::fclose(f);
  }
  return 0;
}
