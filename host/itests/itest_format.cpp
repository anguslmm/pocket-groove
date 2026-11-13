// Project version 3 (the packed Step, lib/pg/src/pg/project.h): version 1 and 2
// files, built here byte for byte in the old step layout (StepV2: note, flags,
// vel, gate, prob, chord, lockParam, lockValue), saved through pg::io, loaded
// and sanitised the way the UI does, come back as the expected version 3
// content; every field of a step survives; a v2 instrument track's macro lock
// becomes that macro's own value; locks that no longer exist are dropped;
// version 3 round-trips byte for byte; the accessors keep the fields apart.
#include <cstring>

#include "common/memfs.h"
#include "itests/itest_util.h"
#include "pg/io/project_io.h"
#include "pg/macro/macro.h"
#include "pg/params.h"

using namespace pg;

namespace {

Instrument g_inst;
Project g_old, g_want, g_got;

void putV2(Project& p, int pt, int t, int s, StepV2 o) { std::memcpy(&p.patterns[pt].tracks[t].steps[s], &o, sizeof(o)); }

// A version-2 file's content and what version 3 must make of it.
void build(int version) {
  Engine& eng = g_inst.engine();
  eng.initProject(g_old);
  const uint16_t acid = macro::instrumentAt(kTrackBass, 0)->id;
  const uint16_t lead = macro::instrumentAt(kTrackLead, 0)->id;
  if (version == 2) {
    setInstrumentId(g_old.tracks[kTrackBass], acid);
    setInstrumentId(g_old.tracks[kTrackLead], lead);
    for (int m = 0; m < kTrackMacros; ++m) g_old.tracks[kTrackBass].macro[m] = uint8_t(40 * m + 10);
  }
  // Every step in the old layout (initProject wrote v3 steps: rewrite all of them).
  for (int pt = 0; pt < kNumPatterns; ++pt)
    for (int t = 0; t < kNumTracks; ++t)
      for (int s = 0; s < kMaxSteps; ++s) putV2(g_old, pt, t, s, StepV2{60, 0, 100, 2, 100, 0, 0, 0});
  const uint8_t on = kStepOn, all = kStepOn | kStepAccent | kStepSlide | kStepRatchetMask;
  // bass (an instrument in v2)
  putV2(g_old, 0, kTrackBass, 0, StepV2{36, all, 127, 255, 37, 0, uint8_t(kLockMacro0 + 1), 200});   // macro lock
  putV2(g_old, 0, kTrackBass, 1, StepV2{37, on, 1, 1, 0, 0, 1, 99});                                 // sound lock
  putV2(g_old, 0, kTrackBass, 2, StepV2{38, on, 64, 9, 100, 0, uint8_t(kMixLevel + 1), 50});         // mixer lock
  putV2(g_old, 0, kTrackBass, 3, StepV2{39, on, 64, 9, 100, 0, uint8_t(kLockMacro0 + 3), 255});
  putV2(g_old, 5, kTrackBass, 63, StepV2{127, uint8_t(on | (3 << kStepRatchetShift)), 90, 4, 50, 0, uint8_t(kLockMacro0), 0});
  // lead: every chord, a macro lock with a chord
  for (int c = 0; c < kChordNum; ++c) putV2(g_old, 1, kTrackLead, c, StepV2{uint8_t(48 + c), on, 99, uint8_t(c + 1), uint8_t(c * 11), uint8_t(c), 0, 0});
  putV2(g_old, 1, kTrackLead, 20, StepV2{50, kStepOn | kStepAccent, 80, 3, 99, kChordMin9, uint8_t(kLockMacro0 + 3), 7});
  // drums (CLASSIC in both versions)
  putV2(g_old, 0, kTrackKick, 0, StepV2{60, on, 110, 2, 100, 0, 3, 50});                             // sound lock
  putV2(g_old, 0, kTrackKick, 1, StepV2{60, on, 110, 2, 100, 0, uint8_t(kLockMacro0 + 2), 77});      // macro lock: ignored
  putV2(g_old, 0, kTrackHatClosed, 4, StepV2{61, on, 70, 2, 80, 0, uint8_t(kMixPan + 1), 20});       // mixer lock
  g_old.version = uint8_t(version);

  // The expected version 3 steps.
  g_want = g_old;
  g_want.version = kProjectVersion;
  for (int pt = 0; pt < kNumPatterns; ++pt)
    for (int t = 0; t < kNumTracks; ++t)
      for (int s = 0; s < kMaxSteps; ++s) g_want.patterns[pt].tracks[t].steps[s] = kDefaultStep;
  auto W = [&](int pt, int t, int s) -> Step& { return g_want.patterns[pt].tracks[t].steps[s]; };
  auto mk = [](int note, int flags, int vel, int gate, int prob) {
    Step s = kDefaultStep;
    s.note = uint8_t(note); s.setFlags(flags); s.vel = uint8_t(vel); s.gate = uint8_t(gate); s.setProb(prob);
    return s;
  };
  const bool inst = version == 2;
  W(0, kTrackBass, 0) = mk(36, all, 127, 255, 37);
  if (inst) W(0, kTrackBass, 0).setMacroValue(1, 200);
  W(0, kTrackBass, 1) = mk(37, on, 1, 1, 0);
  if (!inst) W(0, kTrackBass, 1).setLock(1, 99);           // v1: CLASSIC, the lock plays
  W(0, kTrackBass, 2) = mk(38, on, 64, 9, 100);
  if (!inst) W(0, kTrackBass, 2).setLock(kMixLevel + 1, 50);
  W(0, kTrackBass, 3) = mk(39, on, 64, 9, 100);
  if (inst) W(0, kTrackBass, 3).setMacroValue(3, 255);
  W(5, kTrackBass, 63) = mk(127, on | (3 << kStepRatchetShift), 90, 4, 50);
  if (inst) W(5, kTrackBass, 63).setMacroValue(0, 0);
  for (int c = 0; c < kChordNum; ++c) {
    W(1, kTrackLead, c) = mk(48 + c, on, 99, c + 1, c * 11);
    W(1, kTrackLead, c).setChord(c);
  }
  W(1, kTrackLead, 20) = mk(50, kStepOn | kStepAccent, 80, 3, 99);
  W(1, kTrackLead, 20).setChord(kChordMin9);
  if (inst) W(1, kTrackLead, 20).setMacroValue(3, 7);
  W(0, kTrackKick, 0) = mk(60, on, 110, 2, 100);
  W(0, kTrackKick, 0).setLock(3, 50);
  W(0, kTrackKick, 1) = mk(60, on, 110, 2, 100);
  W(0, kTrackHatClosed, 4) = mk(61, on, 70, 2, 80);
  W(0, kTrackHatClosed, 4).setLock(kMixPan + 1, 20);
}

bool sameSteps(const Project& a, const Project& b, const char* what) {
  int bad = 0;
  for (int pt = 0; pt < kNumPatterns; ++pt)
    for (int t = 0; t < kNumTracks; ++t)
      for (int s = 0; s < kMaxSteps; ++s) {
        const Step& x = a.patterns[pt].tracks[t].steps[s];
        const Step& y = b.patterns[pt].tracks[t].steps[s];
        if (std::memcmp(&x, &y, sizeof(Step)) != 0) {
          if (++bad <= 6)
            std::fprintf(stderr, "  %s: pattern %d track %d step %d: %02x %02x %02x %02x %02x %02x %02x %02x, want %02x %02x %02x %02x %02x %02x %02x %02x\n",
                         what, pt, t, s, x.note, x.flagsRaw, x.vel, x.gate, x.probRaw, x.ext[0], x.ext[1], x.ext[2], y.note,
                         y.flagsRaw, y.vel, y.gate, y.probRaw, y.ext[0], y.ext[1], y.ext[2]);
        }
      }
  return bad == 0;
}

void testLoad(int version) {
  build(version);
  pgtest::MemFs fs;
  it::check(io::saveProject(fs, io::kMountInt, "OLD", g_old) == io::Status::Ok, "v%d save", version);
  io::HeaderInfo hi{};
  it::check(io::loadProject(fs, io::kMountInt, "OLD", g_got, &hi) == io::Status::Ok && hi.projectVersion == version,
            "v%d load", version);
  it::check(std::memcmp(&g_got, &g_old, sizeof(Project)) == 0, "v%d: io reads the bytes as written", version);
  g_inst.engine().sanitizeProject(g_got);
  it::check(g_got.version == kProjectVersion, "v%d becomes version %d", version, kProjectVersion);
  it::check(sameSteps(g_got, g_want, "load"), "v%d: the steps come back as the expected version 3 steps", version);
  it::check((instrumentId(g_got.tracks[kTrackBass]) != 0) == (version == 2), "v%d: the bass's instrument", version);
  if (version == 2)
    for (int m = 0; m < kTrackMacros; ++m)
      it::check(g_got.tracks[kTrackBass].macro[m] == uint8_t(40 * m + 10), "v2: base positions kept");
  // the accessors read back what the old fields said
  const Step& b0 = g_got.patterns[0].tracks[kTrackBass].steps[0];
  it::check(b0.note == 36 && b0.flags() == (kStepOn | kStepAccent | kStepSlide | kStepRatchetMask) && b0.vel == 127 &&
                b0.gate == 255 && b0.prob() == 37 && stepRatchet(b0) == 4 && b0.chord() == 0,
            "v%d: every field of a full step", version);
  if (version == 2) {
    it::check(!b0.hasMacro(0) && b0.hasMacro(1) && !b0.hasMacro(2) && !b0.hasMacro(3), "v2: only macro 2 has a value");
    it::check(b0.macroValue(1) == macroCodeToPos(macroPosToCode(200)) && std::abs(int(b0.macroValue(1)) - 200) <= 2,
              "v2: the lock's value, rounded to 63 levels (%d)", b0.macroValue(1));
  }
  // sanitising again changes nothing; a v3 save / load is byte for byte
  static Project again;
  again = g_got;
  g_inst.engine().sanitizeProject(again);
  it::check(std::memcmp(&again, &g_got, sizeof(Project)) == 0, "v%d: sanitising is idempotent", version);
  it::check(io::saveProject(fs, io::kMountInt, "NEW", g_got) == io::Status::Ok, "v3 save");
  it::check(io::loadProject(fs, io::kMountInt, "NEW", again, &hi) == io::Status::Ok && hi.projectVersion == kProjectVersion,
            "v3 load");
  it::check(std::memcmp(&again, &g_got, sizeof(Project)) == 0, "v3 round trip byte for byte");
  g_inst.engine().sanitizeProject(again);
  it::check(std::memcmp(&again, &g_got, sizeof(Project)) == 0, "v3 load + sanitise unchanged");
}

// The packing: every combination of the old fields survives; the four macro
// codes are independent of each other and of the rest.
void testAccessors() {
  int bad = 0;
  for (int c = 0; c < 16; ++c)
    for (int prob = 0; prob <= 127; prob += 3)
      for (int f = 0; f < 64; ++f) {
        Step s = kDefaultStep;
        s.setFlags(f);
        s.setChord(c);
        s.setProb(prob);
        s.setMacroValue(c & 3, prob * 2);
        if (s.flags() != (f & kStepFlagMask) || s.chord() != c || s.prob() != prob || s.note != 60 || s.vel != 100 ||
            s.gate != 2)
          ++bad;
        s.addFlags(kStepOn);
        s.clearFlags(kStepAccent);
        s.toggleFlags(kStepSlide);
        if (s.chord() != c || s.prob() != prob || !s.on()) ++bad;
      }
  it::check(bad == 0, "flags, chord and probability stay apart (%d bad)", bad);
  bad = 0;
  for (int pos = 0; pos <= 255; ++pos) {
    Step s = kDefaultStep;
    for (int m = 0; m < kTrackMacros; ++m) s.setMacroValue(m, (pos + 64 * m) & 255);
    for (int m = 0; m < kTrackMacros; ++m) {
      const int want = (pos + 64 * m) & 255;
      if (!s.hasMacro(m) || std::abs(int(s.macroValue(m)) - want) > 2) ++bad;
      if (macroPosToCode(s.macroValue(m)) != s.macroCode(m)) ++bad;   // a value written back is the same code
    }
    s.clearMacro(2);
    if (s.hasMacro(2) || !s.hasMacro(1) || !s.hasMacro(3)) ++bad;
  }
  it::check(bad == 0, "four macro codes, 63 levels, independent (%d bad)", bad);
  int levels = 0, last = -1;
  for (int code = 1; code <= kMacroCodeMax; ++code) {
    const int v = macroCodeToPos(code);
    if (v > last) ++levels;
    last = v;
  }
  it::check(levels == 63 && macroCodeToPos(1) == 0 && macroCodeToPos(kMacroCodeMax) == 255, "63 levels from 0 to 255");
}

}  // namespace

int main() {
  g_inst.init(44100.0f, false);
  testAccessors();
  testLoad(1);
  testLoad(2);
  return it::finish("itest_format");
}
