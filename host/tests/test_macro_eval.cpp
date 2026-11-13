// pg::macro (lib/pg/src/pg/macro, docs/MACROLAB.md): the evaluator and the
// .pgm parser against a small fake parameter table, so this test links the
// macro module alone. The real tables and the starter files in macros/ are
// checked by host/itests/itest_macro_files.cpp.
//
//   curves: linear, smooth, step, edge positions, single point, jumps (equal
//           positions), non-monotonic; set / add / mul combine in file order;
//           "by" scaling for each op; base patch vs. the values underneath;
//           clamping to ParamInfo ranges and rounding; owned mask.
//   parser: every feature, every error path with its line number, the limits
//           (macros, targets, points, name lengths, line length, decimals),
//           and format() -> parse() round trips.
//   model v2: the packed point encoding (exact, and decoding as v1 parsed);
//           the pools and every limit (64 targets / 32 per macro, 192 points /
//           16 per curve, patches, morph and trim lines, string lengths);
//           morph (one, two combined on the pad, amount curves, enum switch
//           points by default / per line / per value, targets after morphs);
//           end labels, descriptions and the about line; warnings (a plain set
//           undoing an earlier line or another macro's morph, `override`, an
//           unused patch); trims (dB through the cubic taper, by, file order);
//           enum jumps; v2 round trips; and the time one large evaluation takes.
// Bit-exact compatibility with the 24 round-1 files: host/itests/itest_macro_pin.cpp.
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <string>

#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::macro;

