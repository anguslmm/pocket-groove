// The macro lab's logic (host/app/lab.h) against the real UI and engine (the
// host harness, no SDL): choosing a set in the lab chooses the track's
// instrument; the sliders move the track's own macro positions (the same the
// SOUND page shows); a .pgm file overrides the built-in instrument of its
// voice and name while the lab runs (hot reload while the engine plays, the
// replaced set's memory kept until the engine counted the refresh); a file
// with no built-in counterpart is an extra instrument in the SOUND list; a
// reload keeps positions by macro name; a broken file keeps the last good
// version; a deleted file gives the built-in back (or, for an extra, the track
// keeps playing its stored values); "none" is CLASSIC; positions are
// remembered per instrument; marks go to the notes file in their format.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common/ui_harness.h"
#include "keymap.h"
#include "lab.h"
#include "pg/engine/engine_hooks.h"

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

namespace pm = pg::macro;

uih::Rig* R;

struct RigHost : desk::LabHost {
  int selectedTrack() override { return R->ui.selTrack(); }
  int paramValue(int t, int i) override { return R->ui.paramValue(t, i); }
  int projectValue(int t, int i) override {
    const pg::TrackSettings& ts = R->proj->tracks[t & 7];
    return i >= pg::kParamsPerTrack ? ts.macro[(i - pg::kParamsPerTrack) & 3] : ts.params[i & 31];
  }
  uint16_t instrument(int t) override { return R->ui.instrumentIdOf(t); }
  uint16_t projectInstrument(int t) override { return pg::instrumentId(R->proj->tracks[t & 7]); }
  void setMacro(int t, int m, int v) override { R->ui.setMacro(t, m, v); }
  void setInstrument(int t, uint16_t id, const uint8_t* pos) override { R->ui.setInstrument(t, id, pos); }
  void refreshInstruments() override {
    pg::Command c{};
    c.type = pg::CmdType::RefreshInstruments;
    c.a = 0xFF;
    R->eng.post(c);
    ++refreshes;
  }
  uint8_t instrumentRefreshes() override { return R->eng.instrumentRefreshes(); }
  uint32_t lockedMask(int) override { return locked; }
  uint32_t locked = 0;
  int refreshes = 0;
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
int P(int i) { return R->proj->tracks[pg::kTrackBass].params[i]; }
int MAC(int m) { return R->proj->tracks[pg::kTrackBass].macro[m]; }
uint16_t ID() { return pg::instrumentId(R->proj->tracks[pg::kTrackBass]); }

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "build/host/desktop_test_lab_unit";
  ::mkdir(dir.c_str(), 0755);
  const std::string macros = dir + "/macros", notes = dir + "/notes.txt";
  ::mkdir(macros.c_str(), 0755);
  for (const char* f : {"/a.pgm", "/acid.pgm", "/b.pgm"}) std::remove((macros + f).c_str());
  std::remove(notes.c_str());
  writeFile(macros + "/a.pgm", "voice bass\nname A\nmacro X\n cut 0:10 1:110\n res 0:20 1:120\nmacro Y\n drv 0:0 1:100\n");

  static uih::Rig rig;
  R = &rig;
  rig.start(true);   // the mockup project: CLASSIC everywhere
  pm::ParamTable pt;
  for (int t = 0; t < pg::kNumTracks; ++t)
    for (int p = 0; p < pg::kParamsPerTrack; ++p) pt.info[t][p] = rig.eng.trackParamInfo(t, p);
  RigHost h;
  static desk::Lab lab;
  lab.init(macros, notes, pt, &h, rig.now());
  auto tick = [&](uint32_t ms) {   // ms of UI frames (the engine runs in between)
    for (uint32_t t = 0; t < ms; t += 33) {
      rig.run(33);
      lab.tick(rig.now());
    }
  };
  // the engine plays all along: hot reload while playing
  rig.tap(pg::ui::keyFromName("space"));
  tick(100);
  const int drvClassic = P(DRV);

