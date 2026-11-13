// The macro sets in macros/ (docs/MACROLAB.md) against the instrument's real
// parameter tables:
//   1. every track's parameter names work in the format: short and long names
//      unique per track after the format's normalisation (case, spaces and
//      punctuation ignored), enum value names usable as words (no space, ':',
//      '=', '#') and unique per parameter;
//   2. every .pgm file under macros/ parses, and format() -> parse() gives the
//      same set back (and the same text a second time);
//   3. every set evaluated over a grid of macro positions stays inside every
//      parameter's range, and owns exactly what it names;
//   4. the starter sets prove the rig: together they use every feature (base
//      patch, add, mul, by, smooth, step on an enum, a non-monotonic curve,
//      a mixer row, a start position), there is a set for kick, snare, bass
//      and lead, one bass macro drives both cutoff and resonance, one lead
//      macro both vibrato and decay.
#include <dirent.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "itest_util.h"
#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::macro;

namespace {

pg::Instrument g_inst;
ParamTable g_pt;

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

std::string norm(const char* s) {
  std::string o;
  for (; *s; ++s) {
    const char c = *s;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) o += c;
    else if (c >= 'A' && c <= 'Z') o += char(c - 'A' + 'a');
  }
  return o;
}

void checkNames() {
  for (int t = 0; t < kNumTracks; ++t) {
    for (int p = 0; p < kParamsPerTrack; ++p) {
      const ParamInfo* a = g_pt.info[t][p];
      if (!a) continue;
      // each name finds its own parameter
      it::check(findParam(g_pt, t, a->name) == p, "%s: short name %s does not find itself", trackName(t), a->name);
      it::check(findParam(g_pt, t, a->longName) == p, "%s: long name '%s' does not find itself", trackName(t),
                a->longName);
      for (int q = p + 1; q < kParamsPerTrack; ++q) {
        const ParamInfo* b = g_pt.info[t][q];
        if (!b) continue;
        const std::string n[4] = {norm(a->name), norm(a->longName), norm(b->name), norm(b->longName)};
        it::check(n[0] != n[2] && n[0] != n[3] && n[1] != n[2] && n[1] != n[3], "%s: %s and %s share a name",
                  trackName(t), a->name, b->name);
      }
      for (const char* c = a->name; *c; ++c)
        it::check(*c != ' ' && *c != ':' && *c != '=' && *c != '#', "%s: short name '%s' is not one word",
                  trackName(t), a->name);
      if (a->fmt != ParamFormat::Enum) continue;
      it::check(a->enumNames != nullptr, "%s %s: enum without names", trackName(t), a->name);
      if (!a->enumNames) continue;
      for (int v = a->min; v <= a->max; ++v) {
        const char* e = a->enumNames[v - a->min];
        it::check(e && *e, "%s %s: enum value %d has no name", trackName(t), a->name, v);
        if (!e) continue;
        for (const char* c = e; *c; ++c)
          it::check(*c != ' ' && *c != ':' && *c != '=' && *c != '#', "%s %s: enum name '%s' is not one word",
                    trackName(t), a->name, e);
        for (int w = v + 1; w <= a->max; ++w) {
          const char* f = a->enumNames[w - a->min];
          it::check(!f || strcasecmp(e, f) != 0, "%s %s: enum names %s and %s collide", trackName(t), a->name, e, f);
        }
      }
    }
  }
}

bool sameSet(const MacroSet& a, const MacroSet& b) {
  char x[8192], y[8192];
  const size_t n = format(a, g_pt, x, sizeof(x)), m = format(b, g_pt, y, sizeof(y));
  return n && n == m && std::strcmp(x, y) == 0 && a.track == b.track && a.numMacros == b.numMacros &&
         a.baseMask == b.baseMask && a.ownedMask() == b.ownedMask();
}

struct Features {
  bool base = false, add = false, mul = false, by = false, smooth = false, enumStep = false, nonMono = false,
       mixer = false, start = false;
  bool track[kNumTracks] = {};
  bool cutReso = false, vibDecay = false;
};