namespace {

int gFail = 0, gChecks = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    ++gChecks;                                                           \
    if (!(c)) {                                                          \
      ++gFail;                                                           \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                                    \
  } while (0)

const char* const kWaves[] = {"SAW", "SQR", "PULS"};
const char* const kRates[] = {"1/4", "1/4.", "1/8", "1/16"};
// The fake "bass" track: CUT 0..127, RES 0..127, WAVE enum, RATE enum, TUNE 0..48, LVL at 24.
const ParamInfo kCut{"CUT", "Cutoff", 0, 127, 46, ParamFormat::Number, nullptr};
const ParamInfo kRes{"RES", "Resonance", 0, 127, 78, ParamFormat::Number, nullptr};
const ParamInfo kWave{"WAVE", "Osc Wave", 0, 2, 0, ParamFormat::Enum, kWaves};
const ParamInfo kRate{"RATE", "LFO Rate", 0, 3, 2, ParamFormat::Enum, kRates};
const ParamInfo kTune{"TUNE", "Osc Tune", 0, 48, 24, ParamFormat::Semitones, nullptr};
const ParamInfo kEnv{"ENV", "Env Mod", 0, 127, 70, ParamFormat::Number, nullptr};
const ParamInfo kLvl{"LVL", "Level", 0, 127, 100, ParamFormat::Number, nullptr};

// Indices 6..23: plain 0..127 parameters G6 .. G23 ("Gen 6" ..), so patches can be long.
char gGenNames[kParamsPerTrack][2][12];
ParamInfo gGen[kParamsPerTrack];

ParamTable table() {
  ParamTable t;
  std::memset(&t, 0, sizeof(t));
  for (int p = 6; p < 24; ++p) {
    std::snprintf(gGenNames[p][0], sizeof(gGenNames[p][0]), "G%d", p);
    std::snprintf(gGenNames[p][1], sizeof(gGenNames[p][1]), "Gen %d", p);
    gGen[p] = ParamInfo{gGenNames[p][0], gGenNames[p][1], 0, 127, 64, ParamFormat::Number, nullptr};
  }
  for (int tr = 0; tr < kNumTracks; ++tr) {
    for (int p = 6; p < 24; ++p) t.info[tr][p] = &gGen[p];
    t.info[tr][0] = &kCut;
    t.info[tr][1] = &kRes;
    t.info[tr][2] = &kWave;
    t.info[tr][3] = &kRate;
    t.info[tr][4] = &kTune;
    t.info[tr][5] = &kEnv;
    t.info[tr][24] = &kLvl;
  }
  return t;
}
const ParamTable gT = table();

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

bool parseStr(const char* s, MacroSet* set, ParseError* e) { return parse(s, std::strlen(s), gT, set, e); }

MacroSet must(const char* s) {
  MacroSet set;
  ParseError e;
  if (!parseStr(s, &set, &e)) {
    ++gFail;
    std::printf("  FAIL: did not parse (line %d: %s):\n%s\n", e.line, e.msg, s);
  }
  return set;
}

// The parse must fail at `line` with a message containing `part`.
void expectError(const char* text, int line, const char* part) {
  MacroSet set;
  ParseError e;
  ++gChecks;
  const bool ok = parseStr(text, &set, &e);
  if (ok || e.line != line || !std::strstr(e.msg, part)) {
    ++gFail;
    std::printf("  FAIL error case: want line %d '%s', got %s line %d '%s'\n----\n%s\n----\n", line, part,
                ok ? "SUCCESS" : "error", e.line, e.msg, text);
  }
}

uint8_t gUnder[kParamsPerTrack];

float pos(const MacroSet& s, const Target& t, int k) { return pointPos(s.points[t.first + k]); }
float val(const MacroSet& s, const Target& t, int k) { return pointVal(s.points[t.first + k]); }

void eval(const MacroSet& s, float a, float b, uint8_t* out, float c = 0.0f, float d = 0.0f) {
  const float pos[4] = {a, b, c, d};
  evaluate(s, gT, pos, gUnder, out);
}

// ---------------------------------------------------------------------------------------------
// Packs (pos, val) pairs given as floats with at most 3 decimals.
void pts(Point* o, std::initializer_list<float> pv) {
  int i = 0;
  float pos = 0.0f;
  for (float x : pv) {
    if (i % 2 == 0) pos = x;
    else o[i / 2] = packPoint(int32_t(std::lround(pos * 1000.0f)), int32_t(std::lround(x * 1000.0f)));
    ++i;
  }
}

void testCurves() {
  std::printf("curves\n");
  Point c[8];
  pts(c, {0.2f, 10.0f, 0.6f, 110.0f, 1.0f, 80.0f});   // non-monotonic: peaks at 60 %
  auto at = [&](int n, Interp ip, float x) { return curveAt(c, n, ip, x); };
  CHECK(near(at(3, Interp::Linear, 0.0f), 10.0f));     // flat before the first point
  CHECK(near(at(3, Interp::Linear, -3.0f), 10.0f));    // positions clamp
  CHECK(near(at(3, Interp::Linear, 0.2f), 10.0f));
  CHECK(near(at(3, Interp::Linear, 0.4f), 60.0f));
  CHECK(near(at(3, Interp::Linear, 0.6f), 110.0f));
  CHECK(near(at(3, Interp::Linear, 0.8f), 95.0f));
  CHECK(near(at(3, Interp::Linear, 1.0f), 80.0f));
  CHECK(near(at(3, Interp::Linear, 7.0f), 80.0f));
  CHECK(near(at(3, Interp::Smooth, 0.4f), 60.0f));     // smoothstep is symmetric: same midpoint
  CHECK(near(at(3, Interp::Smooth, 0.3f), 10.0f + 100.0f * 0.15625f));   // t = 0.25 -> 0.15625
  CHECK(near(at(3, Interp::Smooth, 0.2f), 10.0f) && near(at(3, Interp::Smooth, 0.6f), 110.0f));
  CHECK(near(at(3, Interp::Step, 0.1f), 10.0f));
  CHECK(near(at(3, Interp::Step, 0.59f), 10.0f));
  CHECK(near(at(3, Interp::Step, 0.6f), 110.0f));    // a point's own position takes its value
  CHECK(near(at(3, Interp::Step, 0.99f), 110.0f));
  CHECK(near(at(3, Interp::Step, 1.0f), 80.0f));
  // one point: constant
  CHECK(near(at(1, Interp::Linear, 0.0f), 10.0f) && near(at(1, Interp::Linear, 1.0f), 10.0f));
  // equal positions: a jump
  pts(c, {0.0f, 0.0f, 0.5f, 50.0f, 0.5f, 100.0f, 1.0f, 0.0f});
  CHECK(near(at(4, Interp::Linear, 0.25f), 25.0f));
  CHECK(near(at(4, Interp::Linear, 0.4999f), 49.99f, 1e-2f));
  CHECK(near(at(4, Interp::Linear, 0.5f), 100.0f));
  CHECK(near(at(4, Interp::Linear, 0.75f), 50.0f));
  // all points at one position
  pts(c, {0.5f, 10.0f, 0.5f, 20.0f});
  CHECK(near(at(2, Interp::Linear, 0.2f), 10.0f) && near(at(2, Interp::Linear, 0.5f), 20.0f) &&
        near(at(2, Interp::Linear, 0.9f), 20.0f));
  // the packed encoding: exact thousandths, both signs, the full value range
  const int32_t vals[] = {0, 1, -1, 999, -1500, 126999, 255000, -255000, kPointValMax, -kPointValMax};
  for (int32_t v : vals)
    for (int32_t p : {0, 1, 500, 999, 1000}) {
      const Point q = packPoint(p, v);
      CHECK(pointPosMilli(q) == p && pointValMilli(q) == v);
    }
  // decoding gives exactly what v1 parsed: integer part + fraction / 10^decimals
  CHECK(milliToFloat(300) == 3.0f / 10.0f && milliToFloat(125) == 125.0f / 1000.0f);
  CHECK(milliToFloat(-62500) == -(62.0f + 5.0f / 10.0f) && milliToFloat(126999) == 126.0f + 999.0f / 1000.0f);
}

void testEvaluate() {
  std::printf("evaluate\n");
  for (int p = 0; p < kParamsPerTrack; ++p) gUnder[p] = uint8_t(p + 1);
  uint8_t out[kParamsPerTrack];

  // set: value from the curve, rounding at the end
  MacroSet s = must("voice bass\nmacro OPEN\n cutoff 0:20 1:120\n");
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[0] == 20);
  eval(s, 0.333f, 0.0f, out);
  CHECK(out[0] == 53);   // 20 + 33.3 = 53.3
  eval(s, 0.335f, 0.0f, out);
  CHECK(out[0] == 54);   // 53.5 rounds up
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[0] == 120);
  // not owned: copied from underneath
  CHECK(out[1] == 2 && out[24] == 25 && out[31] == 32);
  CHECK(s.ownedMask() == 1u);

  // base patch replaces "underneath" as the starting value; add / mul on top
  s = must("voice bass\nbase reso=40 tune=24 lvl=90\n"
           "macro A\n reso add 0:0 1:30\n"
           "macro B\n lvl mul 0:1 1:0.5\n");
  CHECK(s.ownedMask() == ((1u << 1) | (1u << 4) | (1u << 24)));
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[1] == 40 && out[4] == 24 && out[24] == 90);
  eval(s, 0.5f, 1.0f, out);
  CHECK(out[1] == 55 && out[24] == 45);
  // without a base value, add starts from the value underneath
  s = must("voice bass\nmacro A\n reso add 0:-10 1:10\n");
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[1] == 2);
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[1] == 0);   // 2 - 10 clamps to 0

  // combine in file order: set, then add, then mul, then a later set replaces
  s = must("voice bass\n"
           "macro A\n cut 0:40 1:80\n"
           "macro B\n cut add 0:0 1:20\n cut mul 0:1 1:2\n"
           "macro C\n");
  eval(s, 1.0f, 0.5f, out);
  CHECK(out[0] == 127);   // (80 + 10) * 1.5 = 135, clamped to the range
  eval(s, 0.0f, 0.25f, out);
  CHECK(out[0] == 56);   // (40 + 5) * 1.25 = 56.25
  s = must("voice bass\nmacro A\n cut 0:10 1:20\nmacro B\n cut 0:100 1:110\n");
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[0] == 100);   // the later set wins

  // "by": a target's effect scaled by another macro's position
  s = must("voice bass\nbase cut=50\n"
           "macro OPEN\n"
           "macro BITE\n cut add 0:0 1:40 by OPEN\n reso 0:0 1:100 by OPEN\n env mul 0:1 1:0 by OPEN\n");
  gUnder[1] = 20;
  gUnder[5] = 100;
  eval(s, 0.0f, 1.0f, out);   // OPEN 0: BITE does nothing
  CHECK(out[0] == 50 && out[1] == 20 && out[5] == 100);
  eval(s, 1.0f, 1.0f, out);   // OPEN 1: full effect
  CHECK(out[0] == 90 && out[1] == 100 && out[5] == 0);
  eval(s, 0.5f, 1.0f, out);   // half: add 20, set halfway (20 -> 60), mul by 0.5
  CHECK(out[0] == 70 && out[1] == 60 && out[5] == 50);
  eval(s, 0.5f, 0.5f, out);   // BITE 0.5: curves give 20 / 50 / 0.5, then halved
  CHECK(out[0] == 60 && out[1] == 35 && out[5] == 75);

  // clamping to each parameter's own range, enums stepped
  s = must("voice bass\nmacro A\n tune 0:0 1:48\n tune add 0:0 1:40\n wave 0:saw 0.5:sqr 1:puls\n"
           " rate 0:0 1:3 linear\n");
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[4] == 48 && out[2] == 2 && out[3] == 3);
  eval(s, 0.49f, 0.0f, out);
  CHECK(out[2] == 0);
  CHECK(out[3] == 1);   // linear on an enum: 1.47 rounds to 1
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[2] == 1 && out[3] == 2);   // 1.5 rounds up
  // NaN-proof and out-of-range positions
  const float bad[4] = {std::nanf(""), 5.0f, -1.0f, 0.0f};
  s = must("voice bass\nmacro A\n cut 0:10 1:20\nmacro B\n res 0:10 1:20\nmacro C\n env 0:10 1:20\n");
  evaluate(s, gT, bad, gUnder, out);
  CHECK(out[1] == 20 && out[5] == 10);
  CHECK(out[0] == 10 || out[0] == 20);
}

