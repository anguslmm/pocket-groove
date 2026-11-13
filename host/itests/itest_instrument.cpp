// Instruments in the engine (engine.h "INSTRUMENTS", docs/MACROLAB.md):
//
//   table     the generated built-in table is exactly what parsing macros/
//             gives now (independent of the check_builtin ctest), ids are
//             setId(voice, name), unique and never 0, in the order of
//             macros/builtin.txt; the library lookups agree.
//   derive    SetInstrument / SetMacro derive params 0..23 = evaluate(set,
//             positions, under = the voice defaults); several moves in one
//             quantum (two macros at once); a Project edited directly is
//             followed; the mixer rows stay the player's.
//   mixer     combineMixer's rule, and the engine's effective level and sends;
//             an instrument track renders bit-identically to a CLASSIC track
//             given the same effective values (the values reach the voices).
//   unknown   an id the library does not know plays the stored parameters,
//             keeps the id, never crashes.
//   locks     CLASSIC: a sound lock plays at its step, an old macro lock is
//             dropped (an instrument track's notes carry macro values instead:
//             itest_motion_notes).
//   overlay   a host overlay replacing a built-in takes effect on
//             RefreshInstruments, counted also when discarded by suspend().
//   copy      CopyTrack drops a note's own macro values on another track.
//   files     a project with instruments round-trips through pg::io and the
//             engine binds it again after suspend / load / resume.
//   stems     the offline bounce (host/common/bounce.h) plays the instrument:
//             the bass stem follows a macro, and equals the stem of a CLASSIC
//             project holding the derived values.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/bounce.h"
#include "common/memfs.h"
#include "itests/itest_util.h"
#include "pg/io/project_io.h"
#include "pg/macro/macro.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;
pm::ParamTable g_pt;
uint8_t g_def[kNumTracks][kParamsPerTrack];

void setup() {
  g_inst.init(44100.0f, false);
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) {
      g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
      g_def[t][p] = g_pt.info[t][p] ? g_pt.info[t][p]->def : 0;
    }
}

void quanta(int n) {
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) g_inst.process(buf, kQuantum);
}

void setInstrument(int t, const pm::MacroSet* s, const uint8_t* pos = nullptr) {
  uint8_t p[4] = {0, 0, 0, 0};
  for (int m = 0; m < 4; ++m) p[m] = pos ? pos[m] : (s && m < s->numMacros ? pm::startByte(s->macros[m]) : 0);
  g_inst.engine().post(setInstrumentCommand(t, s ? s->id : 0, p));
}

void derived(const pm::MacroSet& s, const uint8_t* pos, uint8_t* out) {
  float f[4];
  for (int m = 0; m < 4; ++m) f[m] = pm::posFromByte(pos[m]);
  // the values under the set: its voice kind's defaults (VOICE KINDS: a sampler set on perc)
  uint8_t under[kParamsPerTrack];
  std::memcpy(under, g_def[s.track], sizeof(under));
  const ParamInfo* scratch[kParamsPerTrack];
  const pm::TrackParams kp = pm::paramsFor(s, g_pt, scratch);
  if (s.voice != kVoiceNative)
    for (int p = 0; p < kSoundParams; ++p) under[p] = kp[p] ? kp[p]->def : 0;
  pm::evaluate(s, g_pt, f, under, out);
}

const pm::MacroSet* byName(int track, const char* name) {
  for (int i = 0; i < pm::instrumentCount(track); ++i) {
    const pm::MacroSet* s = pm::instrumentAt(track, i);
    if (s && std::strcmp(s->name, name) == 0) return s;
  }
  return nullptr;
}

bool readFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

