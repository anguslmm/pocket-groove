// param_reference: dump every parameter table of the instrument (the voices'
// ParamInfo tables, the mixer rows and the globals) as Markdown, for the
// "Parameter reference" chapter of docs/MANUAL.md.
//
//   param_reference [--notes docs/manual/param_notes.txt] [--splice docs/MANUAL.md]
//
// Names, ranges, defaults and on-screen value texts come from the real tables
// (Engine::trackParamInfo / Engine::globalParamInfo, formatted with the UI's
// own pg::ui::formatValue), so the reference follows the code when a default
// changes. The musical descriptions are hand-written in the notes file, one
// line per parameter:
//
//   KICK TUNE | Pitch of the body ...
//
// (section key = KICK SNAR CLAP CHH OHH PERC BASS LEAD MIX GLOBAL; then the
// parameter's short name). A parameter without a note is reported on stderr
// and gets "(no description yet)", so a new parameter is never silently
// undocumented.
//
// Without --splice the Markdown goes to stdout. With --splice FILE the text
// between the lines "<!-- BEGIN PARAM REFERENCE -->" and
// "<!-- END PARAM REFERENCE -->" in FILE is replaced.
//
// The owner's assessment sheet (docs/ASSESSMENT.md) has its own mode:
//
//   param_reference --assessment docs/ASSESSMENT.md        rewrite the sheet's parameter blocks
//   param_reference --check-assessment docs/ASSESSMENT.md  fail if they are stale (run by ctest)
//   [--assess-notes docs/manual/assessment_notes.txt]
//
// Each block sits between "<!-- BEGIN ASSESS <ID> -->" and "<!-- END ASSESS <ID> -->"
// (ID = KICK SNAR CLAP CHH OHH PERC BASS LEAD MIX DUCK DELAY REVERB MASTER SWING) and holds a
// table generated from the ParamInfo tables (names, ranges, defaults as the screen shows them)
// with the hand-written "does" and "try" texts of the notes file
// ("SECTION NAME | does | try", SECTION = a track, MIX or GLOBAL), then a fenced block of answer
// lines, one per parameter, starting with its short name. Rewriting keeps every answer line
// already there (indented lines continue the line above), adds blank ones for new parameters,
// and keeps lines of parameters that no longer exist, so nothing the owner wrote is lost; the
// check then fails until a person deals with them. The check fails when a block differs from
// what the rewrite would produce, a parameter has no note, a block is missing, or a global
// belongs to no block.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pg/instrument/instrument.h"
#include "pg/ui/format.h"
#include "pg/ui/ui_tables.h"

namespace {

pg::Instrument g_inst;
std::map<std::string, std::string> g_notes;
int g_missing = 0;

const char* const kSection[pg::kNumTracks] = {"KICK", "SNAR", "CLAP", "CHH", "OHH", "PERC", "BASS", "LEAD"};
const char* const kTitle[pg::kNumTracks] = {"KICK", "SNARE (SNAR)", "CLAP", "CLOSED HAT (CHH)", "OPEN HAT (OHH)",
                                            "PERC", "BASS", "LEAD"};
const char kPad[] = "asdfghjk";
const char kCol[] = "zxcvbnm,";

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void loadNotes(const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) {
    std::fprintf(stderr, "param_reference: cannot open notes file %s\n", path);
    return;
  }
  char line[2048];
  while (std::fgets(line, sizeof(line), f)) {
    std::string s = line;
    if (s.empty() || s[0] == '#') continue;
    const size_t bar = s.find('|');
    if (bar == std::string::npos) continue;
    g_notes[trim(s.substr(0, bar))] = trim(s.substr(bar + 1));
  }
  std::fclose(f);
}

std::string note(const std::string& section, const char* name) {
  const std::string key = section + " " + name;
  auto it = g_notes.find(key);
  if (it != g_notes.end()) return it->second;
  std::fprintf(stderr, "param_reference: no description for '%s'\n", key.c_str());
  ++g_missing;
  return "(no description yet)";
}