void testParseFeatures() {
  std::printf("parse: features\n");
  const char* text =
      "# a comment line\n"
      "Voice LEAD   # comment after a statement\n"
      "name glassy\n"
      "base wave=sqr ENV=10\n"
      "base rate=1/4.\n"
      "macro OPEN 0.25\n"
      "\tCutoff 0:20 0.5:70 1:120\n"
      "    resonance 0:60 0.6:110 1:80 smooth\n"
      "macro bite\n"
      "  env mod add 0:0 1:15 by open\n"   // long name with a space? no: "env" + "mod" are two words
      ;
  MacroSet s;
  ParseError e;
  // "env mod" is two words; the long name is written without the space.
  CHECK(!parseStr(text, &s, &e) && e.line == 10);
  const char* ok =
      "# a comment line\n"
      "Voice LEAD   # comment after a statement\n"
      "name glassy\n"
      "base wave=sqr ENV=10\n"
      "base rate=1/4.\n"
      "macro OPEN 0.25\n"
      "\tCutoff 0:20 0.5:70 1:120\n"
      "    resonance 0:60 0.6:110 1:80 smooth\r\n"
      "macro bite\n"
      "  env_mod add by open 0:0 1:15\n"
      "  wave 0:saw 0.5:2 1:puls\n"
      "  lvl mul 0:1 1:0.5 linear\n";
  s = must(ok);
  CHECK(s.track == kTrackLead);
  CHECK(std::strcmp(s.name, "GLASSY") == 0);
  CHECK(s.baseMask == ((1u << 2) | (1u << 5) | (1u << 3)));
  CHECK(s.base[2] == 1 && s.base[5] == 10 && s.base[3] == 1);
  CHECK(s.numMacros == 2);
  CHECK(std::strcmp(s.macros[0].name, "OPEN") == 0 && near(s.macros[0].start(), 0.25f));
  CHECK(std::strcmp(s.macros[1].name, "BITE") == 0 && s.macros[1].start() == 0.0f);
  CHECK(s.macros[0].n == 2 && s.macros[1].n == 3);
  const Target& t0 = s.target(0, 0);
  CHECK(t0.param == 0 && t0.op() == Op::Set && t0.by() == kNoMacro && t0.n == 3 && t0.interp() == Interp::Linear);
  CHECK(near(pos(s, t0, 1), 0.5f) && near(val(s, t0, 2), 120.0f));
  CHECK(s.target(0, 1).interp() == Interp::Smooth);
  const Target& b0 = s.target(1, 0);
  CHECK(b0.param == 5 && b0.op() == Op::Add && b0.by() == 0 && near(val(s, b0, 1), 15.0f));
  const Target& b1 = s.target(1, 1);
  CHECK(b1.param == 2 && b1.interp() == Interp::Step);   // enums default to step
  CHECK(near(val(s, b1, 0), 0.0f) && near(val(s, b1, 1), 2.0f) && near(val(s, b1, 2), 2.0f));
  const Target& b2 = s.target(1, 2);
  CHECK(b2.param == 24 && b2.op() == Op::Mul && near(val(s, b2, 1), 0.5f));
  CHECK(s.numTargets == 5 && s.numPoints == 3 + 3 + 2 + 3 + 2);
  // forward reference in "by", macro names with digits / - / _
  s = must("voice kick\nmacro A-1\n cut 0:0 1:1 by z_2\nmacro z_2\n");
  CHECK(s.target(0, 0).by() == 1);
  // a set with only a base patch, and with an empty macro
  s = must("voice snare\nbase cut=3\n");
  CHECK(s.numMacros == 0 && s.ownedMask() == 1u);
  s = must("voice snare\nmacro EMPTY\n");
  CHECK(s.numMacros == 1 && s.ownedMask() == 0u);
  // track aliases and names
  CHECK(trackFromName("CHH") == kTrackHatClosed && trackFromName("openhat") == kTrackHatOpen);
  CHECK(trackFromName("Snar") == kTrackSnare && trackFromName("hat") == -1);
  CHECK(std::strcmp(trackName(kTrackBass), "bass") == 0);
  CHECK(findParam(gT, kTrackBass, "osc-wave") == 2 && findParam(gT, kTrackBass, "LFO_RATE") == 3);
  CHECK(findParam(gT, kTrackBass, "cutx") == -1 && findParam(gT, 9, "cut") == -1);
  CHECK(findParam(gT, kTrackBass, "reso") == 1 && findParam(gT, kTrackBass, "cutof") == 0);
  CHECK(findParam(gT, kTrackBass, "osc") == kAmbiguous && findParam(gT, kTrackBass, "re") == -1);
  // numbers: signs, leading point, three decimals
  s = must("voice bass\nmacro A\n cut add 0:-1.5 .5:+2 1:0.125\n");
  CHECK(near(val(s, s.target(0, 0), 0), -1.5f) && near(pos(s, s.target(0, 0), 1), 0.5f));
  CHECK(near(val(s, s.target(0, 0), 2), 0.125f));
  // no trailing newline, CRLF, empty lines
  s = must("\r\n\nvoice bass\r\n\r\nmacro A\r\n cut 0:1");
  CHECK(s.macros[0].n == 1);
}

