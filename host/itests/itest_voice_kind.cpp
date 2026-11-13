// Voice kinds (voice.h VOICE KINDS, engine.h "INSTRUMENTS"): an instrument's set
// names the kind of voice that renders it (`voice perc sampler`), a track hosts a
// second voice besides its native one (Engine::setAltVoice), and the engine
// switches at a quantum edge when the instrument changes kind.
//
//   parse     `voice perc sampler` reads the sampler's parameter names, round-trips
//             through format(), is refused on a track that cannot host it, and an
//             unknown kind is an error; the id is still voice:NAME (perc).
//   switch    native -> other kind mid-pattern: the other voice gets the notes from
//             the next step, the native one is choked and fades (rendered until it
//             is silent), the stored params are the other kind's derivation; back to
//             the native kind: native params again; to CLASSIC from the other kind:
//             the native defaults. No NaN, no jump in the output at the switches.
//   unknown   a set whose kind the track does not host (no alt voice registered) is
//             as an unknown id: the native voice plays the stored parameters.
//   sanitize  a loaded project naming such a set gets the other kind's parameters
//             derived when the track hosts it, the native ranges when it does not.
//   ident     with the alt voice registered and no instrument of that kind in use, a
//             render is bit-identical to one without it.
#include <cmath>
#include <cstring>

#include "itests/itest_util.h"
#include "pg/macro/macro.h"
#include "pg/sampler/sampler_params.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

// A stand-in for the sampler: the sampler's parameter table, a sine at the note
// for as long as the gate, with a 2 ms choke fade. Counts what the engine asks.
class FakeVoice final : public Voice {
 public:
  int notes = 0, offs = 0, chokes = 0, renders = 0, rendersAfterChoke = 0, setParamsCalls = 0;
  uint8_t last[kSoundParams] = {};
  void init(float sr) override { sr_ = sr; }
  const ParamInfo* paramInfo(int i) const override { return sampler::paramInfo(i); }
  void setParams(const uint8_t* p) override {
    ++setParamsCalls;
    std::memcpy(last, p, kSoundParams);
  }
  void noteOn(const NoteEvent& e) override {
    ++notes;
    inc_ = 440.0f * std::pow(2.0f, float(int(e.note) - 69) / 12.0f) / sr_;
    gain_ = 0.3f;
    choking_ = false;
    active_ = true;
  }
  void noteOff(uint8_t) override { ++offs; choke(); }
  void choke() override {
    ++chokes;
    if (active_) choking_ = true;
  }
  void render(float* L, float* R, int n) override {
    ++renders;
    if (choking_) ++rendersAfterChoke;
    for (int i = 0; i < n; ++i) {
      if (choking_) {
        gain_ -= 0.3f / (0.002f * sr_);
        if (gain_ <= 0.0f) { gain_ = 0.0f; active_ = false; }
      }
      ph_ += inc_;
      if (ph_ >= 1.0f) ph_ -= 1.0f;
      L[i] = R[i] = gain_ * std::sin(6.2831853f * ph_);
    }
  }
  bool active() const override { return active_; }
  bool mono() const override { return true; }

 private:
  float sr_ = 44100.0f, inc_ = 0.0f, ph_ = 0.0f, gain_ = 0.0f;
  bool active_ = false, choking_ = false;
};

Instrument g_inst;
FakeVoice g_fake;
pm::ParamTable g_pt;

// The library overlay: one sampler-kind set for the perc track.
struct Overlay final : pm::LibraryOverlay {
  pm::MacroSet set;
  const pm::MacroSet* find(uint16_t id) const override { return id == set.id ? &set : nullptr; }
  int extraCount(int track) const override { return track == kTrackPerc ? 1 : 0; }
  const pm::MacroSet* extra(int track, int i) const override { return track == kTrackPerc && i == 0 ? &set : nullptr; }
};
Overlay g_ov;