void scan(const MacroSet& s, Features* f) {
  f->track[s.track] = true;
  f->base |= s.baseMask != 0;
  for (int i = 0; i < s.numMacros; ++i) {
    const Macro& m = s.macros[i];
    f->start |= m.start != 0.0f;
    uint32_t drives = 0;
    for (int j = 0; j < m.numTargets; ++j) {
      const Target& t = m.targets[j];
      drives |= 1u << t.param;
      f->add |= t.op == Op::Add;
      f->mul |= t.op == Op::Mul;
      f->by |= t.by != kNoMacro;
      f->smooth |= t.curve.interp == Interp::Smooth;
      f->mixer |= t.param >= kSoundParams;
      const ParamInfo* in = g_pt.info[s.track][t.param];
      f->enumStep |= in->fmt == ParamFormat::Enum && t.curve.interp == Interp::Step && t.curve.n > 2;
      int dir = 0;
      for (int k = 1; k < t.curve.n; ++k) {
        const float d = t.curve.val[k] - t.curve.val[k - 1];
        const int sd = d > 0 ? 1 : d < 0 ? -1 : 0;
        if (sd && dir && sd != dir) f->nonMono = true;
        if (sd) dir = sd;
      }
    }
    if (s.track == kTrackBass && (drives & (1u << bass::kCutoff)) && (drives & (1u << bass::kReso))) f->cutReso = true;
    if (s.track == kTrackLead && (drives & (1u << lead::kPVibrato)) && (drives & (1u << lead::kPDecay)))
      f->vibDecay = true;
  }
}

}  // namespace

int main() {
  g_inst.init(44100.0f, true);
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
  checkNames();

  std::vector<std::string> files;
  listFiles("macros", &files);
  it::check(files.size() >= 4, "found %zu .pgm files under macros/ (run from the repository root)", files.size());
  Features f;
  for (const std::string& path : files) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    std::string text;
    if (fp) {
      char b[4096];
      size_t n;
      while ((n = std::fread(b, 1, sizeof(b), fp)) > 0) text.append(b, n);
      std::fclose(fp);
    }
    MacroSet s, s2;
    ParseError e;
    const bool ok = parse(text.data(), text.size(), g_pt, &s, &e);
    it::check(ok, "%s:%d: %s", path.c_str(), e.line, e.msg);
    if (!ok) continue;
    char buf[8192];
    const size_t n = format(s, g_pt, buf, sizeof(buf));
    it::check(n > 0 && parse(buf, n, g_pt, &s2, &e) && sameSet(s, s2), "%s: format/parse round trip", path.c_str());
    // the grid: every macro at 0, 1/8 ... 1, the others at their start, then all at once
    uint8_t under[kParamsPerTrack], out[kParamsPerTrack];
    for (int p = 0; p < kParamsPerTrack; ++p) under[p] = g_inst.project().tracks[s.track].params[p];
    float pos[kMaxMacros];
    bool inRange = true, ownsRight = true;
    for (int m = 0; m < s.numMacros; ++m)
      for (int k = 0; k <= 8; ++k) {
        for (int i = 0; i < s.numMacros; ++i) pos[i] = s.macros[i].start;
        pos[m] = float(k) / 8.0f;
        const uint32_t owned = evaluate(s, g_pt, pos, under, out);
        ownsRight &= owned == s.ownedMask();
        for (int p = 0; p < kParamsPerTrack; ++p) {
          const ParamInfo* in = g_pt.info[s.track][p];
          if (in) inRange &= out[p] >= in->min && out[p] <= in->max;
          if (!((owned >> p) & 1)) ownsRight &= out[p] == under[p];
        }
      }
    it::check(inRange, "%s: every value inside its parameter's range", path.c_str());
    it::check(ownsRight, "%s: owns exactly the parameters it names", path.c_str());
    scan(s, &f);
    std::printf("  %-26s %-5s %-12s %d macros, owns %d params\n", path.c_str(), trackName(s.track), s.name,
                s.numMacros, __builtin_popcount(s.ownedMask()));
  }
  it::check(f.base && f.add && f.mul && f.by && f.smooth && f.enumStep && f.nonMono && f.mixer && f.start,
            "the starter sets use every feature (base %d add %d mul %d by %d smooth %d enum-step %d "
            "non-monotonic %d mixer %d start %d)",
            f.base, f.add, f.mul, f.by, f.smooth, f.enumStep, f.nonMono, f.mixer, f.start);
  it::check(f.track[kTrackKick] && f.track[kTrackSnare] && f.track[kTrackBass] && f.track[kTrackLead],
            "a starter set for each of kick, snare, bass, lead");
  it::check(f.cutReso, "a bass macro drives both cutoff and resonance");
  it::check(f.vibDecay, "a lead macro drives both vibrato and decay");
  return it::finish("itest_macro_files");
}
