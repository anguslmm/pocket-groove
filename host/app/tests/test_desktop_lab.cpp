// The macro lab's logic (host/app/lab.h) against a fake host, without SDL:
// choosing a set sends its values; only changed values are sent; a value
// edited by hand stays until the set computes a different one; a reload keeps
// positions by macro name and gives back parameters no longer owned; a file
// that turns into another voice, or is deleted, gives every value back; a
// broken file keeps the last good version; "none" restores; positions are
// remembered per file; marks go to the notes file with the note.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "lab.h"
#include "keymap.h"
#include "pg/instrument/instrument.h"

namespace {

int gFail = 0, gChecks = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    ++gChecks;                                                         \
    if (!(c)) {                                                        \
      ++gFail;                                                         \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
    }                                                                  \
  } while (0)

pg::Instrument g_inst;

struct FakeHost : desk::LabHost {
  int sel = pg::kTrackBass;
  int v[pg::kNumTracks][pg::kParamsPerTrack] = {};
  int sends = 0;
  uint32_t locked = 0;
  int selectedTrack() override { return sel; }
  int paramValue(int t, int i) override { return v[t][i]; }
  void setParam(int t, int i, int x) override {
    v[t][i] = x;
    ++sends;
  }
  uint32_t lockedMask(int) override { return locked; }
};

void writeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "w");
  std::fputs(text.c_str(), f);
  std::fclose(f);
}