// ---- table ----------------------------------------------------------------------------
void testTable() {
  std::string list;
  it::check(readFile("macros/builtin.txt", &list), "macros/builtin.txt readable (run from the repository root)");
  std::istringstream in(list);
  std::string line;
  std::vector<std::pair<int, std::string>> files;
  while (std::getline(in, line)) {
    const size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    std::istringstream ws(line);
    std::string v, f;
    if (!(ws >> v)) continue;
    const int t = pm::trackFromName(v.c_str());
    while (ws >> f) files.push_back({t, f});
  }
  std::stable_sort(files.begin(), files.end(),
                   [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first < b.first; });
  it::check(int(files.size()) == pm::builtinCount(), "builtinCount %d, list %zu", pm::builtinCount(), files.size());
  std::map<uint16_t, int> ids;
  int prevTrack = -1;
  for (int i = 0; i < pm::builtinCount() && i < int(files.size()); ++i) {
    const pm::MacroSet& b = pm::builtinAt(i);
    static pm::MacroSet parsed;
    std::string text;
    readFile("macros/" + files[size_t(i)].second, &text);
    pm::ParseError e;
    const bool ok = pm::parse(text.data(), text.size(), g_pt, &parsed, &e);
    it::check(ok, "%s parses", files[size_t(i)].second.c_str());
    it::check(ok && std::memcmp(&parsed, &b, sizeof(b)) == 0, "built-in %d (%s) is not what %s parses to: regenerate", i,
              b.name, files[size_t(i)].second.c_str());
    it::check(b.id != 0 && b.id == pm::setId(b.track, b.name), "%s: id %04x", b.name, unsigned(b.id));
    it::check(ids.insert({b.id, i}).second, "%s: id %04x collides", b.name, unsigned(b.id));
    it::check(b.track >= prevTrack, "track order");
    prevTrack = b.track;
    it::check(pm::findBuiltin(b.id) == &b && pm::findInstrument(b.id) == &b, "%s: lookups", b.name);
  }
  for (int t = 0; t < kNumTracks; ++t) {
    it::check(pm::instrumentCount(t) >= 1, "track %d has instruments", t);
    for (int i = 0; i < pm::instrumentCount(t); ++i) {
      const pm::MacroSet* s = pm::instrumentAt(t, i);
      it::check(s && s->track == t && pm::instrumentIndex(t, s->id) == i, "track %d instrument %d", t, i);
    }
  }
  // the first of each line is what a new project starts with
  it::check(std::strcmp(pm::instrumentAt(kTrackBass, 0)->name, "ACID") == 0, "bass starts with ACID");
  it::check(pm::findInstrument(0) == nullptr && pm::instrumentIndex(kTrackBass, 0) == -1, "id 0 is no instrument");
  it::check(pm::instrumentAt(kTrackBass, 99) == nullptr && pm::instrumentAt(kTrackBass, -1) == nullptr, "out of range");
  std::printf("  table: %d built-in instruments, %zu bytes each in flash\n", pm::builtinCount(), sizeof(pm::MacroSet));
}