std::string fmt(const pg::ParamInfo* info, const pg::ui::ValueRef& r, int v) {
  char buf[pg::ui::kValueTextCap + 8];
  pg::ui::formatValue(info, r, v, buf);
  return buf;
}

// "0..127", "-24..+24", "1/32 1/16T ... 1/4" (every name for an enum)
std::string range(const pg::ParamInfo* info, const pg::ui::ValueRef& r) {
  if (info->fmt == pg::ParamFormat::Enum) {
    std::string s;
    for (int v = info->min; v <= info->max; ++v) {
      if (!s.empty()) s += " ";
      s += "`" + fmt(info, r, v) + "`";
    }
    return s;
  }
  std::string s = fmt(info, r, info->min) + " .. " + fmt(info, r, info->max);
  return s;
}

// Default as shown on screen; the raw byte too when the screen shows something else.
std::string defText(const pg::ParamInfo* info, const pg::ui::ValueRef& r, int v) {
  const std::string shown = fmt(info, r, v);
  char raw[8];
  std::snprintf(raw, sizeof(raw), "%d", v);
  if (shown == raw) return shown;
  return shown + " (" + raw + ")";
}

void trackTable(std::string& out, int t) {
  const pg::Engine& e = g_inst.engine();
  char buf[256];
  std::snprintf(buf, sizeof(buf), "### %s (pad `%c`)\n\n", kTitle[t], kPad[t]);
  out += buf;
  out += "| Key | Param | Range | Default | What it does |\n|---|---|---|---|---|\n";
  for (int i = 0; i < pg::kSoundParams; ++i) {
    const pg::ParamInfo* info = e.trackParamInfo(t, i);
    if (!info) break;
    const pg::ui::ValueRef r{false, uint8_t(t), uint8_t(i)};
    std::snprintf(buf, sizeof(buf), "| %d `%c` | `%s` %s | ", i / 8 + 1, kCol[i % 8], info->name, info->longName);
    out += buf;
    out += range(info, r) + " | " + defText(info, r, info->def) + " | " + note(kSection[t], info->name) + " |\n";
  }
  out += "\n";
}

void mixTable(std::string& out) {
  const pg::Engine& e = g_inst.engine();
  // MIX view rows in key order z x c v b: LEVEL DUCK DELAY REVERB PAN
  const int rows[5] = {pg::kMixLevel, pg::kMixDuck, pg::kMixDelaySend, pg::kMixReverbSend, pg::kMixPan};
  out += "| Key | Row | Range | KICK | SNAR | CLAP | CHH | OHH | PERC | BASS | LEAD | What it does |\n";
  out += "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (int k = 0; k < 5; ++k) {
    const int p = rows[k];
    const pg::ParamInfo* info0 = e.trackParamInfo(0, p);
    if (!info0) continue;
    const pg::ui::ValueRef r0{false, 0, uint8_t(p)};
    char buf[160];
    std::snprintf(buf, sizeof(buf), "| `%c` | `%s` %s | %s |", kCol[k], info0->name, info0->longName,
                  range(info0, r0).c_str());
    out += buf;
    for (int t = 0; t < pg::kNumTracks; ++t) {
      const pg::ParamInfo* info = e.trackParamInfo(t, p);
      const pg::ui::ValueRef r{false, uint8_t(t), uint8_t(p)};
      out += " " + (info ? defText(info, r, info->def) : std::string("-")) + " |";
    }
    out += " " + note("MIX", info0->name) + " |\n";
  }
  out += "\n";
}

