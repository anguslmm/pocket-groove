// macro_builtin: compiles the .pgm files under macros/ into the built-in
// instrument table the firmware carries in flash
// (lib/pg/src/pg/macro/builtin_data.cpp, docs/MACROLAB.md "Built-in instruments").
//
//   build/host/macro_builtin                       rewrite lib/pg/src/pg/macro/builtin_data.cpp
//   build/host/macro_builtin --check               exit 1 if the checked-in file is stale (ctest check_builtin)
//   build/host/macro_builtin [--check] DIR OUT     another folder / output file
//
// Run it from the repository root. The order comes from DIR/builtin.txt (one
// line per voice: the voice, then its files; the first file is what a new
// project starts with). It fails (and writes nothing) when a listed file does
// not parse, loads with a warning, names another voice than its line, has no
// name; when two sets share an id (macro::setId of voice and name) or one gets
// the reserved id 0; when a .pgm under DIR is not listed, or a listed file is
// missing. The output is a plain C++ source: const MacroSet aggregates, no
// code, deterministic, so the firmware build never runs a host tool.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pg/instrument/instrument.h"
#include "pg/macro/macro.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;
pm::ParamTable g_pt;

bool readFile(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  char b[4096];
  size_t n;
  while ((n = std::fread(b, 1, sizeof(b), f)) > 0) out->append(b, n);
  std::fclose(f);
  return true;
}

void listPgm(const std::string& dir, const std::string& rel, std::vector<std::string>* out, int depth) {
  if (depth > 6) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;
    names.push_back(n);
  }
  closedir(d);
  for (const std::string& n : names) {
    const std::string p = dir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) listPgm(p, r, out, depth + 1);
    else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0) out->push_back(r);
  }
}

std::string cstr(const char* s) {
  std::string o = "\"";
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') o += '\\';
    if (*s == '?' && s[1] == '?') { o += "?\" \""; continue; }   // no trigraph, ever
    o += *s;
  }
  return o + "\"";
}

std::string u8list(const uint8_t* v, int n) {
  std::string o = "{";
  for (int i = 0; i < n; ++i) o += (i ? ", " : "") + std::to_string(v[i]);
  return o + "}";
}

std::string emit(const pm::MacroSet& s, const std::string& file) {
  std::ostringstream o;
  char hex[24];
  o << "    // " << file << "\n    {" << int(s.track) << ", " << int(s.numMacros) << ", " << int(s.numTargets) << ", "
    << int(s.numPoints) << ", " << int(s.numPatches) << ", " << int(s.numPatchValues) << ", " << int(s.numMorphs)
    << ", " << int(s.numTrims) << ",\n";
  std::snprintf(hex, sizeof(hex), "0x%04x", unsigned(s.id));
  o << "     " << hex << ", " << cstr(s.name) << ",\n     " << cstr(s.about) << ",\n";
  std::snprintf(hex, sizeof(hex), "0x%08xu", unsigned(s.baseMask));
  o << "     " << hex << ", " << u8list(s.base, kParamsPerTrack) << ",\n";
  o << "     {";
  for (int m = 0; m < s.numMacros; ++m) {
    const pm::Macro& mc = s.macros[m];
    o << (m ? ",\n      " : "") << "{" << cstr(mc.name) << ", " << cstr(mc.lo) << ", " << cstr(mc.hi) << ", "
      << cstr(mc.desc) << ", " << mc.start1000 << ", " << int(mc.first) << ", " << int(mc.n) << "}";
  }
  o << "},\n     {";
  for (int p = 0; p < s.numPatches; ++p)
    o << (p ? ", " : "") << "{" << cstr(s.patches[p].name) << ", " << int(s.patches[p].first) << ", "
      << int(s.patches[p].n) << "}";
  o << "},\n     {";
  for (int k = 0; k < s.numMorphs; ++k) {
    const pm::Morph& m = s.morphs[k];
    o << (k ? ", " : "") << "{" << int(m.macro) << ", " << int(m.patch) << ", " << int(m.sw) << ", " << int(m.interp)
      << ", " << int(m.first) << ", " << int(m.n) << "}";
  }
  o << "},\n     {";
  for (int k = 0; k < s.numTrims; ++k) {
    const pm::Trim& t = s.trims[k];
    o << (k ? ", " : "") << "{" << int(t.macro) << ", " << int(t.by) << ", " << int(t.interp) << ", " << int(t.first)
      << ", " << int(t.n) << "}";
  }
  o << "},\n     {";
  for (int k = 0; k < s.numTargets; ++k) {
    const pm::Target& t = s.targets[k];
    o << (k ? (k % 8 ? ", " : ",\n      ") : "") << "{" << int(t.param) << ", " << int(t.bits) << ", "
      << int(t.first) << ", " << int(t.n) << "}";
  }
  o << "},\n     {";
  for (int k = 0; k < s.numPatchValues; ++k) {
    const pm::PatchValue& v = s.patchValues[k];
    o << (k ? (k % 10 ? ", " : ",\n      ") : "") << "{" << int(v.param) << ", " << int(v.value) << ", " << int(v.sw)
      << "}";
  }
  o << "},\n     {";
  for (int k = 0; k < s.numPoints; ++k) {
    std::snprintf(hex, sizeof(hex), "0x%08xu", unsigned(s.points[k]));
    o << (k ? (k % 8 ? ", " : ",\n      ") : "") << hex;
  }
  o << "}}";
  return o.str();
}

}  // namespace