// ---- derive ---------------------------------------------------------------------------
void testDerive() {
  setup();
  Project& p = g_inst.project();
  const pm::MacroSet* acid = byName(kTrackBass, "ACID");
  const pm::MacroSet* poles = byName(kTrackBass, "POLES");
  it::check(acid && poles, "ACID and POLES exist");
  if (!acid || !poles) return;
  uint8_t mix[kParamsPerTrack];
  std::memcpy(mix, p.tracks[kTrackBass].params, kParamsPerTrack);
  setInstrument(kTrackBass, acid);
  quanta(1);
  uint8_t want[kParamsPerTrack], pos[4];
  std::memcpy(pos, p.tracks[kTrackBass].macro, 4);
  for (int m = 0; m < acid->numMacros; ++m)
    it::check(pos[m] == pm::startByte(acid->macros[m]), "ACID %s starts at its start", acid->macros[m].name);
  derived(*acid, pos, want);
  it::check(instrumentId(p.tracks[kTrackBass]) == acid->id, "id stored");
  it::check(std::memcmp(p.tracks[kTrackBass].params, want, kSoundParams) == 0, "params 0..23 derived");
  it::check(std::memcmp(p.tracks[kTrackBass].params + kSoundParams, mix + kSoundParams, 8) == 0, "mixer rows untouched");

  // two macros moved at once (one quantum), as two fingers do
  Command c{};
  c.type = CmdType::SetMacro;
  c.a = kTrackBass;
  c.b = 0; c.c = 230; g_inst.engine().post(c);
  c.b = 1; c.c = 40; g_inst.engine().post(c);
  quanta(1);
  pos[0] = 230; pos[1] = 40;
  derived(*acid, pos, want);
  it::check(std::memcmp(p.tracks[kTrackBass].params, want, kSoundParams) == 0, "two macros at once");
  it::check(std::memcmp(p.tracks[kTrackBass].params + kSoundParams, mix + kSoundParams, 8) == 0, "mixer untouched (2)");
  uint8_t eff[kParamsPerTrack];
  engine_detail::effectiveParams(kTrackBass, eff);
  it::check(std::memcmp(eff, want, kSoundParams) == 0, "the voice gets the derived values");

  // another instrument, then a Project edited directly (as a tool does) is followed
  setInstrument(kTrackBass, poles);
  quanta(1);
  std::memcpy(pos, p.tracks[kTrackBass].macro, 4);
  derived(*poles, pos, want);
  it::check(std::memcmp(p.tracks[kTrackBass].params, want, kSoundParams) == 0, "POLES derived");
  p.tracks[kTrackBass].macro[2] = 255;
  pos[2] = 255;
  quanta(1);
  derived(*poles, pos, want);
  it::check(std::memcmp(p.tracks[kTrackBass].params, want, kSoundParams) == 0, "direct edit followed");

  // CLASSIC: the values stay, nothing is derived any more
  uint8_t before[kParamsPerTrack];
  std::memcpy(before, p.tracks[kTrackBass].params, kParamsPerTrack);
  setInstrument(kTrackBass, nullptr);
  quanta(1);
  p.tracks[kTrackBass].macro[0] = 17;
  quanta(2);
  it::check(std::memcmp(before, p.tracks[kTrackBass].params, kParamsPerTrack) == 0, "CLASSIC keeps the values");
  it::check(instrumentId(p.tracks[kTrackBass]) == 0, "CLASSIC id 0");
}

// ---- mixer rule -------------------------------------------------------------------------
void testMixerRule() {
  uint8_t player[kParamsPerTrack] = {}, inst[kParamsPerTrack] = {}, def[kParamsPerTrack] = {}, out[kParamsPerTrack] = {};
  def[kMixLevel] = 98; def[kMixDelaySend] = 0; def[kMixReverbSend] = 10;
  player[kMixLevel] = 70; player[kMixPan] = 20; player[kMixDelaySend] = 10; player[kMixReverbSend] = 100; player[kMixDuck] = 77;
  inst[kMixLevel] = 96; inst[kMixPan] = 127; inst[kMixDelaySend] = 40; inst[kMixReverbSend] = 60; inst[kMixDuck] = 0;
  pm::combineMixer(player, inst, def, out);
  it::check(out[kMixLevel] == 69, "level 70 * 96 / 98 = 69 (got %d)", out[kMixLevel]);
  it::check(out[kMixDelaySend] == 50, "delay 10 + 40 - 0 (got %d)", out[kMixDelaySend]);
  it::check(out[kMixReverbSend] == 127, "reverb 100 + 60 - 10 clamps (got %d)", out[kMixReverbSend]);
  it::check(out[kMixPan] == 20 && out[kMixDuck] == 77, "pan and duck are the player's");
  std::memcpy(player, def, sizeof(def));
  player[kMixPan] = 64;
  pm::combineMixer(player, inst, def, out);
  it::check(out[kMixLevel] == 96 && out[kMixDelaySend] == 40 && out[kMixReverbSend] == 60,
            "player at the defaults: exactly the instrument's values");
  inst[kMixReverbSend] = 0;
  player[kMixReverbSend] = 5;
  pm::combineMixer(player, inst, def, out);
  it::check(out[kMixReverbSend] == 0, "a send below its default subtracts, not below 0");
  std::memcpy(out, player, sizeof(out));
  pm::combineMixer(out, inst, def, out);   // in place
  it::check(out[kMixLevel] == 96, "in place");
}