void globalTable(std::string& out) {
  // FX view tabs and columns (lib/pg/src/pg/ui/ui_tables.h kFxPage)
  struct Col { const char* tab; int key; int id; };
  const Col cols[] = {
      {"DUCK", 0, pg::kGDuckSource},   {"DUCK", 1, pg::kGDuckDepth},       {"DUCK", 2, pg::kGDuckHold},
      {"DUCK", 3, pg::kGDuckRelease},  {"DUCK", 4, pg::kGDuckCurve},       {"DUCK", 5, pg::kGDuckFx},
      {"SPACE", 0, pg::kGDelayTime},   {"SPACE", 1, pg::kGDelayFeedback},  {"SPACE", 2, pg::kGDelayTone},
      {"SPACE", 3, pg::kGReverbSize},  {"SPACE", 4, pg::kGReverbDamp},     {"SPACE", 5, pg::kGDelayWidth},
      {"MASTER", 0, pg::kGTempo},      {"MASTER", 1, pg::kGSwing},         {"MASTER", 2, pg::kGMasterVolume},
      {"MASTER", 3, pg::kGMasterFilter}, {"MASTER", 4, pg::kGMasterReso},  {"MASTER", 5, pg::kGMasterDrive},
      {"MASTER", 6, pg::kGScaleRoot},  {"MASTER", 7, pg::kGScaleType},
  };
  out += "| Tab, key | Param | Range | Default | What it does |\n|---|---|---|---|---|\n";
  for (const Col& c : cols) {
    const pg::ParamInfo* info = pg::Engine::globalParamInfo(c.id);
    if (!info) continue;
    const pg::ui::ValueRef r{true, 0, uint8_t(c.id)};
    char buf[160];
    std::snprintf(buf, sizeof(buf), "| %s `%c` | `%s` %s | ", c.tab, kCol[c.key], info->name, info->longName);
    out += buf;
    out += range(info, r) + " | " + defText(info, r, info->def) + " | " + note("GLOBAL", info->name) + " |\n";
  }
  // Any global the FX view does not show would be missing above: list it too.
  for (int g = 0; g < pg::kGNumUsed; ++g) {
    bool shown = false;
    for (const Col& c : cols) shown |= c.id == g;
    if (!shown) std::fprintf(stderr, "param_reference: global %d is not in the FX page table\n", g);
  }
  out += "\n";
}

std::string build() {
  std::string out;
  out += "<!-- Generated by host/tools/param_reference.cpp from the ParamInfo tables;"
         " descriptions from docs/manual/param_notes.txt. Do not edit by hand. -->\n\n";
  out += "Key = SOUND page and column key (`9` cycles pages; `;` `.` also page). "
         "Ranges and defaults are shown as the screen shows them; the raw 0..127 byte is in brackets "
         "where it differs (that is the number to use with `-`/`=` counting, and in p-locks).\n\n";
  for (int t = 0; t < pg::kNumTracks; ++t) trackTable(out, t);
  out += "### Mixer rows (MIX view, every track)\n\n";
  out += "Key = row key in MIX (`0` cycles rows). One column of defaults per track.\n\n";
  mixTable(out);
  out += "### Globals (FX view)\n\n";
  out += "Key = FX tab (`o` cycles DUCK, SPACE, MASTER) and column key.\n\n";
  globalTable(out);
  return out;
}

bool splice(const char* path, const std::string& body) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "param_reference: cannot open %s\n", path);
    return false;
  }
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  const std::string a = "<!-- BEGIN PARAM REFERENCE -->\n", b = "<!-- END PARAM REFERENCE -->";
  const size_t i = s.find(a), j = s.find(b);
  if (i == std::string::npos || j == std::string::npos || j < i) {
    std::fprintf(stderr, "param_reference: markers not found in %s\n", path);
    return false;
  }
  s = s.substr(0, i + a.size()) + body + s.substr(j);
  f = std::fopen(path, "wb");
  if (!f) return false;
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
  return true;
}

// ---- Assessment sheet (docs/ASSESSMENT.md) ---------------------------------------------

struct AssessNote { std::string does, tryIt; };
std::map<std::string, AssessNote> g_assess;
std::vector<std::string> g_problems;   // reasons the check fails

void loadAssessNotes(const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) {
    g_problems.push_back(std::string("cannot open notes file ") + path);
    return;
  }
  char line[4096];
  while (std::fgets(line, sizeof(line), f)) {
    std::string s = line;
    if (s.empty() || s[0] == '#') continue;
    const size_t a = s.find('|');
    if (a == std::string::npos) continue;
    const size_t b = s.find('|', a + 1);
    AssessNote n;
    n.does = trim(s.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
    n.tryIt = b == std::string::npos ? std::string() : trim(s.substr(b + 1));
    g_assess[trim(s.substr(0, a))] = n;
  }
  std::fclose(f);
}