void testParseErrors() {
  std::printf("parse: every error path, with its line\n");
  expectError("", 0, "empty file");
  expectError("# only a comment\n\n", 0, "empty file");
  expectError("name X\nvoice bass\n", 1, "first line must be 'voice");
  expectError("voice\n", 1, "one track name");
  expectError("voice bass lead\n", 1, "one track name");
  expectError("voice tuba\n", 1, "unknown track 'tuba'");
  expectError("voice bass\nvoice bass\n", 2, "'voice' given twice");
  expectError("voice bass\nname A\nname B\n", 3, "'name' given twice");
  expectError("voice bass\nname\n", 2, "one word");
  expectError("voice bass\nname TWO WORDS\n", 2, "one word");
  expectError("voice bass\nname THIRTEENCHARS\n", 2, "longer than 12");
  expectError("voice bass\nbase\n", 2, "param=value");
  expectError("voice bass\nbase cut 20\n", 2, "param=value");
  expectError("voice bass\nbase cut=\n", 2, "param=value");
  expectError("voice bass\nbase =20\n", 2, "param=value");
  expectError("voice bass\nbase cutx=20\n", 2, "unknown parameter 'cutx' for bass");
  expectError("voice bass\nbase cut=20 cutoff=30\n", 2, "given twice");
  expectError("voice bass\nbase cut=128\n", 2, "out of range (0..127)");
  expectError("voice bass\nbase tune=-1\n", 2, "out of range (0..48)");
  expectError("voice bass\nbase cut=abc\n", 2, "not a number");
  expectError("voice bass\nbase wave=tri\n", 2, "not a value of WAVE");
  expectError("voice bass\nbase wave=3\n", 2, "out of range (0..2)");
  expectError("voice bass\nmacro\n", 2, "macro NAME");
  expectError("voice bass\nmacro A 0.5 x\n", 2, "macro NAME");
  expectError("voice bass\nmacro A 1.5\n", 2, "start '1.5' must be a number 0..1");
  expectError("voice bass\nmacro A x\n", 2, "must be a number 0..1");
  expectError("voice bass\nmacro TOOLONG\n", 2, "longer than 6");
  expectError("voice bass\nmacro A:B\n", 2, "letters, digits");
  expectError("voice bass\nmacro A\nmacro a\n", 3, "declared twice");
  expectError("voice bass\nmacro A\nmacro B\nmacro C\nmacro D\nmacro E\n", 6, "more than 4 macros");
  expectError("voice bass\ncut 0:1\n", 2, "needs a 'macro NAME' line above");
  expectError("voice bass\nmacro A\n cutx 0:1\n", 3, "unknown parameter 'cutx'");
  expectError("voice bass\nmacro A\n osc 0:1\n", 3, "'osc' could be more than one bass parameter");
  expectError("voice bass\nmacro A\n cut\n", 3, "no points");
  expectError("voice bass\nmacro A\n cut add\n", 3, "no points");
  expectError("voice bass\nmacro A\n cut 0:1 bogus\n", 3, "'bogus': expected a point");
  expectError("voice bass\nmacro A\n cut set add 0:1\n", 3, "only one of set / add / mul");
  expectError("voice bass\nmacro A\n cut 0:1 step smooth\n", 3, "only one of linear / smooth / step");
  expectError("voice bass\nmacro A\n cut 0:1 by\n", 3, "needs a macro name");
  expectError("voice bass\nmacro A\n cut 0:1 by 1:2\n", 3, "needs a macro name");
  expectError("voice bass\nmacro A\n cut 0:1 by A by A\n", 3, "'by' given twice");
  expectError("voice bass\nmacro A\n cut 0:1 by NOPE\n\n", 3, "'by NOPE': no macro");
  expectError("voice bass\nmacro A\n cut 0:1 by MUCHTOOLONG\n", 3, "no macro has that name");
  expectError("voice bass\nmacro A\n cut 1.5:1\n", 3, "position '1.5' must be a number 0..1");
  expectError("voice bass\nmacro A\n cut -0.1:1\n", 3, "must be a number 0..1");
  expectError("voice bass\nmacro A\n cut x:1\n", 3, "must be a number 0..1");
  expectError("voice bass\nmacro A\n cut 0.5:\n", 3, "has no value");
  expectError("voice bass\nmacro A\n cut 0:200\n", 3, "out of range (0..127)");
  expectError("voice bass\nmacro A\n cut 0:1:2\n", 3, "not a number");
  expectError("voice bass\nmacro A\n wave 0:tri\n", 3, "not a value of WAVE");
  expectError("voice bass\nmacro A\n cut add 0:saw\n", 3, "plain numbers");
  expectError("voice bass\nmacro A\n cut add 0:300\n", 3, "outside -255..255");
  expectError("voice bass\nmacro A\n cut mul 0:-1\n", 3, "outside 0..16");
  expectError("voice bass\nmacro A\n cut mul 0:17\n", 3, "outside 0..16");
  expectError("voice bass\nmacro A\n cut 0.8:1 0.6:2\n", 3, "go backwards (0.6 after 0.8)");
  expectError("voice bass\nmacro A\n cut 0:1.2345\n", 3, "not a number");
  expectError("voice bass\nmacro A\n cut 0.1234:1\n", 3, "must be a number 0..1");
  {
    std::string t = "voice bass\n# " + std::string(210, 'x') + "\n";
    expectError(t.c_str(), 2, "longer than 200");
    std::string w = "voice bass\nmacro A\n cut";
    for (int i = 0; i < 40; ++i) w += " 0:1";
    expectError((w + "\n").c_str(), 3, "too many words");
  }
  expectError("voice bass\nmacro A\n cut 0:1 \x01\n", 3, "plain ASCII");
  expectError("voice bass\nmacro \xC3\xA9\n", 2, "plain ASCII");
  // an error never leaves a half-loaded set behind in the caller's last good copy: the
  // lab parses into a scratch set; here, just check a failed parse reports no success twice
  MacroSet a, b;
  ParseError e;
  CHECK(!parseStr("voice bass\nmacro A\n cut 0:1\n cut 0:999\n", &a, &e) && e.line == 4);
  CHECK(parseStr("voice bass\nmacro A\n cut 0:1\n", &b, &e) && e.line == 0 && e.msg[0] == 0);
}

// parse() clears the whole table first, so two parses of the same set are byte-identical.
bool sameSet(const MacroSet& a, const MacroSet& b) {
  const unsigned char *x = reinterpret_cast<const unsigned char*>(&a), *y = reinterpret_cast<const unsigned char*>(&b);
  for (size_t i = 0; i < sizeof(MacroSet); ++i)
    if (x[i] != y[i]) {
      std::printf("  sets differ at byte %zu (%d vs %d)\n", i, x[i], y[i]);
      return false;
    }
  return true;
}