  std::printf("an extra instrument: choose, move (the track's own positions)\n");
  CHECK(pm::instrumentCount(pg::kTrackBass) == pm::instrumentIndex(pg::kTrackBass, pm::setId(pg::kTrackBass, "A")) + 1);
  CHECK(lab.selectSet(pg::kTrackBass, "A"));
  tick(66);
  CHECK(ID() == pm::setId(pg::kTrackBass, "A"));
  CHECK(P(CUT) == 10 && P(RES) == 20 && P(DRV) == 0);
  CHECK(lab.synced());
  CHECK(lab.setMacro(pg::kTrackBass, 1, 0.5f));
  tick(66);
  CHECK(MAC(1) == 128 && P(DRV) == 50);
  CHECK(R->ui.macroValue(pg::kTrackBass, 1) == 128);   // the SOUND page shows the same position
  R->ui.setMacro(pg::kTrackBass, 0, 128);               // ... and moving it there moves the lab's
  tick(66);
  CHECK(P(CUT) == 60 && P(RES) == 70);
  CHECK(lab.describe(pg::kTrackBass).find("A " + macros + "/a.pgm@") != std::string::npos);
  CHECK(lab.describe(pg::kTrackBass).find("X=0.50 Y=0.50 | CUT=60 RES=70 DRV=50") != std::string::npos);
  h.locked = 1;
  CHECK(lab.describe(pg::kTrackBass).find("X=0.50~") != std::string::npos);
  h.locked = 0;

  std::printf("keyboard actions\n");
  lab.action(desk::kActLabMacro1 + 1, false);
  lab.action(desk::kActLabUp, false);
  tick(66);
  CHECK(MAC(1) == 141 && P(DRV) == 55);
  lab.action(desk::kActLabDown, true);
  tick(66);
  CHECK(MAC(1) == 138 && P(DRV) == 54);

  std::printf("reload while playing: positions by name, the replaced set is retired\n");
  writeFile(macros + "/a.pgm", "voice bass\nname A\nmacro NEW 0.25\n fold 0:0 1:100\nmacro X\n cut 0:30 1:50\n");
  tick(400);
  CHECK(lab.reloads() == 1);
  CHECK(MAC(1) == 128 && P(CUT) == 40);           // X kept its 0.5 by name, the new curve plays
  CHECK(MAC(0) == 64 && P(pg::bass::kFold) == 25); // NEW at its start
  CHECK(P(RES) == pt.info[pg::kTrackBass][RES]->def);   // no longer owned: the voice's default
  tick(100);
  CHECK(lab.retired() == 0);                       // the engine let go of the old version
  CHECK(lab.synced());
  std::printf("broken file: the last good version stays\n");
  writeFile(macros + "/a.pgm", "voice bass\nname A\nmacro X\n cut 0:30 1:500\n");
  tick(400);
  CHECK(lab.errors() == 1 && lab.reloads() == 1);
  CHECK(P(CUT) == 40 && P(pg::bass::kFold) == 25);

  std::printf("a file replacing a built-in: the built-in's id, the file's sound\n");
  const pm::MacroSet* builtinAcid = pm::findBuiltin(pm::setId(pg::kTrackBass, "ACID"));
  CHECK(builtinAcid != nullptr);
  CHECK(lab.selectSet(pg::kTrackBass, "ACID"));
  tick(66);
  CHECK(ID() == builtinAcid->id);
  const int acidCut = P(CUT);
  writeFile(macros + "/acid.pgm", "voice bass\nname ACID\nmacro OPEN\n cut 0:3 1:3\n");
  tick(400);
  CHECK(pm::findInstrument(builtinAcid->id) != builtinAcid);           // the file wins
  CHECK(pm::instrumentAt(pg::kTrackBass, 0) == pm::findInstrument(builtinAcid->id));   // in its place in the list
  CHECK(P(CUT) == 3 && ID() == builtinAcid->id);
  CHECK(lab.describe(pg::kTrackBass).find("ACID " + macros + "/acid.pgm@") != std::string::npos);
  std::remove((macros + "/acid.pgm").c_str());
  tick(400);
  CHECK(pm::findInstrument(builtinAcid->id) == builtinAcid);           // removed: the built-in is back
  CHECK(P(CUT) == acidCut);
  CHECK(lab.describe(pg::kTrackBass).find("ACID builtin@00000000") != std::string::npos);