const char kSet[] =
    "# CHIRP -- a test set of the sampler kind\n"
    "voice perc sampler\n"
    "name CHIRP\n"
    "base slices=8 cutoff=90 filter=bp\n"
    "macro PICK 0.5\n"
    "  slice 0:0 1:7 step\n"
    "  pitch 0:52 1:76\n"
    "macro DIRT\n"
    "  bits 0:0 1:100\n";

void tables() {
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
}

void quanta(int n, it::Stats* st = nullptr) {
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) {
    g_inst.process(buf, kQuantum);
    if (st) st->add(buf, kQuantum);
  }
}

const pm::MacroSet* builtinNamed(int track, const char* name) {
  for (int i = 0; i < pm::instrumentCount(track); ++i)
    if (std::strcmp(pm::instrumentAt(track, i)->name, name) == 0) return pm::instrumentAt(track, i);
  return nullptr;
}

void setInst(int t, const pm::MacroSet* s) {
  uint8_t p[4] = {0, 0, 0, 0};
  for (int m = 0; m < 4 && s; ++m) p[m] = m < s->numMacros ? pm::startByte(s->macros[m]) : 0;
  g_inst.engine().post(setInstrumentCommand(t, s ? s->id : 0, p));
}

// Perc on every 4th step, kick on every step; playing.
void pattern() {
  Engine& e = g_inst.engine();
  for (int i = 0; i < 16; ++i) {
    Step s = kDefaultStep;
    s.setFlags(kStepOn);
    s.gate = 3;
    if (i % 4 == 0) {
      s.note = uint8_t(60 + i / 4);
      Command c{CmdType::SetStep, 0, uint8_t(kTrackPerc), uint8_t(i), s, nullptr};
      e.post(c);
    }
  }
  e.post(Command{CmdType::SetGlobal, uint8_t(kGTempo), 0, 120, kDefaultStep, nullptr});
}

void play() { g_inst.engine().post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr}); }

// ---- parse ----------------------------------------------------------------------
void testParse() {
  pm::MacroSet s;
  pm::ParseError e;
  const bool ok = pm::parse(kSet, sizeof(kSet) - 1, g_pt, &s, &e);
  it::check(ok, "the sampler set parses (%d: %s)", e.line, e.msg);
  if (!ok) return;
  it::check(s.voice == kVoiceSampler && s.track == kTrackPerc, "kind sampler on perc");
  it::check(s.id == pm::setId(kTrackPerc, "CHIRP"), "the id is still perc:NAME");
  it::check(((s.baseMask >> sampler::kSlices) & 1u) && s.base[sampler::kSlices] == 8, "base slices=8 (a sampler name)");
  it::check(s.base[sampler::kFilter] == sampler::kBP, "filter=bp (a sampler enum)");
  char buf[4096];
  const size_t n = pm::format(s, g_pt, buf, sizeof(buf));
  it::check(n > 0 && std::strstr(buf, "voice perc sampler\n") != nullptr, "format writes the kind");
  pm::MacroSet r;
  it::check(pm::parse(buf, n, g_pt, &r, &e) && std::memcmp(&r, &s, sizeof(s)) == 0, "format round-trips (%s)", e.msg);
  // evaluation reads the kind's ranges: slice 0..15, pitch 16..112
  uint8_t out[kParamsPerTrack], under[kParamsPerTrack];
  const ParamInfo* scratch[kParamsPerTrack];
  const pm::TrackParams kp = pm::paramsFor(s, g_pt, scratch);
  for (int i = 0; i < kParamsPerTrack; ++i) under[i] = kp[i] ? kp[i]->def : 0;
  const float pos[4] = {1.0f, 1.0f, 0, 0};
  pm::evaluate(s, g_pt, pos, under, out);
  it::check(out[sampler::kSlice] == 7 && out[sampler::kPitch] == 76 && out[sampler::kBits] == 100, "evaluates in the kind's terms");
  // errors
  const char bad1[] = "voice bass sampler\nname X\n";
  it::check(!pm::parse(bad1, sizeof(bad1) - 1, g_pt, &r, &e) && std::strstr(e.msg, "cannot go on bass"), "not on bass: %s",
            e.msg);
  const char bad2[] = "voice perc granular\nname X\n";
  it::check(!pm::parse(bad2, sizeof(bad2) - 1, g_pt, &r, &e) && std::strstr(e.msg, "unknown voice kind"), "unknown kind: %s",
            e.msg);
  const char bad3[] = "voice perc sampler\nname X\nmacro A\n  modl 0:tom 1:rim\n";
  it::check(!pm::parse(bad3, sizeof(bad3) - 1, g_pt, &r, &e) && std::strstr(e.msg, "unknown parameter"),
            "a native perc parameter is not a sampler's: %s", e.msg);
  const char nat[] = "voice perc\nname Y\nmacro A\n  modl 0:tom 1:rim\n";
  it::check(pm::parse(nat, sizeof(nat) - 1, g_pt, &r, &e) && r.voice == kVoiceNative, "the plain voice line: native");
}

