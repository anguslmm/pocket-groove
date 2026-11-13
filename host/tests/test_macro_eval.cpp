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
#include <cmath>
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

ParamTable table() {
  ParamTable t;
  std::memset(&t, 0, sizeof(t));
  for (int tr = 0; tr < kNumTracks; ++tr) {
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

void eval(const MacroSet& s, float a, float b, uint8_t* out, float c = 0.0f, float d = 0.0f) {
  const float pos[4] = {a, b, c, d};
  evaluate(s, gT, pos, gUnder, out);
}

// ---------------------------------------------------------------------------------------------
void testCurves() {
  std::printf("curves\n");
  Curve c;
  std::memset(&c, 0, sizeof(c));
  c.n = 3;
  c.interp = Interp::Linear;
  c.pos[0] = 0.2f; c.val[0] = 10.0f;
  c.pos[1] = 0.6f; c.val[1] = 110.0f;   // non-monotonic: peaks at 60 %
  c.pos[2] = 1.0f; c.val[2] = 80.0f;
  CHECK(near(c.at(0.0f), 10.0f));     // flat before the first point
  CHECK(near(c.at(-3.0f), 10.0f));    // positions clamp
  CHECK(near(c.at(0.2f), 10.0f));
  CHECK(near(c.at(0.4f), 60.0f));
  CHECK(near(c.at(0.6f), 110.0f));
  CHECK(near(c.at(0.8f), 95.0f));
  CHECK(near(c.at(1.0f), 80.0f));
  CHECK(near(c.at(7.0f), 80.0f));
  c.interp = Interp::Smooth;
  CHECK(near(c.at(0.4f), 60.0f));     // smoothstep is symmetric: same midpoint
  CHECK(near(c.at(0.3f), 10.0f + 100.0f * 0.15625f));   // t = 0.25 -> 0.15625
  CHECK(near(c.at(0.2f), 10.0f) && near(c.at(0.6f), 110.0f));
  c.interp = Interp::Step;
  CHECK(near(c.at(0.1f), 10.0f));
  CHECK(near(c.at(0.59f), 10.0f));
  CHECK(near(c.at(0.6f), 110.0f));    // a point's own position takes its value
  CHECK(near(c.at(0.99f), 110.0f));
  CHECK(near(c.at(1.0f), 80.0f));
  // one point: constant
  c.n = 1;
  c.interp = Interp::Linear;
  CHECK(near(c.at(0.0f), 10.0f) && near(c.at(1.0f), 10.0f));
  // equal positions: a jump
  c.n = 4;
  c.pos[0] = 0.0f; c.val[0] = 0.0f;
  c.pos[1] = 0.5f; c.val[1] = 50.0f;
  c.pos[2] = 0.5f; c.val[2] = 100.0f;
  c.pos[3] = 1.0f; c.val[3] = 0.0f;
  CHECK(near(c.at(0.25f), 25.0f));
  CHECK(near(c.at(0.4999f), 49.99f, 1e-2f));
  CHECK(near(c.at(0.5f), 100.0f));
  CHECK(near(c.at(0.75f), 50.0f));
  // all points at one position
  c.n = 2;
  c.pos[0] = 0.5f; c.val[0] = 10.0f;
  c.pos[1] = 0.5f; c.val[1] = 20.0f;
  CHECK(near(c.at(0.2f), 10.0f) && near(c.at(0.5f), 20.0f) && near(c.at(0.9f), 20.0f));
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
  CHECK(std::strcmp(s.macros[0].name, "OPEN") == 0 && near(s.macros[0].start, 0.25f));
  CHECK(std::strcmp(s.macros[1].name, "BITE") == 0 && s.macros[1].start == 0.0f);
  CHECK(s.macros[0].numTargets == 2 && s.macros[1].numTargets == 3);
  const Target& t0 = s.macros[0].targets[0];
  CHECK(t0.param == 0 && t0.op == Op::Set && t0.by == kNoMacro && t0.curve.n == 3 && t0.curve.interp == Interp::Linear);
  CHECK(near(t0.curve.pos[1], 0.5f) && near(t0.curve.val[2], 120.0f));
  CHECK(s.macros[0].targets[1].curve.interp == Interp::Smooth);
  const Target& b0 = s.macros[1].targets[0];
  CHECK(b0.param == 5 && b0.op == Op::Add && b0.by == 0 && near(b0.curve.val[1], 15.0f));
  const Target& b1 = s.macros[1].targets[1];
  CHECK(b1.param == 2 && b1.curve.interp == Interp::Step);   // enums default to step
  CHECK(near(b1.curve.val[0], 0.0f) && near(b1.curve.val[1], 2.0f) && near(b1.curve.val[2], 2.0f));
  const Target& b2 = s.macros[1].targets[2];
  CHECK(b2.param == 24 && b2.op == Op::Mul && near(b2.curve.val[1], 0.5f));
  // forward reference in "by", macro names with digits / - / _
  s = must("voice kick\nmacro A-1\n cut 0:0 1:1 by z_2\nmacro z_2\n");
  CHECK(s.macros[0].targets[0].by == 1);
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
  CHECK(near(s.macros[0].targets[0].curve.val[0], -1.5f) && near(s.macros[0].targets[0].curve.pos[1], 0.5f));
  CHECK(near(s.macros[0].targets[0].curve.val[2], 0.125f));
  // no trailing newline, CRLF, empty lines
  s = must("\r\n\nvoice bass\r\n\r\nmacro A\r\n cut 0:1");
  CHECK(s.macros[0].numTargets == 1);
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
  expectError("voice bass\nmacro A\n cut 0:1\n cut 0:1 0.1:1 0.2:1 0.3:1 0.4:1 0.5:1 0.6:1 0.7:1 0.8:1\n", 4,
              "more than 8 points");
  {
    std::string t = "voice bass\nmacro A\n";
    for (int i = 0; i < 9; ++i) t += " cut 0:1\n";
    expectError(t.c_str(), 11, "more than 8 targets");
  }
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

bool sameSet(const MacroSet& a, const MacroSet& b) {
  if (a.track != b.track || a.numMacros != b.numMacros || std::strcmp(a.name, b.name) || a.baseMask != b.baseMask)
    return false;
  for (int p = 0; p < kParamsPerTrack; ++p)
    if (((a.baseMask >> p) & 1) && a.base[p] != b.base[p]) return false;
  for (int i = 0; i < a.numMacros; ++i) {
    const Macro &x = a.macros[i], &y = b.macros[i];
    if (std::strcmp(x.name, y.name) || x.start != y.start || x.numTargets != y.numTargets) return false;
    for (int j = 0; j < x.numTargets; ++j) {
      const Target &s = x.targets[j], &t = y.targets[j];
      if (s.param != t.param || s.op != t.op || s.by != t.by || s.curve.n != t.curve.n ||
          s.curve.interp != t.curve.interp)
        return false;
      for (int k = 0; k < s.curve.n; ++k)
        if (s.curve.pos[k] != t.curve.pos[k] || s.curve.val[k] != t.curve.val[k]) return false;
    }
  }
  return true;
}

void roundTrip(const char* text) {
  MacroSet a = must(text), b;
  char buf[4096], buf2[4096];
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
  CHECK(near(s.macros[0].targets[0].curve.val[1], 126.999f, 1e-4f));
  roundTrip("voice clap\nmacro A\n cut 0:0.001 0.999:126.999\n");
}

}  // namespace

int main() {
  testCurves();
  testEvaluate();
  testParseFeatures();
  testParseErrors();
  testRoundTrip();
  std::printf("test_macro_eval: %d checks, %d failed (sizeof(MacroSet) = %zu bytes)\n", gChecks, gFail,
              sizeof(MacroSet));
  return gFail ? 1 : 0;
}
