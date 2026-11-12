// The desktop's projects directory (host/app/dirfs.h) against the device's
// file format: a project or session the desktop writes is byte-identical to
// what the device writes for the same project (same pg::io code), loads on
// the device's path, and the other way round; the Ui boots, autosaves and
// resumes on it like on the device.
//
//   test_desktop_files <scratch dir>
#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/memfs.h"
#include "dirfs.h"
#include "pg/demo/demo.h"
#include "pg/instrument/instrument.h"
#include "pg/io/project_io.h"
#include "pg/ui/gfx.h"
#include "pg/ui/ui.h"

namespace {
int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

using pg::io::Status;

pg::Instrument g_inst;
pg::Project g_a, g_b;
pg::ui::Ui g_ui;
pg::Framebuffer g_fb;

std::vector<uint8_t> readFile(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return v;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
  std::fclose(f);
  return v;
}
bool writeFile(const std::string& p, const std::vector<uint8_t>& v) {
  FILE* f = std::fopen(p.c_str(), "wb");
  if (!f) return false;
  std::fwrite(v.data(), 1, v.size(), f);
  return std::fclose(f) == 0;
}
void rmrf(const std::string& d) {
  if (DIR* dp = ::opendir(d.c_str())) {
    while (dirent* e = ::readdir(dp)) {
      if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
      const std::string p = d + "/" + e->d_name;
      if (::unlink(p.c_str()) != 0) rmrf(p);
    }
    ::closedir(dp);
  }
  ::rmdir(d.c_str());
}
bool same(const pg::Project& a, const pg::Project& b) { return std::memcmp(&a, &b, sizeof(pg::Project)) == 0; }

void demoProject(int i, pg::Project& out) {
  g_inst.init(44100.0f, false);
  pg::demo::build(g_inst.engine(), g_inst.project(), i);
  g_inst.engine().sanitizeProject(g_inst.project());
  out = g_inst.project();
}

void testPaths(const std::string& root) {
  desk::DirFs fs(root + "/p", "");
  CHECK(!fs.mounted("/int"));          // not yet created
  CHECK(fs.createMounts());
  CHECK(fs.mounted("/int"));
  CHECK(!fs.mounted("/sd"));           // no card unless --sd
  CHECK(fs.real("/int/pg/A.pgv") == root + "/p/A.pgv");
  CHECK(fs.real("/int/pg") == root + "/p");
  CHECK(fs.real("/int/other.cfg") == root + "/p/.int/other.cfg");
  CHECK(fs.real("/sd/pg/A.pgv").empty());
  CHECK(fs.real("/int/pg/../x").empty());
  CHECK(fs.real("/intx/pg/A").empty());
  CHECK(fs.real("relative").empty());
  CHECK(pg::io::saveProject(fs, pg::io::kMountSd, "NOCARD", g_a) == Status::NoMount);
  desk::DirFs withSd(root + "/p", root + "/card");
  CHECK(withSd.createMounts());
  CHECK(withSd.mounted("/sd"));
  CHECK(withSd.real("/sd/pg/B.pgv") == root + "/card/B.pgv");
}

// Desktop save == device save, byte for byte, and each loads on the other.
void testInterchange(const std::string& root) {
  const std::string dir = root + "/x";
  desk::DirFs fs(dir, root + "/xsd");
  CHECK(fs.createMounts());
  demoProject(1, g_a);
  CHECK(pg::io::saveProject(fs, pg::io::kMountInt, "DESKA", g_a) == Status::Ok);
  CHECK(pg::io::saveProject(fs, pg::io::kMountSd, "DESKSD", g_a) == Status::Ok);
  const std::vector<uint8_t> desk = readFile(dir + "/DESKA.pgv");
  CHECK(!desk.empty());
  CHECK(readFile(root + "/xsd/DESKSD.pgv").size() == desk.size());
  CHECK(::access((dir + "/_save.tmp").c_str(), F_OK) != 0);   // the temp was renamed into place

  pgtest::MemFs dev;   // the device's file system model
  CHECK(pg::io::saveProject(dev, pg::io::kMountInt, "DESKA", g_a) == Status::Ok);
  const std::vector<uint8_t>* devBytes = dev.get("/int/pg/DESKA.pgv");
  CHECK(devBytes && *devBytes == desk);

  // the desktop file on the device's load path
  pgtest::MemFs dev2;
  dev2.files["/int/pg/DESKA.pgv"] = desk;
  pg::io::HeaderInfo hi;
  CHECK(pg::io::loadProject(dev2, pg::io::kMountInt, "DESKA", g_b, &hi) == Status::Ok);
  CHECK(same(g_a, g_b));
  CHECK(hi.formatVersion == pg::io::kFormatVersion);
  CHECK(std::strcmp(hi.stem, "DESKA") == 0);

  // a device file (what tools/pg pull writes: the bytes, as NAME.pgv) on the desktop
  demoProject(2, g_a);
  pgtest::MemFs dev3;
  CHECK(pg::io::saveProject(dev3, pg::io::kMountInt, "FROMDEV", g_a) == Status::Ok);
  CHECK(writeFile(dir + "/FROMDEV.pgv", *dev3.get("/int/pg/FROMDEV.pgv")));
  CHECK(writeFile(dir + "/FROMDEV.pgv.part", {1, 2, 3}));   // an interrupted pull: not listed
  std::memset(&g_b, 0, sizeof(g_b));
  CHECK(pg::io::loadProject(fs, pg::io::kMountInt, "FROMDEV", g_b) == Status::Ok);
  CHECK(same(g_a, g_b));
  pg::io::ProjectEntry e[8];
  const int n = pg::io::listProjects(fs, pg::io::kMountInt, e, 8);
  CHECK(n == 2);
  CHECK(n == 2 && !std::strcmp(e[0].name, "DESKA") && !std::strcmp(e[1].name, "FROMDEV") && e[0].valid && e[1].valid);
}

// The Ui on the projects directory: first boot = ROM demo 0, as the device;
// the session the desktop saves at quit is the device's _last.pgv; the next
// boot resumes it.
void testSession(const std::string& root) {
  const std::string dir = root + "/s";
  {
    desk::DirFs fs(dir, "");
    CHECK(fs.createMounts());
    g_inst.init(44100.0f, false);
    g_ui.init(g_inst.engine(), g_inst.project(), &fs);
    g_ui.setGhostFilter(false);
    pg::ui::initPalette(g_fb);
    g_ui.boot(1000);
    CHECK(std::strcmp(g_ui.projectName(), pg::demo::name(0)) == 0);
    CHECK(g_ui.projectMount() == pg::ui::kPmRom);
    CHECK(!g_ui.ghostFilter().enabled());
    pg::ui::UiEnv env{1000, pg::ui::kBatteryUnknown, false, 0};
    g_ui.update(env, g_fb);
    demoProject(0, g_a);
    // the engine is not running here (no audio): change the project as a load would
    g_inst.engine().suspend();
    g_inst.project().globals[pg::kGTempo] = 131;
    g_inst.engine().resume();
    CHECK(g_ui.saveSessionNow());
    g_a = g_inst.project();
  }
  const std::vector<uint8_t> sess = readFile(dir + "/_last.pgv");
  CHECK(!sess.empty());
  pgtest::MemFs dev;
  dev.files[pg::io::kSessionPath] = sess;
  CHECK(pg::io::loadSession(dev, g_b) == Status::Ok);
  CHECK(same(g_a, g_b));
  CHECK(g_b.globals[pg::kGTempo] == 131);
  // the session is not a project in the list (as on the device)
  desk::DirFs fs(dir, "");
  pg::io::ProjectEntry e[4];
  CHECK(pg::io::listProjects(fs, pg::io::kMountInt, e, 4) == 0);
  // next launch resumes it
  g_inst.init(44100.0f, false);
  g_ui.init(g_inst.engine(), g_inst.project(), &fs);
  g_ui.boot(5000);
  CHECK(g_inst.project().globals[pg::kGTempo] == 131);
  CHECK(std::strcmp(g_ui.projectName(), pg::demo::name(0)) == 0);
  CHECK(g_ui.ghostFilter().enabled());   // a fresh init: back on

  // The ghost filter. a + s + d held: on the device f now reads as pressed (a
  // phantom), so the filter drops a real f too. A computer keyboard has no
  // phantoms: with the filter off (the desktop default) f arrives.
  const uint64_t asdf = (1ull << 30) | (1ull << 31) | (1ull << 32) | (1ull << 33);
  for (int pass = 0; pass < 2; ++pass) {
    const bool filterOn = pass == 0;
    g_ui.setGhostFilter(filterOn);
    uint32_t t = 6000 + uint32_t(pass) * 1000;
    for (int k = 30; k <= 33; ++k) g_ui.onKey(pg::KeyEvent{uint8_t(k), true, t * 1000u + uint32_t(k) * 20000u}, t + uint32_t(k));
    pg::ui::UiEnv env{t + 100, pg::ui::kBatteryUnknown, false, 0};
    g_ui.update(env, g_fb);
    CHECK(filterOn ? (g_ui.heldKeys() & asdf) == (asdf & ~(1ull << 33)) : (g_ui.heldKeys() & asdf) == asdf);
    for (int k = 30; k <= 33; ++k) g_ui.onKey(pg::KeyEvent{uint8_t(k), false, (t + 200) * 1000u}, t + 200);
    CHECK(g_ui.heldKeys() == 0);
  }
}
}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "build/desktop_test_files";
  rmrf(root);
  CHECK(desk::makeDirs(root));
  demoProject(0, g_a);
  testPaths(root);
  testInterchange(root);
  testSession(root);
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