// ---- switch ----------------------------------------------------------------------
void testSwitch() {
  Engine& e = g_inst.engine();
  Project& p = g_inst.project();
  const pm::MacroSet* tap = builtinNamed(kTrackPerc, "TAP");
  it::check(tap && tap->voice == kVoiceNative, "TAP is a native perc instrument");
  if (!tap) return;
  setInst(kTrackPerc, tap);
  pattern();
  play();
  it::Stats st;
  quanta(2000, &st);   // ~0.73 s: a few perc hits
  it::check(g_fake.notes == 0, "native: the hosted voice gets no notes (%d)", g_fake.notes);
  uint8_t tapParams[kSoundParams];
  std::memcpy(tapParams, p.tracks[kTrackPerc].params, kSoundParams);

  // to the sampler kind, mid-pattern
  setInst(kTrackPerc, &g_ov.set);
  quanta(1);
  it::check(std::memcmp(g_fake.last, p.tracks[kTrackPerc].params, kSoundParams) == 0 || g_fake.setParamsCalls > 0,
            "the hosted voice is fed");
  it::check(p.tracks[kTrackPerc].params[sampler::kSlices] == 8, "params derived for the sampler kind (slices %d)",
            p.tracks[kTrackPerc].params[sampler::kSlices]);
  it::check(p.tracks[kTrackPerc].params[sampler::kFilter] == sampler::kBP, "filter BP");
  quanta(8);
  it::check(!g_inst.voice(kTrackPerc)->active(), "the native perc voice has faded out after the switch");
  quanta(4000, &st);
  it::check(g_fake.notes >= 3, "the hosted voice plays the steps (%d notes)", g_fake.notes);
  it::check(std::memcmp(g_fake.last, p.tracks[kTrackPerc].params, kSoundParams) == 0, "it gets the derived parameters");
  const int sampNotes = g_fake.notes;

  // back to the native kind
  setInst(kTrackPerc, tap);
  quanta(1);
  it::check(std::memcmp(p.tracks[kTrackPerc].params, tapParams, kSoundParams) == 0, "native params again");
  for (int i = 0; i < kSoundParams; ++i)
    if (p.tracks[kTrackPerc].params[i] != tapParams[i]) std::printf("  param %d: %d vs %d\n", i, p.tracks[kTrackPerc].params[i], tapParams[i]);
  quanta(200);
  it::check(g_fake.rendersAfterChoke > 0 || !g_fake.active(), "the hosted voice fades after the switch back");
  it::check(!g_fake.active(), "and is silent");
  quanta(4000, &st);
  it::check(g_fake.notes == sampNotes, "no more notes to the hosted voice");

  // to CLASSIC straight from the sampler kind: the native defaults
  setInst(kTrackPerc, &g_ov.set);
  quanta(2000, &st);
  setInst(kTrackPerc, nullptr);
  quanta(1);
  bool defaults = true;
  for (int i = 0; i < kSoundParams; ++i) {
    const ParamInfo* in = e.trackParamInfo(kTrackPerc, i);
    defaults &= p.tracks[kTrackPerc].params[i] == (in ? in->def : 0);
  }
  it::check(defaults, "CLASSIC after the sampler kind: the native defaults");
  quanta(2000, &st);
  it::check(st.peak > 1000 && st.minInt == 0, "it played (peak %d)", st.peak);
  // the switches never make a step bigger than the hits themselves do
  it::check(st.maxJump < 20000, "no wild jump at the switches (%d)", st.maxJump);
  e.post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
  quanta(10);
}