void roundTrip(const char* text) {
  MacroSet a = must(text), b;
  char buf[8192], buf2[8192];
  const size_t n = format(a, gT, buf, sizeof(buf));
  CHECK(n > 0);
  ParseError e;
  CHECK(parse(buf, n, gT, &b, &e));
  CHECK(sameSet(a, b));
  CHECK(format(b, gT, buf2, sizeof(buf2)) == n && std::strcmp(buf, buf2) == 0);
  CHECK(format(a, gT, buf2, 10) == 0);   // too small: 0, never overruns
}

void testRoundTrip() {
  std::printf("format -> parse round trips\n");
  roundTrip("voice bass\nname X\nbase wave=puls cut=3 rate=1/4.\nmacro OPEN 0.333\n cut 0:20 0.5:70 1:120 smooth\n"
            " res add 0:-3.5 1:15 by BITE\n wave 0:saw 1:sqr linear\nmacro BITE\n lvl mul 0:1 0.75:0.125 1:2 step\n"
            " rate 0:1/4 1:1/16\n");
  roundTrip("voice ohh\nmacro A\n");
  roundTrip("voice lead\nbase tune=0\n");
  MacroSet s = must("voice clap\nmacro A\n cut 0:0.001 0.999:126.999\n");
  CHECK(near(val(s, s.target(0, 0), 1), 126.999f, 1e-4f));
  roundTrip("voice clap\nmacro A\n cut 0:0.001 0.999:126.999\n");
}


// ---- model v2 ------------------------------------------------------------------------------
std::string rep(const char* line, int n) {
  std::string t;
  for (int i = 0; i < n; ++i) t += line;
  return t;
}
std::string curvePts(int n) {   // n points, positions 0 .. 1, values 0, 1, 2, ...
  std::string t;
  char b[32];
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), " %d.%03d:%d", i == n - 1 ? 1 : 0, i == n - 1 ? 0 : i * 1000 / (n - 1), i);
    t += b;
  }
  return t;
}

void testLimits() {
  std::printf("limits and pools\n");
  CHECK(sizeof(MacroSet) <= 2048);
  CHECK(sizeof(Target) == 4 && sizeof(Point) == 4 && sizeof(PatchValue) == 3);
  // 16 points on a curve; 17 is an error
  MacroSet s = must(("voice bass\nmacro A\n cut" + curvePts(16) + "\n").c_str());
  CHECK(s.target(0, 0).n == 16);
  float a0[4] = {1.0f, 0, 0, 0};
  uint8_t out[kParamsPerTrack];
  evaluate(s, gT, a0, gUnder, out);
  CHECK(out[0] == 15);
  expectError(("voice bass\nmacro A\n cut 0:1\n cut" + curvePts(17) + "\n").c_str(), 4, "more than 16 points on one curve");
  // 32 targets in one macro; the 33rd is an error
  s = must(("voice bass\nmacro A\n" + rep(" cut add 0:0 1:1\n", 32)).c_str());
  CHECK(s.macros[0].n == 32 && s.numTargets == 32);
  evaluate(s, gT, a0, gUnder, out);
  CHECK(out[0] == gUnder[0] + 32);
  expectError(("voice bass\nmacro A\n" + rep(" cut add 0:0 1:1\n", 33)).c_str(), 35, "macro A has more than 32 targets");
  // 64 targets in the set, shared by its macros; the 65th is an error
  const std::string t64 = "voice bass\nmacro A\n" + rep(" res add 0:0 1:1\n", 24) + "macro B\n" +
                          rep(" res add 0:0 1:1\n", 24) + "macro C\n" + rep(" res add 0:0 1:1\n", 16);
  s = must(t64.c_str());
  CHECK(s.numTargets == 64 && s.macros[1].first == 24 && s.macros[2].first == 48 && s.macros[2].n == 16);
  const float all[4] = {1.0f, 1.0f, 1.0f, 0.0f};
  evaluate(s, gT, all, gUnder, out);
  CHECK(out[1] == gUnder[1] + 64);
  expectError((t64 + "macro D\n res 0:1\n").c_str(), 70, "more than 64 targets in the set");
  // 192 points in the set; the 193rd is an error
  const std::string p192 = "voice bass\nmacro A\n" + rep((" g7" + curvePts(16) + "\n").c_str(), 12);
  s = must(p192.c_str());
  CHECK(s.numPoints == 192);
  expectError((p192 + " cut 0:1\n").c_str(), 15, "more than 192 curve points in the set");
  // patches: 4, each up to every parameter; 96 values in the set
  std::string pv;
  for (int p = 6; p < 24; ++p) pv += " g" + std::to_string(p) + "=" + std::to_string(p);
  pv += " cut=1 res=2 tune=3 env=4";   // 22 values
  const std::string four = "voice bass\npatch P1" + pv + "\npatch P2" + pv + "\npatch P3" + pv + "\npatch P4" + pv + "\n";
  s = must(four.c_str());
  CHECK(s.numPatches == 4 && s.numPatchValues == 88);
  expectError((four + "patch P5 cut=1\n").c_str(), 6, "more than 4 patches");
  expectError((four + "patch P1 lvl=1 wave=sqr rate=1/4\npatch P2 lvl=1 wave=sqr rate=1/4\npatch P3 lvl=1 wave=sqr rate=1/4\n").c_str(),
              8, "more than 96 patch values in the set");
  // 8 morph lines, 8 trims
  const std::string m8 = four + "macro A\n morph P1\n morph P2\nmacro B\n morph P3\n morph P4\nmacro C\n morph P1\n morph P2\n"
                                "macro D\n morph P3\n morph P4\n";
  s = must(m8.c_str());
  CHECK(s.numMorphs == 8);
  expectError((m8 + " morph P1\n").c_str(), 18, "more than 8 morph lines in the set");
  const std::string tr8 = "voice bass\nmacro A\n" + rep(" trim 0:0 1:-1\n", 8);
  s = must(tr8.c_str());
  CHECK(s.numTrims == 8);
  expectError((tr8 + " trim 0:0\n").c_str(), 11, "more than 8 trim lines in the set");
  // name and string lengths
  must("voice bass\npatch EIGHTCHR cut=1\nmacro A\n morph eightchr\n");
  expectError("voice bass\npatch NINECHARS cut=1\n", 2, "longer than 8 characters");
  s = must(("voice bass\nmacro A \"12345678\" \"ABCDEFGH\" \"" + std::string(40, 'd') + "\"\n").c_str());
  CHECK(std::strlen(s.macros[0].desc) == 40 && !std::strcmp(s.macros[0].hi, "ABCDEFGH"));
  expectError("voice bass\nmacro A \"123456789\" \"B\"\n", 2, "an end label is longer than 8");
  expectError(("voice bass\nmacro A \"" + std::string(41, 'd') + "\"\n").c_str(), 2, "a description is longer than 40");
}

