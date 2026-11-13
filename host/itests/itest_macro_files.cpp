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
//   4. the sets together use every feature (base patch, add, mul, by, smooth,
//      step on an enum, a non-monotonic curve, a mixer row, a start position,
//      a morph, end labels, a curve longer than model v1 allowed),
//      there is a set for every track, one bass macro drives both cutoff and
//      resonance and one lead macro both vibrato and decay (the two pairings
//      the owner named);
//   5. every set has a name, set names are unique per track, and every file
//      starts with its comment header, whose first line names the family
//      ("# NAME -- family: ...", see macros/INDEX.md); that line is the set's
//      header line for the lab (the parser keeps "family: ..." in MacroSet::about),
//      and every set has a player's line (`about "..."`, MacroSet::tagline, at
//      most 38 characters) that the built-in table carries;
//   6. no set loads with a warning, a loaded set fits in 2 KB, and the time one
//      evaluation takes on this host is printed per set.
// That every set also SOUNDS sane (no silent, clipping or NaN point) is the
// macro_audit test (host/tools/macro_audit.cpp).
#include <dirent.h>
#include <sys/stat.h>

#include <strings.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "itest_util.h"
#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::macro;

namespace {

pg::Instrument g_inst;
ParamTable g_pt;
double g_worstNs = 0.0;
unsigned g_sink = 0;

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
       mixer = false, start = false, morph = false, labels = false, longCurve = false;
  bool track[kNumTracks] = {};
  bool cutReso = false, vibDecay = false;
};

