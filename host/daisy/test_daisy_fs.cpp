// Host tests for /int on the Daisy: the RAM file system (daisy/core/ramfs.h) under
// pg::io's real save / load / list / recovery paths, the mount router, and the
// flash snapshots (daisy/core/fs_persist.h) over a NOR flash model, with a power
// cut at every flash operation of a save.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "core/fs_persist.h"
#include "core/nor_sim.h"
#include "core/ramfs.h"
#include "pg/demo/demo.h"
#include "pg/instrument/instrument.h"
#include "pg/io/project_io.h"

using namespace pgd;

namespace {

constexpr uint32_t kSlot = 96 * 1024, kBank = 1024 * 1024;
std::vector<uint8_t> g_arena(size_t(RamFs::kMaxFiles) * kSlot);
std::vector<uint8_t> g_stage(kBank);
std::vector<uint8_t> g_flash(2 * kBank, 0xFF);
pg::Instrument g_inst;

void collectNames(const char* name, size_t, void* u) { static_cast<std::vector<std::string>*>(u)->push_back(name); }

void testRamFsBasics() {
  RamFs fs;
  fs.init("/int", g_arena.data(), kSlot, 300000);
  CHECK(fs.mounted("/int"));
  CHECK(!fs.mounted("/sd"));
  const uint8_t a[5] = {1, 2, 3, 4, 5};
  CHECK(fs.write("/int/pg/a.bin", a, 5));
  CHECK(!fs.write("/sd/x", a, 5));         // not ours
  CHECK(!fs.write("/int/", a, 5));         // no name
  CHECK(fs.exists("/int/pg/a.bin"));
  uint8_t b[16];
  size_t n = 0;
  CHECK(fs.read("/int/pg/a.bin", b, sizeof(b), &n) && n == 5 && b[4] == 5);
  // open for write truncates at once; the data is the file's at close.
  const int h = fs.open("/int/pg/a.bin", true);
  CHECK(h >= 0);
  CHECK(!fs.read("/int/pg/a.bin", b, sizeof(b), &n));   // being written
  CHECK(fs.writeSome(h, a, 3));
  CHECK(fs.writeSome(h, a, 2));
  CHECK(fs.close(h));
  CHECK(fs.read("/int/pg/a.bin", b, sizeof(b), &n) && n == 5 && b[3] == 1);
  // streaming read
  const int r = fs.open("/int/pg/a.bin", false);
  CHECK(fs.readSome(r, b, 2) == 2 && fs.readSome(r, b, 10) == 3 && fs.readSome(r, b, 10) == 0);
  CHECK(fs.close(r));
  // rename replaces the target
  CHECK(fs.write("/int/pg/b.bin", a, 1));
  CHECK(fs.rename("/int/pg/a.bin", "/int/pg/b.bin"));
  CHECK(!fs.exists("/int/pg/a.bin"));
  CHECK(fs.read("/int/pg/b.bin", b, sizeof(b), &n) && n == 5);
  // list: files directly in the directory only
  CHECK(fs.write("/int/pg/sub/c.bin", a, 1));
  CHECK(fs.write("/int/top.bin", a, 1));
  std::vector<std::string> names;
  CHECK_EQ(fs.list("/int/pg", collectNames, &names), 1);
  CHECK(names.size() == 1 && names[0] == "b.bin");
  // a slot's limit and the budget
  std::vector<uint8_t> big(kSlot + 1, 7);
  CHECK(!fs.write("/int/big", big.data(), big.size()));
  const long before = fs.freeBytes("/int");
  CHECK(before > 0 && before < 300000);
  const int w = fs.open("/int/huge", true);
  bool ok = true;
  for (int i = 0; i < 40 && ok; ++i) ok = fs.writeSome(w, big.data(), 8192);
  CHECK(!ok);                      // the budget (300000) ran out before 40 x 8 KB
  CHECK(!fs.close(w));             // failed write: the file is left empty
  CHECK(fs.exists("/int/huge"));
  CHECK(fs.read("/int/huge", b, sizeof(b), &n) && n == 0);
  CHECK(fs.remove("/int/huge"));
  // the file table's limit
  RamFs t;
  t.init("/int", g_arena.data(), kSlot, kBank);
  int made = 0;
  for (int i = 0; i < 60; ++i) {
    char p[32];
    std::snprintf(p, sizeof(p), "/int/f%02d", i);
    made += t.write(p, a, 1) ? 1 : 0;
  }
  CHECK_EQ(made, RamFs::kMaxFiles);
  // serialize / deserialize round trip
  std::vector<uint8_t> img(fs.serializedSize());
  CHECK_EQ(fs.serialize(img.data(), uint32_t(img.size())), uint32_t(img.size()));
  RamFs g;
  std::vector<uint8_t> arena2(size_t(RamFs::kMaxFiles) * kSlot);
  g.init("/int", arena2.data(), kSlot, 300000);
  CHECK(g.deserialize(img.data(), uint32_t(img.size())));
  CHECK_EQ(g.fileCount(), fs.fileCount());
  CHECK(g.read("/int/pg/b.bin", b, sizeof(b), &n) && n == 5 && b[3] == 1);
  img[img.size() - 1] ^= 1;   // a damaged byte in data still parses (the CRC is the persistence's job)
  img.resize(img.size() - 1); // a truncated image does not
  CHECK(!g.deserialize(img.data(), uint32_t(img.size())));
  CHECK_EQ(g.fileCount(), 0);
}

void testProjectsThroughPgIo() {
  RamFs ram;
  ram.init("/int", g_arena.data(), kSlot, kBank - FsPersist::kHeaderSector);
  MountFs fs;
  fs.set(&ram, nullptr);
  CHECK(!fs.mounted("/sd"));
  g_inst.init(48000.0f, false);
  static pg::Project p, q;
  pg::demo::build(g_inst.engine(), p, 0);
  CHECK(pg::io::recoverAfterPowerLoss(fs, pg::io::kMountInt) == pg::io::Status::Ok);
  CHECK(pg::io::saveProject(fs, pg::io::kMountInt, "SONG1", p) == pg::io::Status::Ok);
  CHECK(pg::io::saveSession(fs, p) == pg::io::Status::Ok);
  CHECK(pg::io::loadProject(fs, pg::io::kMountInt, "SONG1", q) == pg::io::Status::Ok);
  CHECK(std::memcmp(&p, &q, sizeof(p)) == 0);
  pg::io::ProjectEntry e[8];
  CHECK_EQ(pg::io::listProjects(fs, pg::io::kMountInt, e, 8), 1);
  CHECK(pg::io::saveProject(fs, pg::io::kMountSd, "SONG1", p) != pg::io::Status::Ok);   // no card
  // ... and the set survives a snapshot to flash and a "reboot".
  NorSim nor(g_flash.data(), uint32_t(g_flash.size()));
  nor.wipe();
  FsPersist per;
  per.init(&ram, &nor, kBank, g_stage.data());
  CHECK(per.load() == FsPersist::Load::Empty);
  CHECK(pg::io::saveProject(fs, pg::io::kMountInt, "SONG2", p) == pg::io::Status::Ok);
  uint32_t t = 0;
  per.requestNow();
  int steps = 0;
  while (per.step(t) || per.dirty()) {
    t += 5;
    if (++steps > 10000) break;
  }
  CHECK_EQ(per.stats().saves, 1u);
  CHECK(!per.dirty());
  RamFs ram2;
  std::vector<uint8_t> arena2(size_t(RamFs::kMaxFiles) * kSlot);
  ram2.init("/int", arena2.data(), kSlot, kBank - FsPersist::kHeaderSector);
  FsPersist per2;
  per2.init(&ram2, &nor, kBank, g_stage.data());
  CHECK(per2.load() == FsPersist::Load::Loaded);
  MountFs fs2;
  fs2.set(&ram2, nullptr);
  CHECK(pg::io::loadProject(fs2, pg::io::kMountInt, "SONG2", q) == pg::io::Status::Ok);
  CHECK(std::memcmp(&p, &q, sizeof(p)) == 0);
  CHECK(pg::io::loadSession(fs2, q) == pg::io::Status::Ok);
}

// Saves until done; returns false if the flash died.
bool runSave(FsPersist& per, NorSim& nor, uint32_t& t) {
  for (int i = 0; i < 100000; ++i) {
    per.step(t);
    t += 5;
    if (nor.dead()) return false;
    if (!per.busy() && !per.dirty()) return true;
  }
  return false;
}

void testPowerCuts() {
  // Version A on flash; then a save of version B cut at operation k, for every k.
  const uint8_t va[3] = {'A', 'A', 'A'};
  std::vector<uint8_t> vb(70000, 'B');
  int cuts = 0, sawNew = 0, sawOld = 0;
  for (int64_t k = 0;; ++k) {
    NorSim nor(g_flash.data(), uint32_t(g_flash.size()));
    nor.wipe();
    RamFs ram;
    ram.init("/int", g_arena.data(), kSlot, kBank - FsPersist::kHeaderSector);
    FsPersist per;
    per.init(&ram, &nor, kBank, g_stage.data());
    per.load();
    uint32_t t = 0;
    ram.write("/int/pg/x", va, 3);
    per.requestNow();
    CHECK(runSave(per, nor, t));
    // Version B, with the power cut at the k-th flash operation from here.
    ram.write("/int/pg/x", vb.data(), uint32_t(vb.size()));
    per.requestNow();
    nor.cutAfter = int64_t(nor.ops()) + k;
    const bool finished = runSave(per, nor, t);
    nor.revive();
    // Reboot: whatever is found must be one of the two versions, whole.
    RamFs r2;
    std::vector<uint8_t> arena2(size_t(RamFs::kMaxFiles) * kSlot);
    r2.init("/int", arena2.data(), kSlot, kBank - FsPersist::kHeaderSector);
    FsPersist p2;
    p2.init(&r2, &nor, kBank, g_stage.data());
    const FsPersist::Load l = p2.load();
    CHECK(l == FsPersist::Load::Loaded);
    std::vector<uint8_t> got(80000);
    size_t n = 0;
    CHECK(r2.read("/int/pg/x", got.data(), got.size(), &n));
    const bool isA = n == 3 && std::memcmp(got.data(), va, 3) == 0;
    const bool isB = n == vb.size() && std::memcmp(got.data(), vb.data(), n) == 0;
    CHECK(isA || isB);
    sawOld += isA ? 1 : 0;
    sawNew += isB ? 1 : 0;
    if (finished) {
      CHECK(isB);
      break;
    }
    ++cuts;
    if (k > 1000) break;
  }
  CHECK(cuts > 5);       // erase + program chunks + header + verify reads
  CHECK(sawOld > 0);
  CHECK(sawNew > 0);
  std::printf("power cuts: %d cut points, %d found the old snapshot, %d the new\n", cuts, sawOld, sawNew);
}

void testQuietAndRetry() {
  NorSim nor(g_flash.data(), uint32_t(g_flash.size()));
  nor.wipe();
  RamFs ram;
  ram.init("/int", g_arena.data(), kSlot, kBank - FsPersist::kHeaderSector);
  FsPersist per;
  per.init(&ram, &nor, kBank, g_stage.data());
  per.load();
  const uint8_t a[1] = {1};
  ram.write("/int/a", a, 1);
  CHECK(!per.step(0));                          // the change is seen at 0
  CHECK(!per.step(FsPersist::kQuietMs - 1));   // still within the quiet time
  CHECK(per.step(FsPersist::kQuietMs));        // starts
  // A failing flash: the save fails, and it waits kRetryMs before trying again.
  nor.cutAfter = int64_t(nor.ops());
  uint32_t t = FsPersist::kQuietMs;
  for (int i = 0; i < 100; ++i) per.step(t += 5);
  CHECK_EQ(per.stats().failures, 1u);
  nor.revive();
  CHECK(!per.step(t + 10));
  t += FsPersist::kRetryMs + 10;
  for (int i = 0; i < 1000 && (per.step(t) || per.dirty()); ++i) t += 5;
  CHECK_EQ(per.stats().saves, 1u);
  CHECK(per.stats().bank == 0 || per.stats().bank == 1);
  // The next save goes to the other bank.
  const int bank = per.stats().bank;
  ram.write("/int/a", a, 1);
  per.requestNow();
  for (int i = 0; i < 1000 && (per.step(t) || per.dirty()); ++i) t += 5;
  CHECK_EQ(per.stats().saves, 2u);
  CHECK(per.stats().bank == 1 - bank);
  CHECK(per.stats().seq == 2u);
}

}  // namespace

int main() {
  testRamFsBasics();
  testProjectsThroughPgIo();
  testPowerCuts();
  testQuietAndRetry();
  return pgdt::finish("test_daisy_fs");
}