void testMorph() {
  std::printf("morph\n");
  uint8_t out[kParamsPerTrack];
  // one morph: continuous values interpolate, an enum switches at 0.5 by default
  MacroSet s = must("voice bass\nbase cut=20 res=10 wave=saw\npatch B cut=100 wave=puls\nmacro X\n morph B\n");
  CHECK(s.ownedMask() == 0x7u);
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[0] == 20 && out[1] == 10 && out[2] == 0);
  eval(s, 0.25f, 0.0f, out);
  CHECK(out[0] == 40 && out[2] == 0);
  eval(s, 0.49f, 0.0f, out);
  CHECK(out[2] == 0);
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[0] == 60 && out[2] == 2 && out[1] == 10);
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[0] == 100 && out[2] == 2);
  // without a base value the morph starts from the value underneath
  s = must("voice bass\npatch B cut=100\nmacro X\n morph B\n");
  gUnder[0] = 40;
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[0] == 70);
  // an amount curve, a switch point per line, per value
  s = must("voice bass\nbase cut=20 wave=saw\npatch B cut=100 wave=puls\nmacro X\n morph B 0:0 0.5:0 1:1\n");
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[0] == 20 && out[2] == 0);
  eval(s, 0.75f, 0.0f, out);
  CHECK(out[0] == 60 && out[2] == 2);
  s = must("voice bass\nbase wave=saw\npatch B wave=puls\nmacro X\n morph B switch 0.2\n");
  eval(s, 0.19f, 0.0f, out);
  CHECK(out[2] == 0);
  eval(s, 0.2f, 0.0f, out);
  CHECK(out[2] == 2);
  s = must("voice bass\nbase wave=saw rate=1/4\npatch B wave=puls@0.8 rate=1/16\nmacro X\n morph B switch 0.2\n");
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[2] == 0 && out[3] == 3);   // the value's own @0.8 beats the line's switch 0.2
  eval(s, 0.8f, 0.0f, out);
  CHECK(out[2] == 2);
  // two morphs on the XY pad: each adds its share of the difference from the base; past a
  // total of 1 the shares are scaled to add up to 1 (a mix of the patches, never beyond them)
  s = must("voice bass\nbase cut=20 res=10 tune=0 wave=saw\n"
           "patch B cut=100 res=50 wave=sqr\npatch C cut=60 tune=40 wave=puls\n"
           "macro X\n morph B\nmacro Y\n morph C\n");
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[0] == 100 && out[1] == 50 && out[4] == 0 && out[2] == 1);
  eval(s, 0.0f, 1.0f, out);
  CHECK(out[0] == 60 && out[1] == 10 && out[4] == 40 && out[2] == 2);
  eval(s, 1.0f, 1.0f, out);
  CHECK(out[0] == 80 && out[1] == 30 && out[4] == 20);
  eval(s, 0.5f, 0.5f, out);
  CHECK(out[0] == 80 && out[1] == 30 && out[4] == 20);   // the base's weight is used up
  eval(s, 0.25f, 0.25f, out);
  CHECK(out[0] == 50 && out[1] == 20 && out[4] == 10);
  eval(s, 0.25f, 0.5f, out);
  CHECK(out[0] == 60);   // 20 + 0.25 * 80 + 0.5 * 40
  // enums with two morphs: the line with the larger amount past its switch point; a tie goes to the later line
  eval(s, 0.6f, 0.4f, out);
  CHECK(out[2] == 1);
  eval(s, 0.6f, 0.7f, out);
  CHECK(out[2] == 2);
  eval(s, 0.7f, 0.7f, out);
  CHECK(out[2] == 2);
  eval(s, 0.3f, 0.45f, out);
  CHECK(out[2] == 0);
  // target lines adjust after the morphs, wherever they are in the file
  s = must("voice bass\nbase cut=20\npatch B cut=100\nmacro X\n cut add 0:0 1:10\n morph B\n");
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[0] == 110);
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[0] == 65);
  // one patch used by two macros at once
  s = must("voice bass\nbase cut=20\npatch B cut=100\nmacro X\n morph B\nmacro Y\n morph B 0:0 1:0.5\n");
  eval(s, 0.5f, 1.0f, out);
  CHECK(out[0] == 100);
  // a patch no morph uses is not owned
  s = must("voice bass\npatch B cut=100\npatch C res=1\nmacro X\n morph B\n");
  CHECK(s.ownedMask() == 1u);
  // a patch can be continued on more lines, also after another patch
  s = must("voice bass\npatch B cut=100\npatch C res=1\npatch B wave=puls\nmacro X\n morph B\n morph C\n");
  CHECK(s.patches[0].n == 2 && s.patches[1].first == 2 && s.patchValues[1].param == 2 && s.patchValues[2].param == 1);
  CHECK(s.ownedMask() == 0x7u);
  // errors
  expectError("voice bass\npatch B\n", 2, "patch NAME param=value");
  expectError("voice bass\npatch B cut\n", 2, "param=value");
  expectError("voice bass\npatch B:1 cut=1\n", 2, "letters, digits");
  expectError("voice bass\npatch B cut=1 cutoff=2\n", 2, "patch B: CUT given twice");
  expectError("voice bass\npatch B cut=200\n", 2, "out of range");
  expectError("voice bass\npatch B cut=1@0.5\n", 2, "only for enum parameters");
  expectError("voice bass\npatch B wave=sqr@0\n", 2, "switch point '0'");
  expectError("voice bass\npatch B wave=sqr@0.125\n", 2, "at most 2 decimals");
  expectError("voice bass\nmorph B\n", 2, "needs a 'macro NAME' line above");
  expectError("voice bass\nmacro X\n morph\n", 3, "write 'morph PATCH'");
  expectError("voice bass\nmacro X\n morph NOPE\n", 3, "'morph NOPE': no patch has that name");
  expectError("voice bass\npatch B cut=1\nmacro X\n morph B 0:2\n", 4, "morph amount '2' must be a number 0..1");
  expectError("voice bass\npatch B cut=1\nmacro X\n morph B by X\n", 4, "'by' is not for morph lines");
  expectError("voice bass\npatch B cut=1\nmacro X\n morph B add\n", 4, "expected a point pos:amount");
  expectError("voice bass\npatch B cut=1\nmacro X\n morph B switch\n", 4, "needs a point");
  expectError("voice bass\npatch B cut=1\nmacro X\n morph B switch 2\n", 4, "switch point '2'");
  expectError("voice bass\npatch B cut=1\nmacro X\n morph B\n morph b\n", 5, "given twice in macro X");
}

