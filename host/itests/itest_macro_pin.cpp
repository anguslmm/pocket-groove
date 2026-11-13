// Backwards compatibility of the macro model (docs/MACROLAB.md): the 24
// round-1 sets exactly as they were written for model v1 (a frozen copy in
// host/tests/macro_v1/, so later edits to macros/ do not move the pin) must
// still parse and evaluate to the same values, bit for bit.
//
// Per file: every macro at each of 0, 0.125, 0.3, 0.333, 0.5, 0.625, 0.77, 1
// (the full grid over all macros, so 8^4 = 4096 points for a four-macro set),
// over two "underneath" vectors (the instrument's defaults, and an arbitrary
// in-range pattern so parameters without a base value are exercised too); the
// FNV-1a hash of every evaluated parameter byte is pinned below. The hashes
// were produced by the v1 evaluator before model v2 was written.
//
//   itest_macro_pin            check
//   itest_macro_pin --print    print the table (to regenerate after a deliberate change)
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "itest_util.h"
#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::macro;

namespace {

struct Pin {
  const char* path;
  uint32_t hash;
};
// clang-format off
const Pin kPins[] = {
#include "itest_macro_pin_table.inc"
};
// clang-format on

void listFiles(const std::string& dir, std::vector<std::string>* out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;
    const std::string p = dir + "/" + n;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) listFiles(p, out);
    else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0) out->push_back(p);
  }
  closedir(d);
}

std::string readFile(const std::string& path) {
  std::string text;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return text;
  char b[4096];
  size_t n;
  while ((n = std::fread(b, 1, sizeof(b), fp)) > 0) text.append(b, n);
  std::fclose(fp);
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  const bool print = argc > 1 && std::strcmp(argv[1], "--print") == 0;
  static pg::Instrument inst;
  inst.init(44100.0f, true);
  ParamTable pt;
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) pt.info[t][p] = inst.engine().trackParamInfo(t, p);

  std::vector<std::string> files;
  listFiles("host/tests/macro_v1", &files);
  std::sort(files.begin(), files.end());
  it::check(files.size() == 24, "found %zu frozen round-1 sets under host/tests/macro_v1 (want 24)", files.size());
  const float grid[] = {0.0f, 0.125f, 0.3f, 0.333f, 0.5f, 0.625f, 0.77f, 1.0f};
  constexpr int kG = int(sizeof(grid) / sizeof(grid[0]));
  int checked = 0;
  for (const std::string& path : files) {
    const std::string text = readFile(path);
    static MacroSet s;
    ParseError e;
    if (!parse(text.data(), text.size(), pt, &s, &e)) {
      it::check(false, "%s:%d: %s", path.c_str(), e.line, e.msg);
      continue;
    }
    uint8_t under[2][kParamsPerTrack];
    for (int p = 0; p < kParamsPerTrack; ++p) {
      under[0][p] = inst.project().tracks[s.track].params[p];
      const ParamInfo* in = pt.info[s.track][p];
      const int lo = in ? in->min : 0, hi = in ? in->max : 127;
      under[1][p] = uint8_t(lo + (p * 37 + 11) % (hi - lo + 1));
    }
    uint32_t h = 2166136261u;
    const int nm = s.numMacros;
    int total = 1;
    for (int i = 0; i < nm; ++i) total *= kG;
    for (int u = 0; u < 2; ++u)
      for (int idx = 0; idx < total; ++idx) {
        float pos[kMaxMacros] = {0.0f, 0.0f, 0.0f, 0.0f};
        int r = idx;
        for (int i = 0; i < nm; ++i) {
          pos[i] = grid[r % kG];
          r /= kG;
        }
        uint8_t out[kParamsPerTrack];
        const uint32_t owned = evaluate(s, pt, pos, under[u], out);
        for (int p = 0; p < kParamsPerTrack; ++p) h = (h ^ out[p]) * 16777619u;
        for (int k = 0; k < 4; ++k) h = (h ^ ((owned >> (8 * k)) & 0xFF)) * 16777619u;
      }
    if (print) {
      std::printf("    {\"%s\", 0x%08xu},\n", path.c_str(), h);
      continue;
    }
    bool found = false;
    for (const Pin& pin : kPins)
      if (path == pin.path) {
        found = true;
        it::check(pin.hash == h, "%s: evaluates differently from model v1 (hash %08x, pinned %08x)", path.c_str(), h,
                  pin.hash);
        ++checked;
      }
    it::check(found, "%s: no pinned hash", path.c_str());
  }
  if (print) return 0;
  it::check(checked == int(sizeof(kPins) / sizeof(kPins[0])), "checked %d of %zu pinned sets", checked,
            sizeof(kPins) / sizeof(kPins[0]));
  return it::finish("itest_macro_pin");
}