std::string readFile(const std::string& path) {
  std::string s;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return s;
  char b[1024];
  size_t n;
  while ((n = std::fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

constexpr int CUT = pg::bass::kCutoff, RES = pg::bass::kReso, DRV = pg::bass::kDrive;

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "build/host/desktop_test_lab_unit";
  ::mkdir(dir.c_str(), 0755);
  const std::string macros = dir + "/macros", notes = dir + "/notes.txt";
  ::mkdir(macros.c_str(), 0755);
  std::remove((macros + "/a.pgm").c_str());
  std::remove(notes.c_str());
  writeFile(macros + "/a.pgm", "voice bass\nname A\nmacro X\n cut 0:10 1:110\n res 0:20 1:120\nmacro Y\n drv 0:0 1:100\n");

  g_inst.init(44100.0f, false);
  pg::macro::ParamTable pt;
  for (int t = 0; t < pg::kNumTracks; ++t)
    for (int p = 0; p < pg::kParamsPerTrack; ++p) pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
  FakeHost h;
  for (int t = 0; t < pg::kNumTracks; ++t)
    for (int p = 0; p < pg::kParamsPerTrack; ++p) h.v[t][p] = 50 + p;
  const int cut0 = h.v[pg::kTrackBass][CUT], res0 = h.v[pg::kTrackBass][RES], drv0 = h.v[pg::kTrackBass][DRV];
  desk::Lab lab;
  uint32_t now = 1000;
  lab.init(macros, notes, pt, &h, now);
  auto tick = [&](uint32_t ms) {   // ms of UI frames
    for (uint32_t t = 0; t < ms; t += 33) lab.tick(now += 33);
  };

  std::printf("choose, move, hand edits\n");
  CHECK(lab.selectSet(pg::kTrackBass, "A"));
  tick(33);
  CHECK(h.v[pg::kTrackBass][CUT] == 10 && h.v[pg::kTrackBass][RES] == 20 && h.v[pg::kTrackBass][DRV] == 0);
  CHECK(lab.synced());
  int s = h.sends;
  tick(200);
  CHECK(h.sends == s);   // nothing moved: nothing sent
  CHECK(lab.setMacro(pg::kTrackBass, 1, 0.5f));
  tick(33);
  CHECK(h.v[pg::kTrackBass][DRV] == 50 && h.sends == s + 1);   // only the changed value
  h.v[pg::kTrackBass][CUT] = 99;   // edited by hand
  tick(100);
  CHECK(h.v[pg::kTrackBass][CUT] == 99);   // kept while X does not move
  CHECK(!lab.synced());
  CHECK(lab.describe(pg::kTrackBass).find("CUT=99*") != std::string::npos);
  lab.setMacro(pg::kTrackBass, 0, 0.5f);
  tick(33);
  CHECK(h.v[pg::kTrackBass][CUT] == 60 && h.v[pg::kTrackBass][RES] == 70);   // the macro wins on its move
  CHECK(lab.synced());
  h.locked = 1u << RES;
  CHECK(lab.describe(pg::kTrackBass).find("RES=70~") != std::string::npos);
  h.locked = 0;

  std::printf("keyboard actions\n");
  lab.action(desk::kActLabMacro1 + 1, false);
  lab.action(desk::kActLabUp, false);
  tick(33);
  CHECK(h.v[pg::kTrackBass][DRV] == 55);
  lab.action(desk::kActLabDown, true);
  tick(33);
  CHECK(h.v[pg::kTrackBass][DRV] == 54);
  lab.action(desk::kActLabMacro1 + 3, false);   // no 4th macro: focus stays
  lab.action(desk::kActLabUp, false);
  tick(33);
  CHECK(h.v[pg::kTrackBass][DRV] == 59);

  std::printf("reload: positions by name, given-back parameters\n");
  writeFile(macros + "/a.pgm", "voice bass\nname A\nmacro NEW 0.25\n fold 0:0 1:100\nmacro X\n cut 0:30 1:50\n");
  tick(400);
  CHECK(lab.reloads() == 1);
  CHECK(h.v[pg::kTrackBass][CUT] == 40);   // X kept 0.5 by name, new curve
  CHECK(h.v[pg::kTrackBass][pg::bass::kFold] == 25);   // NEW at its start
  CHECK(h.v[pg::kTrackBass][RES] == res0 && h.v[pg::kTrackBass][DRV] == drv0);   // no longer owned: given back
  std::printf("broken file: the last good version stays\n");
  writeFile(macros + "/a.pgm", "voice bass\nmacro X\n cut 0:30 1:500\n");
  tick(400);
  CHECK(lab.errors() == 1 && lab.reloads() == 1);
  CHECK(h.v[pg::kTrackBass][CUT] == 40 && h.v[pg::kTrackBass][pg::bass::kFold] == 25);
  std::printf("none, and back: positions remembered\n");
  CHECK(lab.selectSet(pg::kTrackBass, "none"));
  CHECK(h.v[pg::kTrackBass][CUT] == cut0 && h.v[pg::kTrackBass][pg::bass::kFold] == 50 + pg::bass::kFold);
  CHECK(lab.selectSet(pg::kTrackBass, "a.pgm"));
  tick(33);
  CHECK(h.v[pg::kTrackBass][CUT] == 40);
  std::printf("the file turns into another voice: the bass gets everything back\n");
  writeFile(macros + "/a.pgm", "voice lead\nmacro X\n cut 0:30 1:50\n");
  tick(400);
  CHECK(lab.errors() == 0);
  CHECK(h.v[pg::kTrackBass][CUT] == cut0 && h.v[pg::kTrackBass][pg::bass::kFold] == 50 + pg::bass::kFold);
  CHECK(lab.selectSet(pg::kTrackLead, "a"));
  CHECK(!lab.selectSet(pg::kTrackBass, "a"));
  tick(33);
  CHECK(h.v[pg::kTrackLead][0] == 40);   // X remembered for this file (0.5)
  std::printf("deleted while active: back to the values from before\n");
  std::remove((macros + "/a.pgm").c_str());
  tick(400);
  CHECK(h.v[pg::kTrackLead][0] == 50);
  std::printf("model v2: a morph, end labels, the about line, warnings\n");
  {
    const char* v2 = "# B -- family: TEST.\nvoice bass\nname B\nbase cut=20\npatch HI cut=100\n"
                     "macro M \"LOW\" \"HIGH\" \"a test macro\"\n morph HI\nmacro N\n res 0:10 1:20\n res 0:30 1:40\n";
    writeFile(macros + "/b.pgm", v2);
    tick(400);
    CHECK(lab.errors() == 0 && lab.warnings() == 1);   // the second plain set on RES wipes out the first
    CHECK(lab.selectSet(pg::kTrackBass, "B"));
    CHECK(lab.setMacro(pg::kTrackBass, 0, 0.5f));
    tick(33);
    CHECK(h.v[pg::kTrackBass][CUT] == 60 && h.v[pg::kTrackBass][RES] == 30);
    h.sel = pg::kTrackBass;
    static uint8_t a[desk::kLabW * desk::kCanvasH], b[desk::kLabW * desk::kCanvasH];
    lab.draw(a);
    std::string plain = v2;
    plain.replace(plain.find(" \"LOW\" \"HIGH\" \"a test macro\""), 28, "");
    plain.replace(0, plain.find('\n') + 1, "");   // no about line
    writeFile(macros + "/b.pgm", plain + " res 0:1 override\n");
    tick(400);
    CHECK(lab.warnings() == 1);   // the override line is not warned about, the earlier one still is
    tick(3000);   // let the RELOADED notice pass
    lab.draw(b);
    CHECK(std::memcmp(a, b, sizeof(a)) != 0);   // the labels, the about line, the description are drawn
    CHECK(lab.selectSet(pg::kTrackBass, "none"));
    std::remove((macros + "/b.pgm").c_str());
    tick(400);
  }
  std::printf("marks\n");
  h.sel = pg::kTrackSnare;
  lab.action(desk::kActLabGood, false);
  CHECK(lab.typing());
  lab.typeText("nice ");
  lab.typeText("snap\x01");
  lab.typeKey(desk::Lab::kTypeBackspace);
  lab.typeKey(desk::Lab::kTypeEnter);
  CHECK(!lab.typing());
  lab.action(desk::kActLabBad, false);
  lab.typeText("ignored");
  lab.typeKey(desk::Lab::kTypeEscape);
  lab.action(desk::kActLabBad, false);
  lab.finishTyping(false);   // quit while typing
  const std::string n = readFile(notes);
  CHECK(n.find(" GOOD snare none - | TUNE=50 ") != std::string::npos && n.find("| nice sna\n") != std::string::npos);
  CHECK(n.find(" BAD  snare none - | TUNE=50 ") != std::string::npos && n.find("ignored") == std::string::npos);
  int lines = 0;
  for (char c : n) lines += c == '\n';
  CHECK(lines == 3);
  // the panel draws without a set, with one, while typing (smoke: no crash, something drawn)
  static uint8_t px[desk::kLabW * desk::kCanvasH];
  lab.draw(px);
  int lit = 0;
  for (uint8_t p : px) lit += p != 0;
  CHECK(lit > 500);
  std::printf("test_desktop_lab: %d checks, %d failed\n", gChecks, gFail);
  return gFail ? 1 : 0;
}