void testLabels() {
  std::printf("labels, descriptions, the about line\n");
  MacroSet s = must("voice bass\nmacro TONE 0.5 \"DARK\" \"BRIGHT\" \"how bright: # is not a comment\"\n");
  CHECK(!std::strcmp(s.macros[0].lo, "DARK") && !std::strcmp(s.macros[0].hi, "BRIGHT"));
  CHECK(!std::strcmp(s.macros[0].desc, "how bright: # is not a comment") && near(s.macros[0].start(), 0.5f));
  s = must("voice bass\nmacro TONE \"dark end\" \"lit\"   # a comment\nmacro B \"only a description\"\n");
  CHECK(!std::strcmp(s.macros[0].lo, "dark end") && !std::strcmp(s.macros[0].hi, "lit") && !s.macros[0].desc[0]);
  CHECK(!s.macros[1].lo[0] && !std::strcmp(s.macros[1].desc, "only a description"));
  expectError("voice bass\nmacro A \"a\" \"b\" \"c\" \"d\"\n", 2, "at most three strings");
  expectError("voice bass\nmacro A \"a\" 0.5\n", 2, "macro NAME");
  expectError("voice bass\nmacro A \"\" \"b\"\n", 2, "an empty string");
  expectError("voice bass\nmacro A \"open\n", 2, "without its closing quote");
  expectError("voice bass\nmacro A \"a\"b\n", 2, "a space after a closing quote");
  expectError("voice bass\nmacro A x\"y\" z\n", 2, "inside a word");
  expectError("voice bass\nname \"X\"\n", 2, "only a 'macro' line takes strings");
  expectError("voice bass\nmacro \"A\"\n", 2, "macro NAME");
  // the about line: the file's first comment line, without "NAME -- " when it repeats the name
  s = must("# ACID -- family: INTERACTION.\nvoice bass\nname acid\n");
  CHECK(!std::strcmp(s.about, "family: INTERACTION."));
  s = must("\n  \n#ACID -- family: X.  \nvoice bass\n");
  CHECK(!std::strcmp(s.about, "ACID -- family: X."));   // no name: kept whole
  s = must("# OTHER -- family: X.\nvoice bass\nname ACID\n");
  CHECK(!std::strcmp(s.about, "OTHER -- family: X."));
  s = must("voice bass   # not the first line\n# nor this\n");
  CHECK(s.about[0] == 0);
  s = must(("# " + std::string(100, 'a') + "\nvoice bass\n").c_str());
  CHECK(std::strlen(s.about) == size_t(kAboutLen));
  s = must("# caf\xC3\xA9\nvoice bass\n");   // comments may hold anything; the about line keeps ASCII
  CHECK(!std::strcmp(s.about, "caf??"));
}

int countWarn(const char* text, Warnings* w) {
  MacroSet s;
  ParseError e;
  w->count = -1;
  if (!parse(text, std::strlen(text), gT, &s, &e, w)) {
    ++gFail;
    std::printf("  FAIL: did not parse (line %d: %s)\n", e.line, e.msg);
    return -1;
  }
  return w->count;
}

void testWarnings() {
  std::printf("warnings\n");
  Warnings w;
  // a later plain set throws away an earlier macro's work on the parameter
  CHECK(countWarn("voice bass\nmacro A\n cut 0:10 1:20\nmacro B\n cut 0:30 1:40\n", &w) == 1);
  CHECK(w.w[0].line == 5 && std::strstr(w.w[0].msg, "CUT: plain set undoes line 3 (A)"));
  CHECK(countWarn("voice bass\nmacro A\n cut add 0:0 1:5\n cut 0:30 1:40\n", &w) == 1 && w.w[0].line == 4);
  // marked, scaled, add / mul, or the first line on the parameter: no warning
  CHECK(countWarn("voice bass\nmacro A\n cut 0:10 1:20\nmacro B\n cut 0:30 1:40 override\n", &w) == 0);
  CHECK(countWarn("voice bass\nmacro A\n cut 0:10 1:20\nmacro B\n cut 0:30 1:40 by A\n", &w) == 0);
  CHECK(countWarn("voice bass\nbase cut=3\nmacro A\n cut 0:10 1:20\nmacro B\n cut add 0:1 1:2\n cut mul 0:1 1:2\n", &w) == 0);
  // across a morph of another macro (but not the macro's own morph)
  CHECK(countWarn("voice bass\npatch P cut=10\nmacro A\n morph P\nmacro B\n cut 0:30 1:40\n", &w) == 1);
  CHECK(w.w[0].line == 6 && std::strstr(w.w[0].msg, "undoes 'morph P' (A)"));
  CHECK(countWarn("voice bass\npatch P cut=10\nmacro A\n morph P\n cut 0:30 1:40\n", &w) == 0);
  // a patch nothing morphs toward
  CHECK(countWarn("voice bass\npatch P cut=10\npatch Q res=1\nmacro A\n morph Q\n", &w) == 1);
  CHECK(w.w[0].line == 2 && std::strstr(w.w[0].msg, "patch P is not used"));
  // more than kMaxWarnings: all counted, the first kept
  std::string many = "voice bass\nmacro A\n cut 0:1\n";
  for (int i = 0; i < kMaxWarnings + 3; ++i) many += " cut 0:2\n";
  CHECK(countWarn(many.c_str(), &w) == kMaxWarnings + 3 && w.w[kMaxWarnings - 1].line == kMaxWarnings + 3);
  // a warning never stops the load, and a set parses the same with or without the list
  MacroSet a, b;
  ParseError e;
  const char* t = "voice bass\nmacro A\n cut 0:10 1:20\nmacro B\n cut 0:30 1:40\n";
  CHECK(parseStr(t, &a, &e) && parse(t, std::strlen(t), gT, &b, &e, &w) && sameSet(a, b));
}