int main(int argc, char** argv) {
  bool check = false;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--check") check = true;
    else if (a[0] == '-') {
      std::fprintf(stderr, "usage: macro_builtin [--check] [MACRODIR OUTFILE]   (run from the repository root)\n");
      return 2;
    } else args.push_back(a);
  }
  const std::string dir = args.size() > 0 ? args[0] : "macros";
  const std::string outPath = args.size() > 1 ? args[1] : "lib/pg/src/pg/macro/builtin_data.cpp";

  g_inst.init(44100.0f, false);
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);

  int errors = 0;
  auto err = [&](const std::string& m) {
    std::fprintf(stderr, "macro_builtin: %s\n", m.c_str());
    ++errors;
  };

  // the list
  std::string list;
  if (!readFile(dir + "/builtin.txt", &list)) {
    std::fprintf(stderr, "macro_builtin: cannot read %s/builtin.txt\n", dir.c_str());
    return 2;
  }
  std::vector<std::pair<int, std::string>> files;   // track, relative path
  std::set<std::string> listed;
  std::vector<bool> haveVoice(kNumTracks, false);
  {
    std::istringstream in(list);
    std::string line;
    int ln = 0;
    while (std::getline(in, line)) {
      ++ln;
      const size_t h = line.find('#');
      if (h != std::string::npos) line.resize(h);
      std::istringstream ws(line);
      std::string voice, f;
      if (!(ws >> voice)) continue;
      const int t = pm::trackFromName(voice.c_str());
      if (t < 0) { err("builtin.txt:" + std::to_string(ln) + ": unknown voice '" + voice + "'"); continue; }
      if (haveVoice[size_t(t)]) err("builtin.txt:" + std::to_string(ln) + ": voice '" + voice + "' listed twice");
      haveVoice[size_t(t)] = true;
      while (ws >> f) {
        if (!listed.insert(f).second) err("builtin.txt:" + std::to_string(ln) + ": " + f + " listed twice");
        files.push_back({t, f});
      }
    }
  }
  std::vector<std::string> found;
  listPgm(dir, "", &found, 0);
  std::sort(found.begin(), found.end());
  for (const std::string& f : found)
    if (!listed.count(f)) err(dir + "/" + f + " is not listed in " + dir + "/builtin.txt");

  // parse, in track order (stable: the list order within a track)
  std::stable_sort(files.begin(), files.end(),
                   [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                     return a.first < b.first;
                   });
  static pm::MacroSet sets[128];
  if (files.size() > 128) { err("more than 128 built-in sets"); return 1; }
  std::map<uint16_t, std::string> ids;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string path = dir + "/" + files[i].second;
    std::string text;
    if (!readFile(path, &text)) { err("cannot read " + path); continue; }
    pm::ParseError e;
    pm::Warnings w;
    if (!pm::parse(text.data(), text.size(), g_pt, &sets[i], &e, &w)) {
      err(path + ":" + std::to_string(e.line) + ": " + e.msg);
      continue;
    }
    if (w.count) err(path + ":" + std::to_string(w.w[0].line) + ": warning: " + w.w[0].msg + " (built-ins load clean)");
    if (sets[i].track != files[i].first)
      err(path + " is a " + pm::trackName(sets[i].track) + " set but listed under " + pm::trackName(files[i].first));
    if (!sets[i].name[0]) { err(path + " has no 'name' line (a built-in needs one: it is part of its id)"); continue; }
    if (sets[i].id == pm::kNoInstrument) err(path + ": its id is 0, which means 'no instrument': rename the set");
    const auto it = ids.find(sets[i].id);
    if (it != ids.end()) {
      char b[8];
      std::snprintf(b, sizeof(b), "%04x", unsigned(sets[i].id));
      err(path + " and " + it->second + " have the same id " + b + ": rename one of them");
    }
    ids[sets[i].id] = path;
  }
  for (int t = 0; t < kNumTracks; ++t)
    if (!haveVoice[size_t(t)]) err(std::string("builtin.txt lists no instrument for ") + pm::trackName(t));
  if (errors) {
    std::fprintf(stderr, "macro_builtin: %d problem(s); %s not written\n", errors, outPath.c_str());
    return 1;
  }

  std::ostringstream o;
  o << "// GENERATED by host/tools/macro_builtin from macros/builtin.txt and the .pgm files it lists.\n"
       "// Do not edit: change the .pgm files, then run (from the repository root)\n"
       "//   cmake --build build/host --target macro_builtin && build/host/macro_builtin\n"
       "// and commit the result (ctest check_builtin fails while this file is stale).\n"
       "// Fields in pg::macro::MacroSet order (macro.h); unused pool entries are zero.\n"
       "#include \"builtin_data.h\"\n\n"
       "namespace pg {\nnamespace macro {\nnamespace detail {\n\n"
       "extern const int kBuiltinCount = " << files.size() << ";\n\n"
       "extern const MacroSet kBuiltin[] = {\n";
  for (size_t i = 0; i < files.size(); ++i) o << emit(sets[i], files[i].second) << (i + 1 < files.size() ? ",\n" : "\n");
  o << "};\n\n}  // namespace detail\n}  // namespace macro\n}  // namespace pg\n";
  const std::string text = o.str();

  std::string cur;
  const bool have = readFile(outPath, &cur);
  if (check) {
    if (have && cur == text) {
      std::printf("macro_builtin: %s is current (%zu sets)\n", outPath.c_str(), files.size());
      return 0;
    }
    std::fprintf(stderr,
                 "macro_builtin: %s is STALE (a .pgm or macros/builtin.txt changed). Regenerate and commit:\n"
                 "  cmake --build build/host --target macro_builtin && build/host/macro_builtin\n",
                 outPath.c_str());
    return 1;
  }
  if (have && cur == text) {
    std::printf("macro_builtin: %s unchanged (%zu sets)\n", outPath.c_str(), files.size());
    return 0;
  }
  FILE* f = std::fopen(outPath.c_str(), "wb");
  if (!f || std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
    std::fprintf(stderr, "macro_builtin: cannot write %s\n", outPath.c_str());
    if (f) std::fclose(f);
    return 1;
  }
  std::fclose(f);
  std::printf("macro_builtin: wrote %s (%zu sets)\n", outPath.c_str(), files.size());
  return 0;
}