// One bar of bass notes on pattern 0.
void bassPhrase(Project& p) {
  TrackPattern& tp = p.patterns[0].tracks[kTrackBass];
  const uint8_t notes[4] = {36, 43, 48, 31};
  for (int i = 0; i < 4; ++i) {
    Step& s = tp.steps[i * 4];
    s = kDefaultStep;
    s.setFlags(kStepOn | (i == 1 ? kStepAccent : 0));
    s.note = notes[i];
    s.gate = 6;
  }
}

uint64_t renderBass(long frames) {
  it::Stats st;
  it::render(g_inst, frames, st);
  return st.hash;
}

// An instrument track must sound exactly like a CLASSIC track holding the same
// effective values: the derived parameters and the combined mixer rows reach the voice.
void testReachesVoices() {
  struct Case { const char* track; const char* set; uint8_t pos[4]; uint8_t level, dly, rev; };
  const Case cases[] = {
      {"bass", "ACID", {150, 30, 0, 0}, 0, 0, 0},       // player at the defaults
      {"bass", "ACID", {150, 30, 0, 0}, 70, 20, 0},     // player turned down, a delay send
      {"lead", "VOYAGE", {200, 100, 90, 0}, 110, 10, 30},   // the instrument has sends of its own
  };
  const long frames = 44100L * 2 / kQuantum * kQuantum;
  for (const Case& c : cases) {
    const int t = pm::trackFromName(c.track);
    const pm::MacroSet* s = byName(t, c.set);
    it::check(s != nullptr, "%s exists", c.set);
    if (!s) continue;
    setup();
    Project& p = g_inst.project();
    for (int k = 0; k < kNumTracks; ++k) p.tracks[k].mute = k != t;
    bassPhrase(p);
    if (t != kTrackBass) {
      p.patterns[0].tracks[t] = p.patterns[0].tracks[kTrackBass];
      for (Step& st : p.patterns[0].tracks[t].steps) st.note = uint8_t(st.note + 24);
    }
    if (c.level) p.tracks[t].params[kMixLevel] = c.level;
    p.tracks[t].params[kMixDelaySend] = c.dly;
    p.tracks[t].params[kMixReverbSend] = c.rev;
    setInstrument(t, s, c.pos);
    quanta(1);
    uint8_t eff[kParamsPerTrack];
    engine_detail::effectiveParams(t, eff);
    uint8_t want[kParamsPerTrack], comb[kParamsPerTrack];
    derived(*s, c.pos, want);
    std::memcpy(comb, p.tracks[t].params, kParamsPerTrack);
    pm::combineMixer(comb, want, g_def[t], comb);
    it::check(std::memcmp(eff, comb, kParamsPerTrack) == 0, "%s: effective = derived + mixer rule", c.set);
    Command play{};
    play.type = CmdType::Play;
    g_inst.engine().post(play);
    const uint64_t a = renderBass(frames);
    // the same, CLASSIC, with the effective values stored by hand
    static Project copy;
    copy = p;
    setup();
    Project& q = g_inst.project();
    q = copy;
    setInstrumentId(q.tracks[t], 0);
    std::memcpy(q.tracks[t].params, eff, kParamsPerTrack);
    quanta(1);
    g_inst.engine().post(play);
    const uint64_t b = renderBass(frames);
    it::check(a == b, "%s %s: instrument and CLASSIC with its values differ", c.track, c.set);
  }
}