void testTrim() {
  std::printf("trim\n");
  uint8_t out[kParamsPerTrack];
  // dB on the level through the cubic taper: lvl * 10^(dB / 60)
  MacroSet s = must("voice bass\nbase lvl=100\nmacro A\n trim 0:0 1:-6\n");
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[24] == 100);
  eval(s, 1.0f, 0.0f, out);
  CHECK(out[24] == 79);   // 79.43
  eval(s, 0.5f, 0.0f, out);
  CHECK(out[24] == 89);   // -3 dB: 89.13
  s = must("voice bass\nbase lvl=100\nmacro A\n trim 0:6\n");
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[24] == 126);   // 125.89
  // scaled by another macro; several trims add up in dB
  s = must("voice bass\nbase lvl=100\nmacro A\n trim 0:0 1:-6 by B\nmacro B\n trim 0:0 1:-3\n");
  eval(s, 1.0f, 0.5f, out);
  CHECK(out[24] == 84);   // -3 - 1.5 = -4.5 dB: 84.14
  // applied after every target, whatever the file order; the level is owned
  s = must("voice bass\nmacro A\n trim 0:-6\nmacro B\n lvl 0:100 1:100\n");
  CHECK(s.ownedMask() == (1u << 24));
  eval(s, 0.0f, 1.0f, out);
  CHECK(out[24] == 79);
  s = must("voice bass\nmacro A\n trim 0:-6\n");
  CHECK(s.ownedMask() == (1u << 24));
  gUnder[24] = 25;
  eval(s, 0.0f, 0.0f, out);
  CHECK(out[24] == 20);   // 19.86
  // the factor matches 10^(dB / 60) closely across the range
  int bad = 0;
  for (int db = -240; db <= 240; db += 7)
    for (int lvl : {10, 64, 100, 127}) {
      char text[96];
      const int a = db < 0 ? -db : db;
      std::snprintf(text, sizeof(text), "voice bass\nbase lvl=%d\nmacro A\n trim 0:%s%d.%d\n", lvl, db < 0 ? "-" : "",
                    a / 10, a % 10);
      MacroSet t = must(text);
      eval(t, 0.0f, 0.0f, out);
      const double want = double(lvl) * std::pow(10.0, double(db) / 600.0);
      const double clamped = want > 127.0 ? 127.0 : want;
      const double frac = clamped - std::floor(clamped);
      if (std::fabs(frac - 0.5) > 1e-3 && out[24] != int(clamped + 0.5)) ++bad;
    }
  CHECK(bad == 0);
  expectError("voice bass\nmacro A\n trim\n", 3, "a trim line needs points");
  expectError("voice bass\nmacro A\n trim 0:25\n", 3, "outside -24..24");
  expectError("voice bass\nmacro A\n trim 0:sqr\n", 3, "plain numbers in dB");
  expectError("voice bass\nmacro A\n trim 0:1 add\n", 3, "expected a point pos:dB");
  expectError("voice bass\nmacro A\n trim 0:1 by NOPE\n", 3, "'by NOPE': no macro");
  expectError("voice bass\ntrim 0:1\n", 2, "needs a 'macro NAME' line above");
}

void testJumps() {
  std::printf("enum jumps\n");
  MacroSet s = must("voice bass\nbase wave=saw\npatch B wave=puls@0.3\nmacro X\n morph B\n rate 0:1/4 0.75:1/16\n");
  float jp[8];
  uint8_t jparam[8];
  const float at[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const int n = enumJumps(s, gT, 0, at, gUnder, jp, jparam, 8);
  CHECK(n == 2 && near(jp[0], 0.3f) && jparam[0] == 2 && near(jp[1], 0.75f) && jparam[1] == 3);
  CHECK(enumJumps(s, gT, 1, at, gUnder, jp, jparam, 8) == 0);
}

void testRoundTripV2() {
  std::printf("format -> parse round trips, v2\n");
  roundTrip("# KEYS -- family: MORPH.\nvoice lead\nname KEYS\nbase cut=3\n"
            "patch BELL wave=puls@0.25 rate=1/16 cut=120\npatch PLUCK res=12\npatch BELL lvl=90\n"
            "macro OR-PLK 0.3 \"ORGAN\" \"PLUCK\" \"organ to pluck\"\n morph PLUCK\n morph BELL 0:0 0.5:0.2 1:1 smooth switch 0.4\n"
            " cut add 0:0 1:-10 override\n trim 0:0 1:-2.5 by BELL\nmacro BELL \"ring\"\n cut 0:1 1:2 by OR-PLK\n"
            " trim 0:1\n morph BELL\n");
  // long base and patch lines wrap and still read back
  std::string pv;
  for (int p = 6; p < 24; ++p) pv += " g" + std::to_string(p) + "=" + std::to_string(p + 100);
  roundTrip(("voice bass\nbase" + pv + "\nbase cut=1 res=2 tune=3 env=4 lvl=5 wave=sqr rate=1/4.\npatch LONGLONG" + pv +
             "\npatch LONGLONG cut=1 res=2 tune=3 env=4 lvl=5 wave=sqr@0.75 rate=1/4.@1\nmacro A\n morph longlong\n")
                .c_str());
  MacroSet s = must("# about\nvoice bass\n");
  char buf[256];
  CHECK(format(s, gT, buf, sizeof(buf)) > 0 && !std::strncmp(buf, "# about\nvoice bass\n", 19));
}

// Evaluation cost: a large set, timed.
void benchEvaluate() {
  std::string t = "voice bass\nbase cut=20 res=10 lvl=100\n";
  std::string pv;
  for (int p = 6; p < 24; ++p) pv += " g" + std::to_string(p) + "=" + std::to_string(p);
  t += "patch A" + pv + " cut=1 res=2 tune=3 env=4\npatch B" + pv + " cut=1 res=2 tune=3 env=4\n";
  for (int m = 0; m < 4; ++m) {
    t += "macro M" + std::to_string(m) + "\n";
    if (m < 2) t += std::string(" morph ") + (m ? "B" : "A") + "\n";
    for (int j = 0; j < 15; ++j) t += " g" + std::to_string(6 + j) + " add" + curvePts(3) + "\n";
    t += " trim" + curvePts(2) + "\n";
  }
  MacroSet s = must(t.c_str());
  uint8_t out[kParamsPerTrack];
  float p[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  const int kN = 100000;
  unsigned sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    p[i & 3] = float(i % 997) / 997.0f;
    evaluate(s, gT, p, gUnder, out);
    sink += out[6];
  }
  const double ns =
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / kN;
  std::printf("evaluate: %d targets, %d points, %d morph lines over %d patch values, %d trims: %.0f ns per call here (%u)\n",
              s.numTargets, s.numPoints, s.numMorphs, s.numPatchValues, s.numTrims, ns, sink & 1u);
}

}  // namespace

int main() {
  testCurves();
  testEvaluate();
  testParseFeatures();
  testParseErrors();
  testRoundTrip();
  testLimits();
  testMorph();
  testLabels();
  testWarnings();
  testTrim();
  testJumps();
  testRoundTripV2();
  benchEvaluate();
  std::printf("test_macro_eval: %d checks, %d failed (sizeof(MacroSet) = %zu bytes)\n", gChecks, gFail,
              sizeof(MacroSet));
  return gFail ? 1 : 0;
}
