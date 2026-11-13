// System test: macro motion (engine.h "MACRO MOTION", docs/MACROLAB.md "What a
// moving parameter sounds like"). The bass track's voice is wrapped in a spy that
// records whether the engine sends it bytes (setParams) or fractions
// (setParamsFine), and what.
//
//   still       nothing moving: never a fractional call
//   stored      a SetMacro stores its byte and the 7-bit derivation at once
//   glide       the voice gets fractions that move steadily, start at the bytes it
//               was playing (no jump), keep enums and semitones whole
//   settle      after the ramp and the blend the voice is back on bytes, exactly the
//               stored parameters, and stays there
//   jump        a ramp of 1 never leaves the byte path
//   lock        a note's own value (one note: constant) wins over a move of the base
//   rebind      a new instrument during a motion goes straight back to bytes
//   ramp time   a ramp arrives when it says (the "motion" stage's contract)
#include <cmath>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "pg/macro/macro.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

class Spy final : public Voice {
 public:
  bass::BassVoice v;
  int byteCalls = 0, fineCalls = 0;
  bool lastFine = false;
  uint8_t bytes[kSoundParams] = {};
  float fine[kSoundParams] = {};
  std::vector<float> cut;         // the cutoff of every fractional call
  bool enumsWhole = true;

  void init(float sr) override { v.init(sr); }
  const ParamInfo* paramInfo(int i) const override { return v.paramInfo(i); }
  void setParams(const uint8_t* p) override {
    if (std::memcmp(p, bytes, kSoundParams) != 0 || lastFine) ++byteCalls;
    std::memcpy(bytes, p, kSoundParams);
    lastFine = false;
    v.setParams(p);
  }
  void setParamsFine(const float* p) override {
    ++fineCalls;
    lastFine = true;
    std::memcpy(fine, p, sizeof(fine));
    cut.push_back(p[bass::kCutoff]);
    for (int i : {int(bass::kLfoRate), int(bass::kFmRatio), int(bass::kLfoShape), int(bass::kSubOct), int(bass::kTune)})
      if (p[i] != std::floor(p[i])) enumsWhole = false;
    v.setParamsFine(p);
  }
  void setTempo(float bpm) override { v.setTempo(bpm); }
  void noteOn(const NoteEvent& e) override { v.noteOn(e); }
  void noteOff(uint8_t n) override { v.noteOff(n); }
  void choke() override { v.choke(); }
  void render(float* l, float* r, int n) override { v.render(l, r, n); }
  bool active() const override { return v.active(); }
  bool mono() const override { return true; }
};

Instrument g_inst;
Spy g_spy;
pm::ParamTable g_pt;
uint8_t g_def[kNumTracks][kParamsPerTrack];
const pm::MacroSet* g_acid = nullptr;
const int kRampQ = int(float(kMacroRampMs) * 0.001f * 44100.0f / float(kQuantum) + 0.5f);
const int kSettleQ = int(float(kMacroSettleMs) * 0.001f * 44100.0f / float(kQuantum) + 0.5f);

void quanta(int n) {
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) g_inst.process(buf, kQuantum);
}

void derived(const uint8_t* pos, uint8_t* out) {
  float f[4];
  for (int m = 0; m < 4; ++m) f[m] = pm::posFromByte(pos[m]);
  pm::evaluate(*g_acid, g_pt, f, g_def[kTrackBass], out);
}

void setup(const uint8_t* pos) {
  g_inst.init(44100.0f, false);
  g_spy = Spy();
  g_spy.init(44100.0f);
  g_inst.engine().setVoice(kTrackBass, &g_spy);
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) {
      g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
      g_def[t][p] = g_pt.info[t][p] ? g_pt.info[t][p]->def : 0;
    }
  for (int i = 0; i < pm::instrumentCount(kTrackBass); ++i)
    if (std::strcmp(pm::instrumentAt(kTrackBass, i)->name, "ACID") == 0) g_acid = pm::instrumentAt(kTrackBass, i);
  g_inst.engine().post(setInstrumentCommand(kTrackBass, g_acid->id, pos));
  Command n{};
  n.type = CmdType::NoteOn;
  n.a = kTrackBass;
  n.b = 45;
  n.c = 110;
  g_inst.engine().post(n);
  quanta(8);
}

bool storedIs(const uint8_t* pos) {
  uint8_t want[kParamsPerTrack];
  derived(pos, want);
  return std::memcmp(g_inst.project().tracks[kTrackBass].params, want, kSoundParams) == 0;
}