// ---- unknown id -------------------------------------------------------------------------
void testUnknown() {
  setup();
  Project& p = g_inst.project();
  uint16_t id = 0xBEEF;
  while (pm::findInstrument(id)) ++id;
  bassPhrase(p);
  for (int k = 0; k < kNumTracks; ++k) p.tracks[k].mute = k != kTrackBass;
  p.tracks[kTrackBass].params[0] = 99;
  const uint8_t pos[4] = {255, 255, 255, 255};
  g_inst.engine().post(setInstrumentCommand(kTrackBass, id, pos));
  quanta(1);
  Command play{};
  play.type = CmdType::Play;
  g_inst.engine().post(play);
  const uint64_t a = renderBass(44100L / kQuantum * kQuantum);
  it::check(instrumentId(p.tracks[kTrackBass]) == id, "unknown id kept");
  it::check(p.tracks[kTrackBass].params[0] == 99, "unknown id: the stored values play");
  it::check(engine_detail::faultCount() == 0, "no faults");
  // a known id of ANOTHER voice is as unknown on this track
  const pm::MacroSet* lead = pm::instrumentAt(kTrackLead, 0);
  g_inst.engine().post(setInstrumentCommand(kTrackBass, lead->id, pos));
  quanta(2);
  it::check(p.tracks[kTrackBass].params[0] == 99, "another voice's instrument is not applied");
  // same render as CLASSIC with those values
  static Project copy;
  copy = p;
  setup();
  g_inst.project() = copy;
  setInstrumentId(g_inst.project().tracks[kTrackBass], 0);
  quanta(1);
  g_inst.engine().post(play);
  const uint64_t b = renderBass(44100L / kQuantum * kQuantum);
  it::check(a == b, "unknown id renders as CLASSIC");
}

// ---- locks --------------------------------------------------------------------------------
// Runs quanta until the bass track's step changes; returns its effective params then.
struct StepWatch {
  int last = -1;
  bool next(int* step, uint8_t* eff, int maxQuanta = 4000) {
    int16_t buf[2 * kQuantum];
    for (int i = 0; i < maxQuanta; ++i) {
      g_inst.process(buf, kQuantum);
      const EngineStatus st = g_inst.engine().status();
      if (st.step[kTrackBass] != last || (i == 0 && last < 0)) {
        last = st.step[kTrackBass];
        *step = last;
        engine_detail::effectiveParams(kTrackBass, eff);
        return true;
      }
    }
    return false;
  }
};

// CLASSIC locks (an instrument track's notes carry macro values instead: itest_motion_notes).
void testLocks() {
  Command play{};
  play.type = CmdType::Play;
  // CLASSIC: a macro lock (version 2 numbering) is dropped, a sound lock works as before
  setup();
  Project& c = g_inst.project();
  TrackPattern& t4 = c.patterns[0].tracks[kTrackBass];
  t4.steps[0] = kDefaultStep; t4.steps[0].setFlags(kStepOn);
  t4.steps[0].setLock(kLockMacro0, 255);
  t4.steps[4] = kDefaultStep; t4.steps[4].setFlags(kStepOn);
  t4.steps[4].setLock(1, 3);
  g_inst.engine().post(play);
  StepWatch w4;
  int step;
  uint8_t eff[kParamsPerTrack];
  w4.next(&step, eff);
  it::check(std::memcmp(eff, c.tracks[kTrackBass].params, kParamsPerTrack) == 0, "CLASSIC ignores macro locks");
  for (int k = 0; k < 6 && step != 4; ++k) w4.next(&step, eff);
  it::check(step == 4 && eff[0] == 3, "CLASSIC sound lock works");
}

