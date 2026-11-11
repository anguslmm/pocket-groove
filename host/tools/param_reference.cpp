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
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "pg/instrument/instrument.h"
#include "pg/ui/format.h"

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

}  // namespace

int main(int argc, char** argv) {
  const char* notes = "docs/manual/param_notes.txt";
  const char* target = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--notes") && i + 1 < argc) notes = argv[++i];
    else if (!std::strcmp(argv[i], "--splice") && i + 1 < argc) target = argv[++i];
    else {
      std::fprintf(stderr, "usage: param_reference [--notes FILE] [--splice MANUAL.md]\n");
      return 2;
    }
  }
  g_inst.init(44100.0f, true);
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