void testStillAndGlide() {
  const uint8_t pos[4] = {102, 0, 0, 255};
  setup(pos);
  quanta(200);
  it::check(g_spy.fineCalls == 0, "still: no fractional call while nothing moves (%d)", g_spy.fineCalls);
  it::check(storedIs(pos), "still: the voice plays the stored derivation");
  const float before = float(g_spy.bytes[bass::kCutoff]);

  g_inst.engine().post(setMacroCommand(kTrackBass, 0, 166));
  quanta(1);
  const uint8_t to[4] = {166, 0, 0, 255};
  it::check(g_inst.project().tracks[kTrackBass].macro[0] == 166, "stored: the byte is the target at once");
  it::check(storedIs(to), "stored: the parameters are the 7-bit derivation at the target at once");
  quanta(kRampQ - 1);
  it::check(g_spy.fineCalls > kRampQ / kFineQuanta - 4, "glide: fractional calls at the control rate (%d in %d quanta)",
            g_spy.fineCalls, kRampQ);
  it::check(!g_spy.cut.empty() && std::fabs(g_spy.cut.front() - before) < 0.1f,
            "glide: starts at the bytes it was playing (%.3f vs %.0f)", double(g_spy.cut.empty() ? -1.0f : g_spy.cut.front()),
            double(before));
  float maxStep = 0.0f;
  bool monotone = true, fractional = false;
  for (size_t i = 1; i < g_spy.cut.size(); ++i) {
    const float d = g_spy.cut[i] - g_spy.cut[i - 1];
    maxStep = std::max(maxStep, std::fabs(d));
    if (d < -1e-4f) monotone = false;
    if (g_spy.cut[i] != std::floor(g_spy.cut[i])) fractional = true;
  }
  it::check(monotone, "glide: OPEN up moves the cutoff up only");
  it::check(fractional, "glide: the cutoff takes fractional values");
  // Steady: no control tick moves much more than the average (the curve is smooth, not linear).
  const float avg = g_spy.cut.size() > 1 ? (g_spy.cut.back() - g_spy.cut.front()) / float(g_spy.cut.size() - 1) : 0.0f;
  it::check(maxStep < 2.5f * avg + 0.05f, "glide: moves steadily (largest step %.3f, average %.3f)", double(maxStep),
            double(avg));
  it::check(g_spy.enumsWhole, "glide: enums and TUNE stay whole numbers");

  quanta(kSettleQ + kFineEvalQuanta + 2 * kFineQuanta + 2);   // the ramp's end is seen at an interval's end
  it::check(!g_spy.lastFine, "settle: back on bytes after the ramp and the blend");
  it::check(std::memcmp(g_spy.bytes, g_inst.project().tracks[kTrackBass].params, kSoundParams) == 0,
            "settle: the bytes are the stored parameters");
  const int calls = g_spy.fineCalls;
  quanta(300);
  it::check(g_spy.fineCalls == calls && !g_spy.lastFine, "settle: stays on bytes");

  g_inst.engine().post(setMacroCommand(kTrackBass, 0, 102, kMacroJump));
  quanta(100);
  it::check(g_spy.fineCalls == calls, "jump: a ramp of 1 never leaves the byte path");
  it::check(storedIs(pos), "jump: stored at the target");
}

void testRampTime() {
  const uint8_t pos[4] = {0, 0, 0, 255};
  setup(pos);
  // A ramp of 1000 quanta to 255: halfway the cutoff is about the curve's halfway value.
  g_inst.engine().post(setMacroCommand(kTrackBass, 0, 255, 1000));
  quanta(500);
  uint8_t half[kParamsPerTrack];
  const uint8_t hp[4] = {128, 0, 0, 255};
  derived(hp, half);
  it::check(g_spy.lastFine && std::fabs(g_spy.fine[bass::kCutoff] - float(half[bass::kCutoff])) < 1.5f,
            "ramp time: halfway at half the ramp (%.2f vs %d)", double(g_spy.fine[bass::kCutoff]), half[bass::kCutoff]);
  quanta(500 + kFineQuanta);
  uint8_t end[kParamsPerTrack];
  const uint8_t ep[4] = {255, 0, 0, 255};
  derived(ep, end);
  it::check(std::fabs(g_spy.fine[bass::kCutoff] - float(end[bass::kCutoff])) < 0.6f,
            "ramp time: there at the end of the ramp (%.2f vs %d)", double(g_spy.fine[bass::kCutoff]), end[bass::kCutoff]);
}

void testLockWins() {
  const uint8_t pos[4] = {60, 0, 0, 255};
  setup(pos);
  // Step 0 of the bass: a note with a lock of OPEN at 230, the only step of the pattern.
  Command s{};
  s.type = CmdType::SetStep;
  s.a = 0;
  s.b = kTrackBass;
  s.c = 0;
  s.step = kDefaultStep;
  s.step.setFlags(kStepOn);
  s.step.note = 45;
  s.step.gate = 60;
  s.step.setMacroValue(0, 230);
  g_inst.engine().post(s);
  Command play{};
  play.type = CmdType::Play;
  g_inst.engine().post(play);
  quanta(4);
  // A long movement of OPEN the other way: the note's value holds (motion owns OPEN:
  // one note with its own value = constant); the move only changes the base.
  g_inst.engine().post(setMacroCommand(kTrackBass, 0, 0, 3000));
  quanta(400);
  uint8_t lk[kParamsPerTrack];
  const uint8_t lp[4] = {230, 0, 0, 255};
  derived(lp, lk);
  it::check(g_inst.project().tracks[kTrackBass].macro[0] == 0, "lock: the base moved (%d)", g_inst.project().tracks[kTrackBass].macro[0]);
  it::check(engine_detail::macroPosition(kTrackBass, 0) == 230.0f, "lock: OPEN stays at the note's value (%.2f)",
            double(engine_detail::macroPosition(kTrackBass, 0)));
  it::check(!g_spy.lastFine && g_spy.bytes[bass::kCutoff] == lk[bass::kCutoff],
            "lock: the 7-bit path plays the note's value (%d vs %d)", g_spy.bytes[bass::kCutoff], lk[bass::kCutoff]);
}

void testRebind() {
  const uint8_t pos[4] = {102, 0, 0, 255};
  setup(pos);
  g_inst.engine().post(setMacroCommand(kTrackBass, 0, 200, 2000));
  quanta(40);
  it::check(g_spy.lastFine, "rebind: moving");
  g_inst.engine().post(setInstrumentCommand(kTrackBass, g_acid->id, pos));
  quanta(2);
  it::check(!g_spy.lastFine, "rebind: SetInstrument goes straight back to bytes");
  it::check(storedIs(pos), "rebind: at the new positions");
  quanta(100);
  it::check(!g_spy.lastFine, "rebind: and stays there");
}

}  // namespace

int main() {
  testStillAndGlide();
  testRampTime();
  testLockWins();
  testRebind();
  return it::finish("itest_motion");
}