// ---- overlay ------------------------------------------------------------------------------
struct TestOverlay final : pm::LibraryOverlay {
  const pm::MacroSet* repl = nullptr;
  const pm::MacroSet* find(uint16_t id) const override { return repl && repl->id == id ? repl : nullptr; }
  int extraCount(int) const override { return 0; }
  const pm::MacroSet* extra(int, int) const override { return nullptr; }
};

void testOverlay() {
  setup();
  Project& p = g_inst.project();
  const pm::MacroSet* acid = byName(kTrackBass, "ACID");
  if (!acid) return;
  static pm::MacroSet mod;
  mod = *acid;
  mod.baseMask |= 1u << 9;    // SUB fixed at 3
  mod.base[9] = 3;
  TestOverlay ov;
  setInstrument(kTrackBass, acid);
  quanta(1);
  const uint8_t sub = p.tracks[kTrackBass].params[9];
  ov.repl = &mod;
  pm::setLibraryOverlay(&ov);
  it::check(pm::instrumentAt(kTrackBass, 0) == &mod, "the overlay replaces in the list");
  quanta(2);
  it::check(p.tracks[kTrackBass].params[9] == sub, "no refresh: the engine keeps its set");
  const uint8_t before = g_inst.engine().instrumentRefreshes();
  Command r{};
  r.type = CmdType::RefreshInstruments;
  r.a = 0xFF;
  g_inst.engine().post(r);
  quanta(1);
  it::check(uint8_t(g_inst.engine().instrumentRefreshes() - before) == 1, "refresh counted");
  it::check(p.tracks[kTrackBass].params[9] == 3, "refreshed: the replacement plays");
  // a refresh discarded by suspend() still counts (the old set may then be freed)
  g_inst.engine().suspend();
  g_inst.engine().post(r);
  quanta(1);
  g_inst.engine().resume();
  quanta(1);
  it::check(uint8_t(g_inst.engine().instrumentRefreshes() - before) == 2, "a discarded refresh counts");
  it::check(p.tracks[kTrackBass].params[9] == 3, "after resume: bound again through the overlay");
  ov.repl = nullptr;
  pm::setLibraryOverlay(nullptr);
  g_inst.engine().post(r);
  quanta(1);
  it::check(p.tracks[kTrackBass].params[9] == sub, "overlay gone: the built-in again");
}

// ---- copy ----------------------------------------------------------------------------------
void testCopy() {
  setup();
  setInstrument(kTrackBass, pm::instrumentAt(kTrackBass, 0));
  setInstrument(kTrackLead, pm::instrumentAt(kTrackLead, 0));
  quanta(1);
  Project& p = g_inst.project();
  Step& s = p.patterns[0].tracks[kTrackBass].steps[2];
  s.setFlags(kStepOn);
  s.setMacroValue(2, 9);
  Command c{};
  c.type = CmdType::CopyTrack;
  c.a = 0; c.b = kTrackBass; c.c = kTrackLead;
  g_inst.engine().post(c);
  quanta(1);
  it::check(p.patterns[0].tracks[kTrackLead].steps[2].hasExt() == false, "own macro values dropped on another track");
  it::check(p.patterns[0].tracks[kTrackBass].steps[2].macroValue(2) == macroCodeToPos(macroPosToCode(9)), "source keeps it");
  Step st = s;
  st.setMacroValue(3, 250);
  Command ss{};
  ss.type = CmdType::SetStep;
  ss.a = 0; ss.b = kTrackBass; ss.c = 5; ss.step = st;
  g_inst.engine().post(ss);
  quanta(1);
  it::check(p.patterns[0].tracks[kTrackBass].steps[5].macroValue(3) == macroCodeToPos(macroPosToCode(250)) &&
                p.patterns[0].tracks[kTrackBass].steps[5].macroValue(2) == macroCodeToPos(macroPosToCode(9)),
            "own macro values survive the sanitiser");
}

