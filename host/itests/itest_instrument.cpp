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
//   locks     macro lock scope at step time (until the next firing step, also
//             across the pattern wrap), sound locks ignored on an instrument
//             track, mixer locks combined; macro locks ignored on CLASSIC.
//   overlay   a host overlay replacing a built-in takes effect on
//             RefreshInstruments, counted also when discarded by suspend().
//   copy      CopyTrack drops macro locks on another track.
//   files     a project with instruments round-trips through pg::io and the
//             engine binds it again after suspend / load / resume.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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
  pm::evaluate(s, g_pt, f, g_def[s.track], out);
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
    s.flags = kStepOn | (i == 1 ? kStepAccent : 0);
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

void testLocks() {
  setup();
  Project& p = g_inst.project();
  const pm::MacroSet* acid = byName(kTrackBass, "ACID");
  if (!acid) return;
  TrackPattern& tp = p.patterns[0].tracks[kTrackBass];
  for (int s : {0, 4, 8, 12}) { tp.steps[s] = kDefaultStep; tp.steps[s].flags = kStepOn; tp.steps[s].note = 36; }
  tp.steps[4].lockParam = kLockMacro0 + 0;          // OPEN at 255 on step 5
  tp.steps[4].lockValue = 255;
  tp.steps[8].lockParam = 1;                        // CUT: ignored on an instrument track
  tp.steps[8].lockValue = 3;
  tp.steps[12].lockParam = kLockMacro0 + 1;         // BITE at 200 on step 13: its scope wraps
  tp.steps[12].lockValue = 200;
  const uint8_t pos[4] = {60, 0, 0, 0};
  setInstrument(kTrackBass, acid, pos);
  quanta(1);
  uint8_t base[kParamsPerTrack], lk0[kParamsPerTrack], lk1[kParamsPerTrack];
  derived(*acid, pos, base);
  uint8_t pl[4];
  std::memcpy(pl, pos, 4); pl[0] = 255; derived(*acid, pl, lk0);
  std::memcpy(pl, pos, 4); pl[1] = 200; derived(*acid, pl, lk1);
  it::check(std::memcmp(base, lk0, kSoundParams) != 0 && std::memcmp(base, lk1, kSoundParams) != 0, "locks change something");
  Command play{};
  play.type = CmdType::Play;
  g_inst.engine().post(play);
  StepWatch w;
  int seen = 0, bad = 0;
  for (int k = 0; k < 40; ++k) {
    int step;
    uint8_t eff[kParamsPerTrack];
    if (!w.next(&step, eff)) break;
    ++seen;
    // scope = from the step that fires with the lock until the next step that fires:
    // 5..8 OPEN locked, 9..12 the CUT lock (ignored: the base), 13..16 BITE locked, wrap: base
    const uint8_t* want = base;
    if (step >= 4 && step < 8) want = lk0;
    if (step >= 12) want = lk1;
    if (std::memcmp(eff, want, kSoundParams) != 0) ++bad;
  }
  it::check(seen >= 32, "steps seen %d", seen);
  it::check(bad == 0, "macro lock scope: %d steps with the wrong values", bad);
  // the wrap explicitly: lock only on step 13, steps 1 and 13 on
  setup();
  Project& q = g_inst.project();
  TrackPattern& t2 = q.patterns[0].tracks[kTrackBass];
  t2.steps[0] = kDefaultStep; t2.steps[0].flags = kStepOn;
  t2.steps[12] = kDefaultStep; t2.steps[12].flags = kStepOn;
  t2.steps[12].lockParam = kLockMacro0 + 1;
  t2.steps[12].lockValue = 200;
  setInstrument(kTrackBass, acid, pos);
  g_inst.engine().post(play);
  StepWatch w2;
  int wrapBad = 0, wraps = 0;
  bool inLock = false;
  for (int k = 0; k < 48; ++k) {
    int step;
    uint8_t eff[kParamsPerTrack];
    if (!w2.next(&step, eff)) break;
    if (step == 12) inLock = true;
    if (step == 0 && inLock) { inLock = false; ++wraps; }
    const uint8_t* want = inLock ? lk1 : base;
    if (std::memcmp(eff, want, kSoundParams) != 0) ++wrapBad;
  }
  it::check(wraps >= 2 && wrapBad == 0, "lock across the wrap: %d wraps, %d wrong", wraps, wrapBad);

  // mixer lock on an instrument track: the player's value at the step, combined
  setup();
  Project& r = g_inst.project();
  TrackPattern& t3 = r.patterns[0].tracks[kTrackBass];
  t3.steps[0] = kDefaultStep; t3.steps[0].flags = kStepOn;
  t3.steps[0].lockParam = kMixLevel + 1;
  t3.steps[0].lockValue = 50;
  const uint8_t loud[4] = {153, 0, 0, 0};   // ACID OPEN 0.6: lvl 96
  setInstrument(kTrackBass, acid, loud);
  g_inst.engine().post(play);
  StepWatch w3;
  int step;
  uint8_t eff[kParamsPerTrack];
  w3.next(&step, eff);
  uint8_t d[kParamsPerTrack];
  derived(*acid, loud, d);
  uint8_t player[kParamsPerTrack];
  std::memcpy(player, r.tracks[kTrackBass].params, kParamsPerTrack);
  player[kMixLevel] = 50;
  pm::combineMixer(player, d, g_def[kTrackBass], player);
  it::check(eff[kMixLevel] == player[kMixLevel], "mixer lock combined: %d want %d", eff[kMixLevel], player[kMixLevel]);

  // CLASSIC: a macro lock does nothing, a sound lock works as before
  setup();
  Project& c = g_inst.project();
  TrackPattern& t4 = c.patterns[0].tracks[kTrackBass];
  t4.steps[0] = kDefaultStep; t4.steps[0].flags = kStepOn;
  t4.steps[0].lockParam = kLockMacro0;
  t4.steps[0].lockValue = 255;
  t4.steps[4] = kDefaultStep; t4.steps[4].flags = kStepOn;
  t4.steps[4].lockParam = 1;
  t4.steps[4].lockValue = 3;
  g_inst.engine().post(play);
  StepWatch w4;
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
  Project& p = g_inst.project();
  Step& s = p.patterns[0].tracks[kTrackBass].steps[2];
  s.flags = kStepOn;
  s.lockParam = kLockMacro0 + 2;
  s.lockValue = 9;
  Command c{};
  c.type = CmdType::CopyTrack;
  c.a = 0; c.b = kTrackBass; c.c = kTrackLead;
  g_inst.engine().post(c);
  quanta(1);
  it::check(p.patterns[0].tracks[kTrackLead].steps[2].lockParam == 0, "macro lock dropped on another track");
  it::check(p.patterns[0].tracks[kTrackBass].steps[2].lockParam == kLockMacro0 + 2, "source keeps it");
  Step st = s;
  st.lockParam = kLockMacro0 + 3;
  st.lockValue = 250;
  Command ss{};
  ss.type = CmdType::SetStep;
  ss.a = 0; ss.b = kTrackBass; ss.c = 5; ss.step = st;
  g_inst.engine().post(ss);
  quanta(1);
  it::check(p.patterns[0].tracks[kTrackBass].steps[5].lockParam == kLockMacro0 + 3 &&
                p.patterns[0].tracks[kTrackBass].steps[5].lockValue == 250,
            "a macro lock survives the sanitiser with any value");
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
  return it::finish("itest_instrument");
}