// ---- unknown / sanitize -------------------------------------------------------------
void testUnknownAndSanitize() {
  Engine& e = g_inst.engine();
  Project& p = g_inst.project();
  // With the hosted voice removed the set's kind is not hosted: as unknown.
  e.setAltVoice(kTrackPerc, kVoiceSampler, nullptr);
  it::check(!e.hostsKind(kTrackPerc, kVoiceSampler) && e.hostsKind(kTrackPerc, kVoiceNative), "hostsKind");
  e.suspend();
  setInstrumentId(p.tracks[kTrackPerc], g_ov.set.id);
  for (int i = 0; i < kSoundParams; ++i) p.tracks[kTrackPerc].params[i] = 120;
  e.sanitizeProject(p);
  bool nativeRanges = true;
  for (int i = 0; i < kSoundParams; ++i) {
    const ParamInfo* in = e.trackParamInfo(kTrackPerc, i);
    nativeRanges &= in ? p.tracks[kTrackPerc].params[i] <= in->max : p.tracks[kTrackPerc].params[i] == 0;
  }
  it::check(nativeRanges, "not hosted: clamped to the native ranges");
  it::check(instrumentId(p.tracks[kTrackPerc]) == g_ov.set.id, "the id is kept");
  e.resume();
  const int before = g_fake.notes;
  play();
  quanta(3000);
  it::check(g_fake.notes == before, "not hosted: nothing reaches the other voice");
  e.post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
  quanta(4);
  // Hosted again: the sanitiser derives the kind's parameters.
  e.suspend();
  e.setAltVoice(kTrackPerc, kVoiceSampler, &g_fake);
  e.sanitizeProject(p);
  it::check(p.tracks[kTrackPerc].params[sampler::kSlices] == 8 && p.tracks[kTrackPerc].params[sampler::kFilter] == sampler::kBP,
            "hosted: the sanitiser derives the sampler parameters");
  e.resume();
  play();
  quanta(3000);
  it::check(g_fake.notes > before, "hosted: the other voice plays after the load");
  e.post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
  quanta(4);
}

// ---- ident ----------------------------------------------------------------------
uint64_t renderTapProject(bool hosted) {
  g_inst.init(44100.0f, false);
  g_inst.engine().setAltVoice(kTrackPerc, kVoiceSampler, hosted ? &g_fake : nullptr);
  engine_detail::setRngSeed(1234);
  setInst(kTrackPerc, builtinNamed(kTrackPerc, "TAP"));
  pattern();
  play();
  it::Stats st;
  quanta(6000, &st);
  return st.hash;
}

void testIdentical() {
  const uint64_t a = renderTapProject(false), b = renderTapProject(true);
  it::check(a == b, "a render with the hosted voice unused is bit-identical (%016llx vs %016llx)",
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
}

}  // namespace

int main() {
  g_inst.init(44100.0f, false);
  g_fake.init(44100.0f);
  tables();
  pm::ParseError e;
  if (!pm::parse(kSet, sizeof(kSet) - 1, g_pt, &g_ov.set, &e)) {
    std::fprintf(stderr, "the test set does not parse: %d %s\n", e.line, e.msg);
    return 1;
  }
  pm::setLibraryOverlay(&g_ov);
  testParse();
  testIdentical();
  g_inst.init(44100.0f, false);
  g_inst.engine().setAltVoice(kTrackPerc, kVoiceSampler, &g_fake);
  testSwitch();
  testUnknownAndSanitize();
  pm::setLibraryOverlay(nullptr);
  return it::finish("itest_voice_kind");
}