AssessNote assessNote(const std::string& section, const char* name) {
  auto it = g_assess.find(section + " " + name);
  if (it != g_assess.end() && !it->second.does.empty() && !it->second.tryIt.empty()) return it->second;
  g_problems.push_back("no does/try note for '" + section + " " + name + "' in the notes file");
  return {"(no note yet)", "(no note yet)"};
}

const char* const kAssessIds[] = {"KICK", "SNAR", "CLAP", "CHH", "OHH", "PERC", "BASS", "LEAD",
                                  "MIX", "DUCK", "DELAY", "REVERB", "MASTER", "SWING"};
constexpr int kNumAssessIds = int(sizeof(kAssessIds) / sizeof(kAssessIds[0]));

// Which sheet block a global belongs to; nullptr = none (the check then fails).
const char* blockOfGlobal(int g) {
  switch (g) {
    case pg::kGDuckSource: case pg::kGDuckDepth: case pg::kGDuckHold:
    case pg::kGDuckRelease: case pg::kGDuckCurve: case pg::kGDuckFx: return "DUCK";
    case pg::kGDelayTime: case pg::kGDelayFeedback: case pg::kGDelayTone: case pg::kGDelayWidth: return "DELAY";
    case pg::kGReverbSize: case pg::kGReverbDamp: return "REVERB";
    case pg::kGTempo: case pg::kGMasterVolume: case pg::kGMasterFilter: case pg::kGMasterReso:
    case pg::kGMasterDrive: case pg::kGScaleRoot: case pg::kGScaleType: return "MASTER";
    case pg::kGSwing: return "SWING";
    default: return nullptr;
  }
}

std::string cell(std::string s) {   // keep a table cell on one line and its pipes escaped
  std::string o;
  for (char c : s) {
    if (c == '|') o += "\\|";
    else if (c == '\n') o += ' ';
    else o += c;
  }
  return o;
}

std::string answerTemplate(const pg::ParamInfo* info) {
  char buf[128];
  if (info->fmt == pg::ParamFormat::Enum)
    std::snprintf(buf, sizeof(buf), "%-4s  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:", info->name);
  else
    std::snprintf(buf, sizeof(buf), "%-4s  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:",
                  info->name);
  return buf;
}

struct Row { const pg::ParamInfo* info; std::string key, range, def, section; };

std::vector<Row> blockRows(const std::string& id) {
  const pg::Engine& e = g_inst.engine();
  std::vector<Row> rows;
  for (int t = 0; t < pg::kNumTracks; ++t) {
    if (id != kSection[t]) continue;
    for (int i = 0; i < pg::kSoundParams; ++i) {
      const pg::ParamInfo* info = e.trackParamInfo(t, i);
      if (!info) break;
      const pg::ui::ValueRef r{false, uint8_t(t), uint8_t(i)};
      char key[16];
      std::snprintf(key, sizeof(key), "%d `%c`", i / 8 + 1, kCol[i % 8]);
      rows.push_back({info, key, range(info, r), defText(info, r, info->def), kSection[t]});
    }
  }
  if (id == "MIX") {
    for (int k = 0; k < pg::ui::kNumMixRows; ++k) {
      const int p = pg::ui::kMixRowParam[k];
      const pg::ParamInfo* info0 = e.trackParamInfo(0, p);
      if (!info0) continue;
      std::string defs;
      for (int t = 0; t < pg::kNumTracks; ++t) {
        const pg::ParamInfo* info = e.trackParamInfo(t, p);
        const pg::ui::ValueRef r{false, uint8_t(t), uint8_t(p)};
        if (!defs.empty()) defs += " ";
        defs += info ? fmt(info, r, info->def) : std::string("-");
      }
      char key[16];
      std::snprintf(key, sizeof(key), "`%c`", kCol[k]);
      rows.push_back({info0, key, range(info0, pg::ui::ValueRef{false, 0, uint8_t(p)}), defs, "MIX"});
    }
  }
  for (int tab = 0; tab < pg::ui::kNumFxTabs; ++tab) {
    for (int c = 0; c < 8; ++c) {
      const int g = pg::ui::kFxPage[tab][c];
      if (g == 0xFF) continue;
      const char* b = blockOfGlobal(g);
      if (!b || id != b) continue;
      const pg::ParamInfo* info = pg::Engine::globalParamInfo(g);
      if (!info) continue;
      const pg::ui::ValueRef r{true, 0, uint8_t(g)};
      std::string key = std::string(pg::ui::kFxTabNames[tab]) + " `" + kCol[c] + "`";
      rows.push_back({info, key, range(info, r), defText(info, r, info->def), "GLOBAL"});
    }
  }
  return rows;
}