// ---- files ---------------------------------------------------------------------------------
void testFiles() {
  setup();
  Project& p = g_inst.project();
  for (int t = 0; t < kNumTracks; ++t) {
    const pm::MacroSet* s = pm::instrumentAt(t, pm::instrumentCount(t) - 1);
    const uint8_t pos[4] = {uint8_t(10 + t), 200, uint8_t(t * 30), 255};
    setInstrument(t, s, pos);
  }
  quanta(1);
  static Project saved;
  saved = p;
  pgtest::MemFs fs;
  it::check(io::saveProject(fs, io::kMountInt, "INST", p) == io::Status::Ok, "save");
  // load into a fresh engine the way the UI does
  setup();
  Project& q = g_inst.project();
  g_inst.engine().suspend();
  it::check(io::loadProject(fs, io::kMountInt, "INST", q) == io::Status::Ok, "load");
  g_inst.engine().sanitizeProject(q);
  g_inst.engine().resume();
  quanta(1);
  it::check(std::memcmp(&q, &saved, sizeof(Project)) == 0, "the project comes back byte for byte, re-derived the same");
  for (int t = 0; t < kNumTracks; ++t) {
    uint8_t want[kParamsPerTrack];
    derived(*pm::instrumentAt(t, pm::instrumentCount(t) - 1), saved.tracks[t].macro, want);
    it::check(std::memcmp(q.tracks[t].params, want, kSoundParams) == 0, "track %d bound after load", t);
  }
}

// ---- stems ---------------------------------------------------------------------------------
uint64_t bassStem(const Project& p) {
  bounce::Options o;
  o.pattern = 0;
  o.loops = 1;
  o.tailSec = 0.5f;
  o.sampleRate = 44100.0f;
  bounce::Plan plan;
  std::string err;
  if (!bounce::makePlan(p, o, plan, &err)) return 0;
  bounce::Pass pass;
  bounce::PassResult res;
  uint64_t h = 1469598103934665603ull;
  bounce::renderPass(
      g_inst, p, o, plan, pass, 0,
      [&](const bounce::Block& b) {
        for (int i = 0; i < b.n; ++i) {
          uint32_t u;
          std::memcpy(&u, &b.track[kTrackBass][0][i], 4);
          h = (h ^ u) * 1099511628211ull;
        }
      },
      res, &err);
  return h;
}

void testStems() {
  setup();
  static Project p;
  p = g_inst.project();
  bassPhrase(p);
  const pm::MacroSet* acid = byName(kTrackBass, "ACID");
  if (!acid) return;
  setInstrumentId(p.tracks[kTrackBass], acid->id);
  const uint8_t lo[4] = {20, 0, 0, 0}, hi[4] = {230, 0, 0, 0};
  std::memcpy(p.tracks[kTrackBass].macro, lo, 4);
  const uint64_t a = bassStem(p);
  std::memcpy(p.tracks[kTrackBass].macro, hi, 4);
  const uint64_t b = bassStem(p);
  it::check(a != b, "the bass stem follows the macro");
  // CLASSIC with the derived values stored: the same stem
  static Project c;
  c = p;
  setInstrumentId(c.tracks[kTrackBass], 0);
  uint8_t d[kParamsPerTrack];
  derived(*acid, hi, d);
  std::memcpy(c.tracks[kTrackBass].params, d, kSoundParams);
  const uint8_t lvl = c.tracks[kTrackBass].params[kMixLevel];
  pm::combineMixer(c.tracks[kTrackBass].params, d, g_def[kTrackBass], c.tracks[kTrackBass].params);
  it::check(bassStem(c) == b, "instrument stem = CLASSIC stem with its values (level %d -> %d)", lvl,
            c.tracks[kTrackBass].params[kMixLevel]);
}

}  // namespace

int main() {
  setup();
  testTable();
  testDerive();
  testMixerRule();
  testReachesVoices();
  testUnknown();
  testLocks();
  testOverlay();
  testCopy();
  testFiles();
  testStems();
  return it::finish("itest_instrument");
}