  std::printf("none = CLASSIC, and back: positions remembered\n");
  CHECK(lab.selectSet(pg::kTrackBass, "A"));
  tick(66);
  CHECK(MAC(1) == 128 && P(CUT) == 40);           // remembered for A
  CHECK(lab.selectSet(pg::kTrackBass, "none"));
  tick(66);
  CHECK(ID() == 0 && P(CUT) == 40);                // CLASSIC: the values stay
  CHECK(lab.describe(pg::kTrackBass).find("bass none - | CUT=40 ") != std::string::npos);
  CHECK(lab.selectSet(pg::kTrackBass, "a.pgm"));
  tick(66);
  CHECK(P(CUT) == 40 && MAC(1) == 128);

  std::printf("the file turns into another voice; then it is deleted\n");
  writeFile(macros + "/a.pgm", "voice lead\nname A\nmacro X\n cut 0:30 1:50\n");
  tick(400);
  CHECK(lab.errors() == 0);
  CHECK(pm::findInstrument(pm::setId(pg::kTrackBass, "A")) == nullptr);   // the bass's A is gone ...
  CHECK(ID() == pm::setId(pg::kTrackBass, "A") && P(CUT) == 40);         // ... the track keeps playing its values
  CHECK(lab.selectSet(pg::kTrackLead, "a"));
  CHECK(!lab.selectSet(pg::kTrackBass, "a"));
  tick(66);
  CHECK(R->proj->tracks[pg::kTrackLead].params[0] == 30);
  std::remove((macros + "/a.pgm").c_str());
  tick(400);
  CHECK(R->proj->tracks[pg::kTrackLead].params[0] == 30);   // an unknown id plays its stored values
  CHECK(lab.retired() == 0);
  (void)drvClassic;

  std::printf("model v2: a morph, end labels, the about line, warnings; a file without a name\n");
  {
    const char* v2 = "# B -- family: TEST.\nvoice bass\nname B\nbase cut=20\npatch HI cut=100\n"
                     "macro M \"LOW\" \"HIGH\" \"a test macro\"\n morph HI\nmacro N\n res 0:10 1:20\n res 0:30 1:40\n";
    writeFile(macros + "/b.pgm", v2);
    tick(400);
    CHECK(lab.errors() == 0 && lab.warnings() == 1);   // the second plain set on RES wipes out the first
    CHECK(lab.selectSet(pg::kTrackBass, "B"));
    CHECK(lab.setMacro(pg::kTrackBass, 0, 0.5f));
    tick(66);
    CHECK(P(CUT) == 60 && P(RES) == 30);
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
    writeFile(macros + "/b.pgm", "voice bass\nmacro Q\n cut 0:1 1:2\n");   // no name line: named after the file
    tick(400);
    CHECK(lab.selectSet(pg::kTrackBass, "B"));
    tick(66);
    CHECK(P(CUT) == 1);
    CHECK(lab.selectSet(pg::kTrackBass, "none"));
    std::remove((macros + "/b.pgm").c_str());
    tick(400);
  }
  std::printf("marks\n");
  R->ui.setInstrument(pg::kTrackSnare, 0);
  tick(66);
  rig.down(pg::ui::keyFromName("shift"));
  rig.tap(pg::ui::keyFromName("s"));
  rig.up(pg::ui::keyFromName("shift"));
  tick(66);
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
  CHECK(n.find(" GOOD snare none - | TUNE=") != std::string::npos && n.find("| nice sna\n") != std::string::npos);
  CHECK(n.find(" BAD  snare none - | TUNE=") != std::string::npos && n.find("ignored") == std::string::npos);
  int lines = 0;
  for (char c : n) lines += c == '\n';
  CHECK(lines == 3);
  // the panel draws without a set, with one, while typing (smoke: no crash, something drawn)
  static uint8_t px[desk::kLabW * desk::kCanvasH];
  lab.draw(px);
  int lit = 0;
  for (uint8_t p : px) lit += p != 0;
  CHECK(lit > 500);
  CHECK(pg::engine_detail::faultCount() == 0);
  pm::setLibraryOverlay(nullptr);
  std::printf("test_desktop_lab: %d checks, %d failed\n", gChecks, gFail);
  return gFail ? 1 : 0;
}
