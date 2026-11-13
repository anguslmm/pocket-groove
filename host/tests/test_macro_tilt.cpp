// pg::macro: the `tilt` lines of the .pgm format (docs/MACROLAB.md "Tilt") against a
// small fake parameter table, so this test links the macro module alone.
//
//   default     no tilt line: X = macro 1, Y = macro 2, smooth; a set with one macro:
//               Y moves nothing; a set with none: nothing
//   lines       tilt x NAME [fast|smooth|fine|wide], tilt y NAME, tilt x none; names
//               case-insensitive, forward references (before the macro line), anywhere
//               in the file (also between a macro's targets); the packed byte
//   errors      each with its line: axis, twice, unknown macro, unknown response,
//               none with a response, too few / many words, both axes on one macro
//   format      round trips (only what the file gave is written; smooth is not written)
//   size        MacroSet grew by the two bytes (and their alignment)
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

bool is(const MacroSet& s, int axis, int macro, int response) {
  const TiltAssign a = s.tiltAssign(axis);
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
  CHECK(s.tilt[0] == 0 && s.tilt[1] == 0);
  CHECK(is(s, 0, 0, kTiltDefaultResponse) && is(s, 1, 1, kTiltDefaultResponse));
  const MacroSet one = must("voice bass\nmacro OPEN\n  cut 0:0 1:127\n");
  CHECK(is(one, 0, 0, 1) && is(one, 1, -1, 1));
  const MacroSet none = must("voice bass\nbase cut=10\n");
  CHECK(is(none, 0, -1, 1) && is(none, 1, -1, 1));
  CHECK(kTiltDefaultResponse == 1);   // smooth (pg/tilt/tilt.h kSmooth)
}

void testLines() {
  std::printf("lines\n");
  std::string t = std::string(kThree) + "tilt x wobl fast\ntilt y OPEN\n";
  MacroSet s = must(t.c_str());
  CHECK(is(s, 0, 2, 0) && is(s, 1, 0, 1));
  CHECK(s.tilt[0] == (kTiltGiven | 0x00 | 2) && s.tilt[1] == (kTiltGiven | 0x10 | 0));
  // forward references, at the top; responses by name, any case
  s = must("voice bass\ntilt y BITE WIDE\ntilt x none\nmacro OPEN\n  cut 0:0 1:127\nmacro BITE\n  res 0:0 1:127\n");
  CHECK(is(s, 0, -1, 1) && is(s, 1, 1, 3));
  CHECK(s.tilt[0] == (kTiltGiven | 0x10 | kTiltNoMacro));
  // between a macro's targets: the next line still belongs to the macro
  s = must("voice bass\nmacro OPEN\n  cut 0:0 1:127\ntilt x OPEN fine\n  res 0:0 1:64\nmacro BITE\n  lvl 0:90 1:100\n");
  CHECK(is(s, 0, 0, 2) && is(s, 1, 1, 1));
  CHECK(s.macros[0].n == 2);
  // only one axis given: the other keeps its default
  s = must((std::string(kThree) + "tilt y WOBL\n").c_str());
  CHECK(is(s, 0, 0, 1) && is(s, 1, 2, 1) && s.tilt[0] == 0);
  // X on macro 2 with Y left at its default (macro 2): Y moves nothing (never two axes
  // on one macro); the same the other way round
  s = must((std::string(kThree) + "tilt x BITE\n").c_str());
  CHECK(is(s, 0, 1, 1) && is(s, 1, -1, 1));
  s = must((std::string(kThree) + "tilt y OPEN fine\n").c_str());
  CHECK(is(s, 0, -1, 1) && is(s, 1, 0, 2));
}

void testErrors() {
  std::printf("errors\n");
  const std::string b = kThree;   // 8 lines
  expectError((b + "tilt z OPEN\n").c_str(), 9, "axis is x");
  expectError((b + "tilt x OPEN\ntilt x BITE\n").c_str(), 10, "given twice");
  expectError((b + "tilt x SHAKE\n").c_str(), 9, "no macro has that name");
  expectError((b + "tilt x LONGNAMEX\n").c_str(), 9, "no macro has that name");
  expectError((b + "tilt x OPEN slow\n").c_str(), 9, "not a tilt response");
  expectError((b + "tilt y none fast\n").c_str(), 9, "takes no response");
  expectError((b + "tilt x\n").c_str(), 9, "write 'tilt x MACRO");
  expectError((b + "tilt x OPEN fast now\n").c_str(), 9, "write 'tilt x MACRO");
  expectError((b + "tilt x OPEN\ntilt y open fine\n").c_str(), 10, "both tilt axes move OPEN");
  expectError((b + "tilt x \"OPEN\"\n").c_str(), 9, "only a 'macro' or 'about' line takes strings");
}

void testFormat() {
  std::printf("format\n");
  const char* texts[] = {
      kThree,
      "voice bass\nname T\ntilt x WOBL fast\ntilt y none\nmacro OPEN\n  cut 0:0 1:127\nmacro BITE\n  res 0:0 1:127\n"
      "macro WOBL\n  lvl 0:80 1:100\n",
      "voice lead\nname U\ntilt y OPEN wide\nmacro OPEN\n  cut 0:0 1:127\n",
      "voice lead\nname V\ntilt x OPEN smooth\nmacro OPEN\n  cut 0:0 1:127\n",
  };
  for (const char* t : texts) {
    const MacroSet a = must(t);
    char buf[4096];
    const size_t n = format(a, gT, buf, sizeof(buf));
    CHECK(n > 0);
    const MacroSet b = must(buf);
    CHECK(std::memcmp(&a, &b, sizeof(MacroSet)) == 0);
    CHECK((std::strstr(buf, "tilt ") != nullptr) == ((a.tilt[0] | a.tilt[1]) != 0));
    CHECK(std::strstr(buf, " smooth\n") == nullptr || std::strstr(buf, "tilt") == nullptr ||
          std::strstr(std::strstr(buf, "tilt"), " smooth\n") == nullptr);
  }
  char buf[4096];
  format(must(texts[1]), gT, buf, sizeof(buf));
  CHECK(std::strstr(buf, "tilt x WOBL fast\ntilt y none\n") != nullptr);
  format(must(texts[3]), gT, buf, sizeof(buf));
  CHECK(std::strstr(buf, "tilt x OPEN\n") != nullptr);   // smooth is the default: not written
}

void testSize() {
  std::printf("size\n");
  std::printf("  sizeof(MacroSet) = %zu\n", sizeof(MacroSet));
  CHECK(sizeof(MacroSet) <= 1900);   // 1896 before the tilt bytes: +2, rounded up by the alignment
}

}  // namespace

int main() {
  testDefaults();
  testLines();
  testErrors();
  testFormat();
  testSize();
  std::printf("test_macro_tilt: %d checks, %d failed\n", gChecks, gFail);
  return gFail ? 1 : 0;
}