void scan(const MacroSet& s, Features* f) {
  f->track[s.track] = true;
  f->base |= s.baseMask != 0;
  f->morph |= s.numMorphs > 0;
  for (int i = 0; i < s.numMacros; ++i) {
    const Macro& m = s.macros[i];
    f->start |= m.start1000 != 0;
    f->labels |= m.lo[0] && m.hi[0];
    uint32_t drives = 0;
    for (int j = 0; j < m.n; ++j) {
      const Target& t = s.target(i, j);
      drives |= 1u << t.param;
      f->add |= t.op() == Op::Add;
      f->mul |= t.op() == Op::Mul;
      f->by |= t.by() != kNoMacro;
      f->smooth |= t.interp() == Interp::Smooth;
      f->mixer |= t.param >= kSoundParams;
      f->longCurve |= t.n > 8;   // more points than model v1 allowed
      const ParamInfo* in = g_pt.info[s.track][t.param];
      f->enumStep |= in->fmt == ParamFormat::Enum && t.interp() == Interp::Step && t.n > 2;
      int dir = 0;
      for (int k = 1; k < t.n; ++k) {
        const int32_t d = pointValMilli(s.points[t.first + k]) - pointValMilli(s.points[t.first + k - 1]);
        const int sd = d > 0 ? 1 : d < 0 ? -1 : 0;
        if (sd && dir && sd != dir) f->nonMono = true;
        if (sd) dir = sd;
      }
    }
    for (int k = 0; k < s.numMorphs; ++k)
      if (s.morphs[k].macro == i) {
        const Patch& pa = s.patches[s.morphs[k].patch];
        for (int e = 0; e < pa.n; ++e) drives |= 1u << s.patchValues[pa.first + e].param;
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
  std::set<std::string> names;
  for (const std::string& path : files) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    std::string text;
    if (fp) {
      char b[4096];
      size_t n;
      while ((n = std::fread(b, 1, sizeof(b), fp)) > 0) text.append(b, n);
      std::fclose(fp);
    }
    static MacroSet s, s2;
    ParseError e;
    Warnings w;
    const bool ok = parse(text.data(), text.size(), g_pt, &s, &e, &w);
    it::check(ok, "%s:%d: %s", path.c_str(), e.line, e.msg);
    if (!ok) continue;
    // a set in macros/ loads without warnings (a deliberate override is marked 'override')
    for (int k = 0; k < w.count && k < kMaxWarnings; ++k)
      it::check(false, "%s:%d: warning: %s", path.c_str(), w.w[k].line, w.w[k].msg);
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
        for (int i = 0; i < s.numMacros; ++i) pos[i] = s.macros[i].start();
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
    // 5. a name, unique per track; the header comment naming the family
    it::check(s.name[0] != 0, "%s: has a 'name' line", path.c_str());
    std::string key = std::string(trackName(s.track)) + "/" + s.name;
    for (char& c : key) c = char(std::toupper(c));
    it::check(names.insert(key).second, "%s: set name %s is used twice on %s", path.c_str(), s.name,
              trackName(s.track));
    // the header: the first line "# NAME -- family: ...", which is also the set's about line
    // (the parser drops the "NAME -- " that repeats the name)
    const size_t eol = text.find('\n');
    const std::string first = text.substr(0, eol == std::string::npos ? text.size() : eol);
    const std::string head = std::string("# ") + s.name + " -- ";
    it::check(strncasecmp(first.c_str(), head.c_str(), head.size()) == 0,
              "%s: the first line is the header '# NAME -- family: ...' (got '%s')", path.c_str(), first.c_str());
    it::check(std::strncmp(s.about, "family: ", 8) == 0, "%s: the header line names the family (got '%s')",
              path.c_str(), s.about);
    // the player's line (`about "..."`): every set has one, it fits the instrument page,
    // it does not repeat the author's header, and the built-in table carries it
    it::check(s.tagline[0] != 0 && std::strlen(s.tagline) <= size_t(kTaglineLen),
              "%s: has an 'about' line of 1..%d characters (got '%s')", path.c_str(), kTaglineLen, s.tagline);
    it::check(strncasecmp(s.tagline, "family", 6) != 0, "%s: the about line speaks to a player (got '%s')",
              path.c_str(), s.tagline);
    if (const MacroSet* b = findBuiltin(s.id))
      it::check(std::strcmp(b->tagline, s.tagline) == 0, "%s: the built-in table's about line is '%s'", path.c_str(),
                b->tagline);
    // cheap enough to evaluate at a few hundred Hz on the device's UI core
    {
      float p[kMaxMacros] = {0.1f, 0.4f, 0.6f, 0.9f};
      const int kN = 20000;
      unsigned sink = 0;
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < kN; ++i) {
        p[i & 3] = float(i % 101) / 100.0f;
        evaluate(s, g_pt, p, under, out);
        sink += out[0];
      }
      const double ns =
          double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / kN;
      g_worstNs = ns > g_worstNs ? ns : g_worstNs;
      g_sink += sink;
      std::printf("  %-26s %3d targets %3d points %2d morphs %2d patch values %d trims  %4.0f ns/eval\n", path.c_str(),
                  s.numTargets, s.numPoints, s.numMorphs, s.numPatchValues, s.numTrims, ns);
    }
    std::printf("  %-26s %-5s %-12s %d macros, owns %d params\n", path.c_str(), trackName(s.track), s.name,
                s.numMacros, __builtin_popcount(s.ownedMask()));
  }
  it::check(f.base && f.add && f.mul && f.by && f.smooth && f.enumStep && f.nonMono && f.mixer && f.start && f.morph &&
                f.labels && f.longCurve,
            "the sets use every feature (base %d add %d mul %d by %d smooth %d enum-step %d "
            "non-monotonic %d mixer %d start %d morph %d end labels %d a curve over 8 points %d)",
            f.base, f.add, f.mul, f.by, f.smooth, f.enumStep, f.nonMono, f.mixer, f.start, f.morph, f.labels,
            f.longCurve);
  std::printf("sizeof(MacroSet) = %zu bytes; slowest set: %.0f ns per evaluation on this host (%u)\n",
              sizeof(MacroSet), g_worstNs, g_sink & 1u);
  it::check(sizeof(MacroSet) <= 2048, "a loaded set fits in 2 KB (%zu bytes)", sizeof(MacroSet));
  for (int t = 0; t < kNumTracks; ++t) it::check(f.track[t], "a set for %s", trackName(t));
  it::check(f.cutReso, "a bass macro drives both cutoff and resonance");
  it::check(f.vibDecay, "a lead macro drives both vibrato and decay");
  return it::finish("itest_macro_files");
}