// Answer lines of an existing block, by short name, in their order. Indented lines continue
// the line above.
void parseAnswers(const std::string& block, std::vector<std::pair<std::string, std::string>>& out) {
  const size_t f0 = block.find("\n```");
  if (f0 == std::string::npos) return;
  size_t p = block.find('\n', f0 + 1);
  while (p != std::string::npos && p + 1 < block.size()) {
    const size_t q = block.find('\n', p + 1);
    const std::string line = block.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
    p = q;
    if (line.compare(0, 3, "```") == 0) break;
    if (trim(line).empty()) continue;
    if ((line[0] == ' ' || line[0] == '\t') && !out.empty()) {
      out.back().second += "\n" + line;
      continue;
    }
    const size_t sp = line.find_first_of(" \t");
    out.push_back({line.substr(0, sp), line});
  }
}

std::string assessBlock(const std::string& id, const std::string& old) {
  const std::vector<Row> rows = blockRows(id);
  std::string out = "<!-- BEGIN ASSESS " + id + " -->\n";
  if (id == "MIX")
    out += "| Key | Row | Range | Defaults K S C CH OH P B L | What it does | Try |\n";
  else
    out += "| Key | Param | Range | Default | What it does | Try |\n";
  out += "|---|---|---|---|---|---|\n";
  for (const Row& r : rows) {
    const AssessNote n = assessNote(r.section, r.info->name);
    out += "| " + r.key + " | `" + r.info->name + "` " + r.info->longName + " | " + r.range + " | " + r.def + " | " +
           cell(n.does) + " | " + cell(n.tryIt) + " |\n";
  }
  std::vector<std::pair<std::string, std::string>> answers;
  parseAnswers(old, answers);
  std::vector<bool> used(answers.size(), false);
  out += "\n```text\n";
  for (const Row& r : rows) {
    bool found = false;
    for (size_t i = 0; i < answers.size() && !found; ++i) {
      if (used[i] || answers[i].first != r.info->name) continue;
      used[i] = found = true;
      out += answers[i].second + "\n";
    }
    if (!found) out += answerTemplate(r.info) + "\n";
  }
  for (size_t i = 0; i < answers.size(); ++i) {
    if (used[i]) continue;
    g_problems.push_back("block " + id + ": answer line '" + answers[i].first +
                         "' names no parameter of the instrument (move or delete it)");
    out += answers[i].second + "\n";
  }
  out += "```\n<!-- END ASSESS " + id + " -->";
  return out;
}

bool readFile(const char* path, std::string& s) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return true;
}

