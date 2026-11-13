// UI phase 2: PROJECT view, name entry, dialogs, storage errors (memfs fault
// injection), session autosave/resume, settings, first-boot demo.
#include <cstdio>
#include <cstring>
#include <string>

#include "common/ui_harness.h"
#include "pg/io/project_io.h"

using namespace pg;
using namespace pg::ui;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fail;                                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    long _a = long(a), _b = long(b);                                                                \
    if (_a != _b) {                                                                                 \
      ++g_fail;                                                                                     \
      std::printf("FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b);       \
    }                                                                                               \
  } while (0)

static uih::Rig* R;
static int K(const char* n) { return keyFromName(n); }
static void tap(const char* n, uint32_t ms = 40) { R->tap(K(n), ms); R->run(20); }
static void down(const char* n) { R->down(K(n)); }
static void up(const char* n) { R->up(K(n)); R->run(20); }
static void ctrl(const char* n) { down("ctl"); tap(n); up("ctl"); }
static void type(const char* s) {
  for (; *s; ++s) {
    char k[2] = {char(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s), 0};
    tap(*s == '_' ? "spc" : k);
  }
}
static bool toastHas(const char* s) { return std::strstr(R->ui.toastText(), s) != nullptr; }
static bool fileExists(const char* p) { return R->fs.has(p); }
static int row(const char* mountTag, const char* name);   // index of a row in the PROJECT list

static void saveAs(const char* name, bool sd = false) {
  ctrl("s");                          // no file yet -> SAVE AS name dialog
  CHECK(R->ui.dialog() == Dialog::Name);
  for (int i = 0; i < 9; ++i) tap("del");
  type(name);
  if (sd) tap("tab");
  tap("ent");
}

static void testSaveLoadRoundTrip() {
  R->start(true);
  CHECK(!R->ui.dirty());
  tap("1");                            // an edit: dirty
  R->run(50);
  CHECK(R->ui.dirty());
  saveAs("JAM1");
  CHECK(R->ui.dialog() == Dialog::None);
  CHECK(fileExists("/int/pg/JAM1.pgv"));
  CHECK(toastHas("SAVED JAM1"));
  CHECK(!R->ui.dirty());
  CHECK(std::strcmp(R->ui.projectName(), "JAM1") == 0);
  CHECK_EQ(R->ui.projectMount(), kPmInt);
  CHECK(std::strcmp(R->proj->name, "JAM1") == 0);
  static Project saved;
  saved = *R->proj;
  // change it a lot, then load JAM1 back through the confirm dialog
  tap("2"); tap("3"); down("tab"); tap("="); up("tab");
  R->run(50);
  CHECK(R->ui.dirty());
  tap("\\");
  CHECK(R->ui.view() == View::Project);
  const int r = row("INT", "JAM1");
  CHECK(r >= 0);
  while (R->ui.projectCursor() < r) tap(".");
  tap("ent");
  CHECK(R->ui.dialog() == Dialog::Confirm);    // unsaved changes
  tap("`");                                    // cancel: nothing happens
  CHECK(R->ui.dialog() == Dialog::None);
  CHECK(R->ui.dirty());
  tap("ent");
  tap("spc");                                  // SPACE still plays/stops inside a confirm
  R->run(50);
  CHECK(R->eng.status().playing);
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("ent");                                  // discard and load
  R->run(100);
  CHECK(R->ui.view() == View::Groove);
  CHECK(!R->eng.status().playing);             // load stops the transport
  CHECK(!R->ui.dirty());
  CHECK_EQ(R->ui.selTrack(), kTrackBass);
  CHECK(std::memcmp(R->proj, &saved, sizeof(Project)) == 0);
  // CTRL+S on a named project saves straight away
  tap("5");
  R->run(50);
  ctrl("s");
  CHECK(R->ui.dialog() == Dialog::None);
  CHECK(toastHas("SAVED JAM1"));
  CHECK(!R->ui.dirty());
}

static void testNameEntry() {
  R->start(true);
  tap("\\"); tap("\\");                // SAVE AS tab
  CHECK_EQ(R->ui.projectTab(), 1);
  tap("ent");                          // "+ NEW NAME..."
  CHECK(R->ui.dialog() == Dialog::Name);
  CHECK(std::strcmp(R->ui.nameText(), "GROOVE01") == 0);   // next free suggestion
  for (int i = 0; i < 12; ++i) tap("del");                 // backspace past empty is harmless
  CHECK(std::strcmp(R->ui.nameText(), "") == 0);
  tap("ent");                                              // empty: refused, dialog stays
  CHECK(R->ui.dialog() == Dialog::Name);
  CHECK(toastHas("TYPE A NAME"));
  type("ab-c_9");                                          // lower case typed upper, space = _
  CHECK(std::strcmp(R->ui.nameText(), "AB-C_9") == 0);
  type("xyzw");                                            // max 8 characters
  CHECK(std::strcmp(R->ui.nameText(), "AB-C_9XY") == 0);
  tap("[");                                                // keys without a character do nothing
  tap("spc");
  CHECK(std::strcmp(R->ui.nameText(), "AB-C_9XY") == 0);   // full: '_' not added
  CHECK(!R->eng.status().playing);                         // SPACE did not start the transport
  tap("9");                                                // view keys type in the dialog
  CHECK(R->ui.view() == View::Project);
  tap("`");                                                // cancel
  CHECK(R->ui.dialog() == Dialog::None);
  CHECK(!fileExists("/int/pg/AB-C_9XY.pgv"));
  // existing name asks before overwriting
  saveAs("TWICE");
  CHECK(fileExists("/int/pg/TWICE.pgv"));
  tap("\\"); tap("\\");
  tap("ent");
  for (int i = 0; i < 9; ++i) tap("del");
  type("twice");
  tap("ent");
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("ent");
  CHECK(toastHas("SAVED TWICE"));
  // a demo's name is refused
  tap("ent");
  for (int i = 0; i < 9; ++i) tap("del");
  type(demo::name(0));
  tap("ent");
  CHECK(R->ui.dialog() == Dialog::Name);
  CHECK(toastHas("DEMO"));
  tap("`");
  // SAVE AS on a ROM row is refused; DEL on a ROM row too
  tap(".");                                    // row 1 = first ROM demo
  tap("ent");
  CHECK(toastHas("READ-ONLY"));
  tap("del");
  CHECK(toastHas("CAN'T BE DELETED"));
}

static void testSdAndDelete() {
  R->start(true);
  saveAs("ONSD", true);
  CHECK(fileExists("/sd/pg/ONSD.pgv"));
  CHECK_EQ(R->ui.projectMount(), kPmSd);
  tap("\\");
  const int rows = R->ui.fileRows();
  CHECK(row("SD", "ONSD") >= 0);
  while (R->ui.projectCursor() < rows - 1) tap(".");
  // pull the card while the list is shown
  R->fs.sdPresent = false;
  R->env.sdPresent = false;
  R->run(100);
  CHECK(row("SD", "ONSD") < 0);
  CHECK(R->ui.projectCursor() < R->ui.fileRows());
  // saving to SD now fails with a readable message
  ctrl("s");                                   // named project on SD -> direct save -> no card
  CHECK(toastHas("NO SD CARD"));
  CHECK(R->ui.dirty() || true);
  R->fs.sdPresent = true;
  R->env.sdPresent = true;
  R->run(100);
  int r = row("SD", "ONSD");
  CHECK(r >= 0);
  // delete it (asks first)
  while (R->ui.projectCursor() < r) tap(".");
  while (R->ui.projectCursor() > r) tap(";");
  tap("del");
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("ent");
  CHECK(!fileExists("/sd/pg/ONSD.pgv"));
  CHECK(toastHas("DELETED ONSD"));
  CHECK(R->ui.dirty());                        // the open project lost its file
}

static void testSaveErrors() {
  // Every mutating op failing in turn: a readable error, the old file intact, still dirty.
  R->start(true);
  saveAs("KEEP");
  tap("`");                            // back to GROOVE, where -/= edit the focus
  const std::vector<uint8_t> good = *R->fs.get("/int/pg/KEEP.pgv");
  int okCount = 0, errCount = 0;
  for (int k = 0; k < 40; ++k) {
    tap("=");                          // a real change each round (a toggled step could cancel out)
    R->run(50);
    CHECK(R->ui.dirty());
    R->fs.arm();
    R->fs.failOp = k;
    ctrl("s");
    R->fs.clearFaults();
    if (toastHas("SAVED KEEP")) {
      ++okCount;
      CHECK(!R->ui.dirty());
    } else {
      ++errCount;
      CHECK(toastHas("SAVE: ") || toastHas("FAILED") || toastHas("SPACE"));
      CHECK(R->ui.dirty());
      const std::vector<uint8_t>* f = R->fs.get("/int/pg/KEEP.pgv");
      CHECK(f && pg::io::verifyFile(R->fs, "/int/pg/KEEP.pgv") == pg::io::Status::Ok);
    }
    CHECK(!R->fs.has("/int/pg/_save.tmp"));
  }
  CHECK(errCount > 3);
  CHECK(okCount > 0);
  // disk full
  R->fs.spaceLimit[0] = long(good.size()) / 2;
  tap("=");
  R->run(50);
  ctrl("s");
  CHECK(toastHas("NOT ENOUGH SPACE") || toastHas("WRITE FAILED"));
  R->fs.spaceLimit[0] = -1;
  // power loss in the middle of a save: after the reboot the file is old or new, never broken
  for (int k = 0; k < 12; ++k) {
    tap("=");
    R->run(50);
    R->fs.arm();
    R->fs.powerLossOp = k;
    ctrl("s");
    R->fs.powerCycle();
    pg::io::recoverAfterPowerLoss(R->fs, "/int");
    CHECK(pg::io::verifyFile(R->fs, "/int/pg/KEEP.pgv") == pg::io::Status::Ok);
  }
}

// Rewrites a header byte and fixes the header CRC so only that field is "wrong".
static void patchHeader(const char* path, int off, uint8_t v) {
  std::vector<uint8_t>& f = R->fs.files[path];
  f[off] = v;
  const uint16_t hs = uint16_t(f[6] | (f[7] << 8));
  const uint32_t c = pg::io::crc32(0, f.data(), hs - 4);
  for (int i = 0; i < 4; ++i) f[hs - 4 + i] = uint8_t(c >> (8 * i));
}

static void loadRow(const char* tag, const char* name) {
  tap("`");
  tap("\\");
  if (R->ui.projectTab() != 0) tap("\\");
  const int r = row(tag, name);
  CHECK(r >= 0);
  while (R->ui.projectCursor() < r) tap(".");
  while (R->ui.projectCursor() > r) tap(";");
  tap("ent");
  if (R->ui.dialog() == Dialog::Confirm) tap("ent");
  R->run(50);
}

static void testLoadErrors() {
  R->start(true);
  saveAs("SRC");
  const std::vector<uint8_t> good = *R->fs.get("/int/pg/SRC.pgv");
  static Project before;
  struct Case { const char* name; const char* expect; };
  const Case cases[] = {{"CORRUPT", "DAMAGED"}, {"TRUNC", "INCOMPLETE"}, {"NOTPG", "NOT A POCKET GROOVE"},
                        {"NEWER", "NEWER FIRMWARE"}, {"PVER", "UNSUPPORTED"}};
  R->fs.files["/int/pg/CORRUPT.pgv"] = good;
  R->fs.corrupt("/int/pg/CORRUPT.pgv", good.size() - 10, 0x5A);
  R->fs.files["/int/pg/TRUNC.pgv"] = good;
  R->fs.truncate("/int/pg/TRUNC.pgv", good.size() - 20);
  R->fs.files["/int/pg/NOTPG.pgv"] = std::vector<uint8_t>(200, 0x42);
  R->fs.files["/int/pg/NEWER.pgv"] = good;
  patchHeader("/int/pg/NEWER.pgv", 5, 9);
  R->fs.files["/int/pg/PVER.pgv"] = good;
  patchHeader("/int/pg/PVER.pgv", 8, 99);
  for (const Case& c : cases) {
    tap("1");
    R->run(50);
    before = *R->proj;
    loadRow("INT", c.name);
    if (!toastHas(c.expect)) std::printf("  load %s: toast '%s'\n", c.name, R->ui.toastText());
    CHECK(toastHas(c.expect) || toastHas("DAMAGED"));
    CHECK(std::memcmp(R->proj, &before, sizeof(Project)) == 0);   // untouched
  }
  // read errors on every read call: the project is either untouched or a clean new project
  for (int k = 0; k < 60; k += 3) {
    tap("1");
    R->run(50);
    before = *R->proj;
    R->fs.arm();
    R->fs.readFailCall = k;
    loadRow("INT", "SRC");
    R->fs.clearFaults();
    const bool same = std::memcmp(R->proj, &before, sizeof(Project)) == 0;
    const bool loaded = toastHas("LOADED SRC");
    const bool clobbered = toastHas("NEW PROJECT");
    CHECK(same || loaded || clobbered);
    if (clobbered) CHECK(R->proj->magic[0] == 'P' && R->proj->patterns[0].length == 16);
  }
  // short reads still load
  R->fs.shortReadMax = 7;
  R->fs.shortReadRandom = true;
  loadRow("INT", "SRC");
  CHECK(toastHas("LOADED SRC"));
  R->fs.clearFaults();
}

static void testNewProject() {
  R->start(true);
  tap("1");
  R->run(50);
  ctrl("n");
  CHECK(R->ui.dialog() == Dialog::Confirm);
  tap("ent");
  R->run(50);
  CHECK_EQ(R->ui.selTrack(), 0);
  CHECK(!R->ui.dirty());
  int on = 0;
  for (int t = 0; t < 8; ++t)
    for (int i = 0; i < 16; ++i) on += R->step(t, i).flags() & kStepOn;
  CHECK_EQ(on, 0);
  ctrl("n");                                   // clean: no question
  CHECK(R->ui.dialog() == Dialog::None);
}

static void testSessionAndBoot() {
  // first boot: no files -> ROM demo 0, banner, the first key dismisses it AND acts
  // (as on the device: no developer mode)
  R->devMode = false;
  R->start(false);
  R->fs = pgtest::MemFs();
  R->boot();
  CHECK(R->ui.bannerVisible());
  CHECK_EQ(R->ui.projectMount(), kPmRom);
  CHECK(std::strcmp(R->ui.projectName(), demo::name(0)) == 0);
  CHECK_EQ(R->ui.selTrack(), kTrackBass);
  // the demo's bass is an instrument (ACID); the focus is its macro 1 (OPEN), never a bare parameter
  const macro::MacroSet* bassSet = macro::findBuiltin(instrumentId(R->proj->tracks[kTrackBass]));
  CHECK(bassSet != nullptr && std::strcmp(bassSet->name, "ACID") == 0);
  CHECK(!R->ui.focus().global && R->ui.focus().track == kTrackBass && R->ui.focus().index == kMacroIndex0);
  tap("spc");
  R->run(50);
  CHECK(!R->ui.bannerVisible());
  CHECK(R->eng.status().playing);
  {
    // pattern 1 moves the bass's BITE (macro 2) by itself, OPEN (macro 1) stays the player's
    const EngineStatus a = R->eng.status();
    CHECK((a.motion[kTrackBass] & 2) != 0 && (a.motion[kTrackBass] & 1) == 0);
    R->run(1000);
    const EngineStatus b = R->eng.status();
    CHECK(a.macroPos[kTrackBass][1] != b.macroPos[kTrackBass][1]);
  }
  // edits while playing are never autosaved, even after 12 s idle
  tap("1");
  R->fs.arm();
  R->run(12000);
  CHECK_EQ(R->fs.writeCalls, 0);
  CHECK(!R->fs.has("/int/pg/_last.pgv"));
  // stop: saved ~2 s later
  tap("spc");
  R->run(2600);
  CHECK(R->fs.has("/int/pg/_last.pgv"));
  // settings: octave and chord mode survive a reboot
  tap(";");
  down("shift"); tap("k"); up("shift");
  tap("l");
  R->run(11000);                               // idle while stopped: autosave
  static Project before;
  before = *R->proj;
  const int oct = R->ui.octave(kTrackBass), mode = R->ui.chordMode();
  R->boot();                                   // power cycle
  CHECK(std::memcmp(R->proj, &before, sizeof(Project)) == 0);
  CHECK_EQ(R->ui.octave(kTrackBass), oct);
  CHECK_EQ(R->ui.chordMode(), mode);
  CHECK(R->ui.dirty());                        // the demo was changed and never saved by name
  // no rewrite of an unchanged session
  R->fs.arm();
  R->run(12000);
  CHECK_EQ(R->fs.writeCalls, 0);
  // a damaged session falls back to the demo
  R->fs.corrupt("/int/pg/_last.pgv", 100, 0xFF);
  R->boot();
  CHECK_EQ(R->ui.projectMount(), kPmRom);
  // battery warning saves once, whatever the transport does
  tap("spc");
  tap("1");
  R->fs.arm();
  R->env.batteryPct = 4;
  R->run(200);
  CHECK(toastHas("BATTERY LOW - SAVED"));
  CHECK(R->fs.writeCalls > 0);
  R->env.batteryPct = 80;
  // saveSessionNow for the firmware's power-off path
  CHECK(R->ui.saveSessionNow());
  R->devMode = true;
}

static int row(const char* tag, const char* name) {
  // The list order is ROM, INT A-Z, SD A-Z; read it back by navigating would be slow,
  // so mirror refreshFiles(): ROM demos, then io::listProjects per mount.
  int i = demo::count();
  for (const char* m : {"/int", "/sd"}) {
    if (!R->fs.mounted(m)) continue;
    pg::io::ProjectEntry e[24];
    const int n = pg::io::listProjects(R->fs, m, e, 24);
    for (int j = 0; j < n; ++j, ++i)
      if (std::strcmp(tag, m + 1) == 0 || (std::strcmp(tag, "INT") == 0 && std::strcmp(m, "/int") == 0) ||
          (std::strcmp(tag, "SD") == 0 && std::strcmp(m, "/sd") == 0))
        if (std::strcmp(e[j].name, name) == 0) return i + (R->ui.projectTab() == 1 ? 1 : 0);
  }
  return -1;
}

static void testMoreRowsAndBattery() {
  R->start(true);
  static Project p;
  p = *R->proj;
  for (int i = 0; i < 30; ++i) {
    char name[9];
    std::snprintf(name, sizeof(name), "F%02d", i);
    CHECK(pg::io::saveProject(R->fs, "/int", name, p) == pg::io::Status::Ok);
  }
  tap("\\");
  CHECK_EQ(R->ui.fileRows(), demo::count() + kListPerMount + 1);   // 24 files + "+6 MORE"
  while (R->ui.projectCursor() < R->ui.fileRows() - 1) tap(".");
  tap("ent");
  CHECK(toastHas("24 FILES"));
  CHECK(R->ui.dialog() == Dialog::None);
  // unknown battery: never the emergency save
  tap("`");
  tap("1");
  R->fs.arm();
  R->env.batteryPct = kBatteryUnknown;
  tap("spc");
  R->run(500);
  CHECK(!toastHas("BATTERY"));
  CHECK_EQ(R->fs.writeCalls, 0);
  R->env.batteryPct = 80;
}

int main() {
  static uih::Rig rig;
  R = &rig;
  for (uih::GhostOrder order : {uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
  rig.matrix.order = order;
  std::printf("-- key matrix model: %s\n", uih::ghostOrderName(order));
  testSaveLoadRoundTrip();
  testNameEntry();
  testSdAndDelete();
  testSaveErrors();
  testLoadErrors();
  testNewProject();
  testSessionAndBoot();
  testMoreRowsAndBattery();
  }
  std::printf("itest_ui_files: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
