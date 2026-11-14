// pg::macro: the `vel` and `press` lines of the .pgm format (docs/MACROLAB.md
// "Velocity and pressure") against a small fake parameter table, so this test links
// the macro module alone.
//
//   defaults    no vel line: every amount 0, hasVel false, no offset at any velocity;
//               no press line: what tilt X moves, else what tilt Y moves, smooth;
//               a set without macros: nothing
//   shape       velShape: exactly 0 at 100, +1 at 127, -1 at 1, linear on each side;
//               velOffset in position units
//   lines       vel NAME AMOUNT (signed, 2 decimals, forward references, one per macro,
//               up to four), press NAME [response], press none; the packed byte
//   errors      each with its line
//   format      round trips; only what the file gave is written
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "pg/macro/macro.h"

using namespace pg;
using namespace pg::macro;

namespace {

int gFail = 0, gChecks = 0;
#define CHECK(c)                                                 \
  do {                                                           \
    ++gChecks;                                                   \
    if (!(c)) {                                                  \
      ++gFail;                                                   \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    }                                                            \
  } while (0)

const ParamInfo kCut{"CUT", "Cutoff", 0, 127, 46, ParamFormat::Number, nullptr};
const ParamInfo kRes{"RES", "Resonance", 0, 127, 78, ParamFormat::Number, nullptr};
const ParamInfo kLvl{"LVL", "Level", 0, 127, 100, ParamFormat::Number, nullptr};

ParamTable table() {
  ParamTable t;
  std::memset(&t, 0, sizeof(t));
  for (int tr = 0; tr < kNumTracks; ++tr) {
    t.info[tr][0] = &kCut;
    t.info[tr][1] = &kRes;
    t.info[tr][24] = &kLvl;
  }
  return t;
}
const ParamTable gT = table();

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

bool press(const MacroSet& s, int macro, int response) {
  const PressAssign a = s.pressAssign();
  return a.macro == macro && a.response == response;
}

const char* kThree =
    "voice bass\nname T\n"
    "macro OPEN\n  cut 0:0 1:127\n"
    "macro BITE\n  res 0:0 1:127\n"
    "macro WOBL\n  lvl 0:80 1:100\n";

void testDefaults() {
  std::printf("defaults\n");
  const MacroSet s = must(kThree);
  CHECK(!s.hasVel() && s.vel[0] == 0 && s.vel[1] == 0 && s.vel[2] == 0 && s.vel[3] == 0);
  for (int v = 1; v <= 127; ++v) CHECK(s.velOffset(0, v) == 0.0f);
  CHECK(s.press == 0);
  CHECK(press(s, 0, kTiltDefaultResponse));   // tilt X's macro (macro 1)
  // tilt X moved elsewhere: pressure follows it
  CHECK(press(must((std::string(kThree) + "tilt x WOBL fast\n").c_str()), 2, kTiltDefaultResponse));
  // tilt x none: what Y moves
  CHECK(press(must((std::string(kThree) + "tilt x none\n").c_str()), 1, kTiltDefaultResponse));
  CHECK(press(must((std::string(kThree) + "tilt x none\ntilt y WOBL\n").c_str()), 2, kTiltDefaultResponse));
  // neither axis: nothing
  CHECK(press(must((std::string(kThree) + "tilt x none\ntilt y none\n").c_str()), -1, kTiltDefaultResponse));
  CHECK(press(must("voice bass\nbase cut=10\n"), -1, kTiltDefaultResponse));
  // one macro: X has it
  CHECK(press(must("voice bass\nmacro OPEN\n  cut 0:0 1:127\n"), 0, kTiltDefaultResponse));
}

void testShape() {
  std::printf("shape\n");
  CHECK(velShape(kVelRef) == 0.0f);
  CHECK(velShape(127) == 1.0f);
  CHECK(velShape(1) == -1.0f);
  CHECK(std::fabs(velShape(64) - (64.0f - 100.0f) / 99.0f) < 1e-6f);
  CHECK(std::fabs(velShape(110) - 10.0f / 27.0f) < 1e-6f);
  for (int v = 2; v <= 127; ++v) CHECK(velShape(v) > velShape(v - 1));   // rises everywhere
  const MacroSet s = must((std::string(kThree) + "vel OPEN 0.5\nvel WOBL -0.25\n").c_str());
  CHECK(s.hasVel());
  CHECK(s.velOffset(0, 100) == 0.0f && s.velOffset(2, 100) == 0.0f && s.velOffset(1, 127) == 0.0f);
  CHECK(std::fabs(s.velOffset(0, 127) - 127.5f) < 1e-3f);    // half of 255
  CHECK(std::fabs(s.velOffset(0, 1) + 127.5f) < 1e-3f);
  CHECK(std::fabs(s.velOffset(2, 127) + 63.75f) < 1e-3f);    // negative: a harder hit lowers it
  CHECK(std::fabs(s.velOffset(2, 1) - 63.75f) < 1e-3f);
}

void testLines() {
  std::printf("lines\n");
  MacroSet s = must((std::string(kThree) + "vel open 0.4\nvel BITE -1\npress WOBL fine\n").c_str());
  CHECK(s.vel[0] == 40 && s.vel[1] == -100 && s.vel[2] == 0 && s.vel[3] == 0);
  CHECK(press(s, 2, 2));
  CHECK(s.press == (kTiltGiven | 0x20 | 2));
  // forward references, at the top; any case; a response by name
  s = must("voice bass\nvel BITE 0.05\npress bite WIDE\nmacro OPEN\n  cut 0:0 1:127\nmacro BITE\n  res 0:0 1:127\n");
  CHECK(s.vel[1] == 5 && press(s, 1, 3));
  // between a macro's targets: the next line still belongs to the macro
  s = must("voice bass\nmacro OPEN\n  cut 0:0 1:127\nvel OPEN 1\n  res 0:0 1:64\npress none\n  lvl 0:90 1:100\n");
  CHECK(s.macros[0].n == 3 && s.vel[0] == 100);
  CHECK(press(s, -1, 1) && s.press == (kTiltGiven | 0x10 | kTiltNoMacro));
  // all four macros
  s = must("voice bass\nmacro A\n  cut 0:0 1:1\nmacro B\n  cut 0:0 1:1\nmacro C\n  cut 0:0 1:1\nmacro D\n  cut 0:0 1:1\n"
           "vel A 0.1\nvel B 0.2\nvel C -0.3\nvel D +0.4\n");
  CHECK(s.vel[0] == 10 && s.vel[1] == 20 && s.vel[2] == -30 && s.vel[3] == 40);
  // press and tilt on one macro are allowed (they add)
  s = must((std::string(kThree) + "tilt x OPEN\npress OPEN fast\n").c_str());
  CHECK(press(s, 0, 0) && s.tiltAssign(0).macro == 0);
}

void testErrors() {
  std::printf("errors\n");
  const std::string b = kThree;   // 8 lines
  expectError((b + "vel OPEN\n").c_str(), 9, "write 'vel MACRO AMOUNT'");
  expectError((b + "vel OPEN 0.5 fast\n").c_str(), 9, "write 'vel MACRO AMOUNT'");
  expectError((b + "vel OPEN 1.5\n").c_str(), 9, "must be a number -1..1");
  expectError((b + "vel OPEN -1.01\n").c_str(), 9, "must be a number -1..1");
  expectError((b + "vel OPEN 0.125\n").c_str(), 9, "at most 2 decimals");
  expectError((b + "vel OPEN half\n").c_str(), 9, "must be a number -1..1");
  expectError((b + "vel SHAKE 0.5\n").c_str(), 9, "no macro has that name");
  expectError((b + "vel open 0.5\nvel OPEN 0.2\n").c_str(), 10, "'vel OPEN' given twice");
  expectError((b + "vel A 0.1\nvel B 0.1\nvel C 0.1\nvel D 0.1\nvel E 0.1\n").c_str(), 13, "more than 4 vel lines");
  expectError((b + "press\n").c_str(), 9, "write 'press MACRO");
  expectError((b + "press OPEN fast now\n").c_str(), 9, "write 'press MACRO");
  expectError((b + "press OPEN slow\n").c_str(), 9, "not a press response");
  expectError((b + "press none fast\n").c_str(), 9, "takes no response");
  expectError((b + "press SHAKE\n").c_str(), 9, "no macro has that name");
  expectError((b + "press OPEN\npress BITE\n").c_str(), 10, "'press' given twice");
  expectError((b + "vel \"OPEN\" 0.5\n").c_str(), 9, "only a 'macro' or 'about' line takes strings");
}

void testFormat() {
  std::printf("format\n");
  const char* texts[] = {
      kThree,
      "voice bass\nname T\nvel WOBL -0.35\nvel OPEN 0.6\npress BITE wide\nmacro OPEN\n  cut 0:0 1:127\n"
      "macro BITE\n  res 0:0 1:127\nmacro WOBL\n  lvl 0:80 1:100\n",
      "voice lead\nname U\npress none\nmacro OPEN\n  cut 0:0 1:127\n",
      "voice lead\nname V\npress OPEN smooth\nvel OPEN 0.01\nmacro OPEN\n  cut 0:0 1:127\n",
  };
  for (const char* t : texts) {
    const MacroSet a = must(t);
    char buf[4096];
    const size_t n = format(a, gT, buf, sizeof(buf));
    CHECK(n > 0);
    const MacroSet b = must(buf);
    CHECK(std::memcmp(&a, &b, sizeof(MacroSet)) == 0);
    CHECK((std::strstr(buf, "press ") != nullptr) == (a.press != 0));
    CHECK((std::strstr(buf, "vel ") != nullptr) == a.hasVel());
  }
  char buf[4096];
  format(must(texts[1]), gT, buf, sizeof(buf));
  CHECK(std::strstr(buf, "press BITE wide\nvel OPEN 0.6\nvel WOBL -0.35\n") != nullptr);   // macro order
  format(must(texts[2]), gT, buf, sizeof(buf));
  CHECK(std::strstr(buf, "press none\n") != nullptr);
  format(must(texts[3]), gT, buf, sizeof(buf));
  CHECK(std::strstr(buf, "press OPEN\nvel OPEN 0.01\n") != nullptr);   // smooth is the default: not written
}

}  // namespace

int main() {
  testDefaults();
  testShape();
  testLines();
  testErrors();
  testFormat();
  std::printf("test_macro_vel: %d checks, %d failed\n", gChecks, gFail);
  return gFail ? 1 : 0;
}