// Rewrite (check = false) or check (check = true) the sheet. Returns the exit code.
int assessment(const char* path, bool check) {
  std::string s;
  if (!readFile(path, s)) {
    std::fprintf(stderr, "param_reference: cannot open %s\n", path);
    return 1;
  }
  for (int g = 0; g < pg::kGNumUsed; ++g) {
    bool inFx = false;
    for (int t = 0; t < pg::ui::kNumFxTabs; ++t)
      for (int c = 0; c < 8; ++c) inFx |= pg::ui::kFxPage[t][c] == g;
    if (!blockOfGlobal(g) || !inFx)
      g_problems.push_back("global " + std::string(pg::Engine::globalParamInfo(g)->name) +
                           " has no block in the sheet (blockOfGlobal) or no FX column");
  }
  std::string out = s;
  for (int k = 0; k < kNumAssessIds; ++k) {
    const std::string id = kAssessIds[k];
    const std::string a = "<!-- BEGIN ASSESS " + id + " -->", b = "<!-- END ASSESS " + id + " -->";
    const size_t i = out.find(a), j = out.find(b);
    if (i == std::string::npos || j == std::string::npos || j < i) {
      g_problems.push_back("markers for block " + id + " not found");
      continue;
    }
    const std::string old = out.substr(i, j + b.size() - i);
    out = out.substr(0, i) + assessBlock(id, old) + out.substr(j + b.size());
  }
  for (size_t p = out.find("<!-- BEGIN ASSESS "); p != std::string::npos; p = out.find("<!-- BEGIN ASSESS ", p + 1)) {
    const size_t e = out.find(" -->", p);
    const std::string id = out.substr(p + 18, e == std::string::npos ? 0 : e - p - 18);
    bool known = false;
    for (int k = 0; k < kNumAssessIds; ++k) known |= id == kAssessIds[k];
    if (!known) g_problems.push_back("unknown block " + id + " in the sheet");
  }
  if (check && out != s) {
    size_t line = 1, i = 0;
    while (i < s.size() && i < out.size() && s[i] == out[i]) line += s[i++] == '\n';
    const size_t ls = s.rfind('\n', i == 0 ? 0 : i - 1), lo = out.rfind('\n', i == 0 ? 0 : i - 1);
    auto lineAt = [](const std::string& t, size_t st) {
      st = st == std::string::npos ? 0 : st + 1;
      return t.substr(st, t.find('\n', st) - st);
    };
    g_problems.push_back("the sheet is stale at line " + std::to_string(line) + ":\n    sheet: " + lineAt(s, ls) +
                         "\n    code:  " + lineAt(out, lo));
  }
  if (!check && out != s) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "param_reference: rewrote the parameter blocks of %s\n", path);
  }
  for (const std::string& p : g_problems) std::fprintf(stderr, "param_reference: %s\n", p.c_str());
  if (check && !g_problems.empty()) {
    std::fprintf(stderr,
                 "param_reference: %s is out of step with the code. Fix: add or update the notes in "
                 "docs/manual/assessment_notes.txt, then run build/host/param_reference --assessment %s "
                 "from the repository root.\n",
                 path, path);
    return 1;
  }
  if (check) std::fprintf(stderr, "param_reference: %s matches the code\n", path);
  return g_problems.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* notes = "docs/manual/param_notes.txt";
  const char* assessNotes = "docs/manual/assessment_notes.txt";
  const char* target = nullptr;
  const char* sheet = nullptr;
  bool check = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--notes") && i + 1 < argc) notes = argv[++i];
    else if (!std::strcmp(argv[i], "--splice") && i + 1 < argc) target = argv[++i];
    else if (!std::strcmp(argv[i], "--assess-notes") && i + 1 < argc) assessNotes = argv[++i];
    else if (!std::strcmp(argv[i], "--assessment") && i + 1 < argc) sheet = argv[++i];
    else if (!std::strcmp(argv[i], "--check-assessment") && i + 1 < argc) sheet = argv[++i], check = true;
    else {
      std::fprintf(stderr,
                   "usage: param_reference [--notes FILE] [--splice MANUAL.md]\n"
                   "       param_reference [--assess-notes FILE] --assessment|--check-assessment ASSESSMENT.md\n");
      return 2;
    }
  }
  g_inst.init(44100.0f, true);
  if (sheet) {
    loadAssessNotes(assessNotes);
    return assessment(sheet, check);
  }
  loadNotes(notes);
  const std::string md = build();
  if (target) {
    if (!splice(target, md)) return 1;
    std::fprintf(stderr, "param_reference: wrote %s (%d parameters without a description)\n", target, g_missing);
  } else {
    std::fwrite(md.data(), 1, md.size(), stdout);
  }
  return 0;
}
