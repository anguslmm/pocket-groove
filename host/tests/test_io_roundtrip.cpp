// Round trips (byte exact), size targets, streaming chunk sizes, header-only
// listing, names, session/UI blobs, version handling, both filesystem fakes.
#include <unistd.h>

#include <string>

#include "common/hostfs.h"
#include "tests/test_io_util.h"

using namespace pg;
using namespace pg::io;
using namespace iot;
using pgtest::MemFs;

static void testCrc() {
  const char* s = "123456789";
  CHECK(crc32(0, reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
  // incremental == one-shot
  CHECK(crc32(crc32(0, reinterpret_cast<const uint8_t*>(s), 4), reinterpret_cast<const uint8_t*>(s) + 4, 5) == 0xCBF43926u);
}

static size_t roundTrip(const char* label, const Project& src, size_t sizeLimit) {
  MemFs fs;
  CHECK_ST(saveProject(fs, kMountInt, "SONG", src), Status::Ok);
  const auto* f = fs.get("/int/pg/SONG.pgv");
  CHECK(f != nullptr);
  CHECK(!fs.has("/int/pg/_save.tmp"));
  CHECK(fs.openHandles() == 0);
  CHECK(fs.maxOpenHandles == 1);
  const size_t sz = f ? f->size() : 0;
  std::printf("  %-8s %6zu bytes (%.1f%% of %zu)\n", label, sz, 100.0 * double(sz) / double(sizeof(Project)), sizeof(Project));
  if (sizeLimit) CHECK(sz < sizeLimit);
  auto dst = newProject();
  sentinel(*dst);
  HeaderInfo hi;
  CHECK_ST(loadProject(fs, kMountInt, "SONG", *dst, &hi), Status::Ok);
  CHECK(same(src, *dst));
  CHECK(hi.tempo == src.globals[kGTempo]);
  CHECK(std::strcmp(hi.stem, "SONG") == 0);
  CHECK(hi.fileSize() == sz);
  // in-memory codec agrees with the file
  auto buf = encode(src, "SONG");
  CHECK(f && buf == *f);
  sentinel(*dst);
  CHECK_ST(decodeFromBuffer(buf.data(), buf.size(), *dst), Status::Ok);
  CHECK(same(src, *dst));
  // streaming with tiny / odd chunk sizes and short reads
  const uint16_t chunks[] = {1, 3, 7, 13, 64};
  for (uint16_t wc : chunks)
    for (uint16_t rc : chunks) {
      if (wc != rc && wc != 1 && rc != 1 && !(wc == 13 && rc == 7)) continue;
      MemFs f2;
      IoOptions o;
      o.writeChunk = wc;
      o.readChunk = rc;
      CHECK_ST(saveProject(f2, kMountSd, "X", src, &o), Status::Ok);
      CHECK(f2.maxWriteLen <= wc);
      CHECK(f2.get("/sd/pg/X.pgv")->size() == buf.size());
      for (int mode = 0; mode < 3; ++mode) {
        f2.shortReadMax = mode == 0 ? 1 : 5;
        f2.shortReadRandom = mode == 2;
        sentinel(*dst);
        CHECK_ST(loadProject(f2, kMountSd, "X", *dst, nullptr, &o), Status::Ok);
        CHECK(same(src, *dst));
      }
    }
  return sz;
}

static void testRoundTrips() {
  std::printf("sizes:\n");
  auto p = newProject();
  initProject(*p);
  roundTrip("empty", *p, 1024);
  typicalProject(*p);
  roundTrip("typical", *p, 8192);
  for (uint32_t seed = 1; seed < 6; ++seed) {  // several typical songs must all meet the target
    typicalProject(*p, seed * 101);
    auto b = encode(*p);
    CHECK(!b.empty() && b.size() < 8192);
  }
  denseProject(*p);
  roundTrip("dense", *p, 0);
  randomProject(*p);
  const size_t worst = roundTrip("random", *p, 0);
  CHECK(worst <= size_t(kHeaderSize) + sizeof(Project) + sizeof(Project) / 128 + 16);
  // all-zero payload apart from magic: one huge overlapping match
  std::memset(p.get(), 0, sizeof(Project));
  std::memcpy(p->magic, "PGRV", 4);
  p->version = kProjectVersion;
  roundTrip("zeros", *p, 256);
  // all 0xFF and alternating patterns: long-length varints
  std::memset(p.get(), 0xFF, sizeof(Project));
  std::memcpy(p->magic, "PGRV", 4);
  p->version = kProjectVersion;
  roundTrip("ones", *p, 256);
}

static void testSaveRules() {
  MemFs fs;
  auto p = newProject();
  initProject(*p);
  CHECK_ST(saveProject(fs, kMountInt, "bad name", *p), Status::BadName);
  CHECK_ST(saveProject(fs, kMountInt, "TOOLONGNAME", *p), Status::BadName);
  CHECK_ST(saveProject(fs, "/usb", "OK", *p), Status::BadName);
  fs.sdPresent = false;
  CHECK_ST(saveProject(fs, kMountSd, "OK", *p), Status::NoMount);
  CHECK_ST(loadProject(fs, kMountSd, "OK", *p), Status::NoMount);
  CHECK(listProjects(fs, kMountSd, nullptr, 0) == -1);
  fs.sdPresent = true;
  CHECK_ST(loadProject(fs, kMountSd, "NOPE", *p), Status::NotFound);
  // bad project magic
  auto q = newProject();
  initProject(*q);
  q->magic[0] = 'X';
  CHECK_ST(saveProject(fs, kMountInt, "OK", *q), Status::BadProject);
  // space: freeBytes too small
  fs.spaceLimit[0] = 2000;
  CHECK_ST(saveProject(fs, kMountInt, "OK", *p), Status::NoSpace);
  CHECK(!fs.has("/int/pg/OK.pgv") && !fs.has("/int/pg/_save.tmp"));
  // space unknown: the write itself fails
  fs.reportFree = false;
  fs.spaceLimit[0] = 300;
  CHECK_ST(saveProject(fs, kMountInt, "OK", *p), Status::WriteFailed);
  CHECK(!fs.has("/int/pg/_save.tmp"));
  fs.reportFree = true;
  fs.spaceLimit[0] = -1;
  CHECK_ST(saveProject(fs, kMountInt, "OK", *p), Status::Ok);
  // SD pulled during the save
  {
    // card pulled after the open: the mount vanishes at the first write
    struct PullFs : MemFs {
      bool writeSome(int h, const uint8_t* b, size_t n) override { sdPresent = false; return MemFs::writeSome(h, b, n); }
    } pf;
    CHECK_ST(saveProject(pf, kMountSd, "OK", *p), Status::NoMount);
  }
  fs.clearFaults();
  // stale temp from an earlier crash is replaced
  fs.files["/int/pg/_save.tmp"] = {1, 2, 3};
  CHECK_ST(saveProject(fs, kMountInt, "OK2", *p), Status::Ok);
  CHECK(!fs.has("/int/pg/_save.tmp"));
  // delete
  CHECK_ST(deleteProject(fs, kMountInt, "OK2"), Status::Ok);
  CHECK_ST(deleteProject(fs, kMountInt, "OK2"), Status::NotFound);
  CHECK(!projectExists(fs, kMountInt, "OK2"));
  CHECK(projectExists(fs, kMountInt, "OK"));
}

static void testVersions() {
  auto p = newProject();
  typicalProject(*p);
  auto f = encode(*p);
  auto d = newProject();
  // compatVersion newer -> NewerFormat (fix header CRC so only the version is wrong)
  auto fixCrc = [](std::vector<uint8_t>& v) {
    const uint32_t hs = uint32_t(v[6]) | (uint32_t(v[7]) << 8);
    const uint32_t c = crc32(0, v.data(), hs - 4);
    for (int i = 0; i < 4; ++i) v[hs - 4 + i] = uint8_t(c >> (8 * i));
  };
  auto g = f;
  g[5] = kFormatVersion + 1;
  fixCrc(g);
  sentinel(*d);
  CHECK_ST(decodeFromBuffer(g.data(), g.size(), *d), Status::NewerFormat);
  CHECK(isSentinel(*d));
  // a newer writer with formatVersion 2 but compat 1 and 16 extra header bytes is still readable
  g = f;
  g[4] = 2;
  g.insert(g.begin() + 60, 16, uint8_t(0x5A));
  g[6] = 80;
  fixCrc(g);
  sentinel(*d);
  CHECK_ST(decodeFromBuffer(g.data(), g.size(), *d), Status::Ok);
  CHECK(same(*p, *d));
  // unsupported Project version
  g = f;
  g[8] = kProjectVersion + 1;
  fixCrc(g);
  CHECK_ST(decodeFromBuffer(g.data(), g.size(), *d), Status::ProjectVersion);
  // a version-1 project (before instruments) is still read, byte for byte (the engine's
  // sanitiser then makes it a version-2 project with no instrument: itest_golden)
  {
    typicalProject(*p);
    p->version = 1;
    MemFs v1;
    CHECK_ST(saveProject(v1, kMountInt, "OLD", *p), Status::Ok);
    sentinel(*d);
    HeaderInfo hv{};
    CHECK_ST(loadProject(v1, kMountInt, "OLD", *d, &hv), Status::Ok);
    CHECK(hv.projectVersion == 1);
    CHECK(same(*p, *d));
    p->version = 0;
    CHECK_ST(saveProject(v1, kMountInt, "ZERO", *p), Status::BadProject);
    p->version = kProjectVersion;
  }
  // raw Project file (old format of UX.md 13.2) is not mistaken for a valid file
  std::vector<uint8_t> raw(reinterpret_cast<const uint8_t*>(p.get()), reinterpret_cast<const uint8_t*>(p.get()) + sizeof(Project));
  CHECK_ST(decodeFromBuffer(raw.data(), raw.size(), *d), Status::NotPgFile);
}

static void testNames() {
  CHECK(isValidName("ACIDJAM"));
  CHECK(isValidName("A"));
  CHECK(isValidName("GROOVE01"));
  CHECK(isValidName("_X-1"));
  CHECK(!isValidName(""));
  CHECK(!isValidName("acidjam"));
  CHECK(!isValidName("ACID JAM"));
  CHECK(!isValidName("ACIDJAM12"));
  CHECK(!isValidName("A.B"));
  CHECK(!isValidName("A/B"));
  CHECK(!isValidName(nullptr));
  char out[kNameBuf];
  CHECK(normalizeName("acid jam!", out) && std::strcmp(out, "ACIDJAM") == 0);
  CHECK(normalizeName("bass-house_mix", out) && std::strcmp(out, "BASS-HOU") == 0);
  CHECK(!normalizeName("  !!", out) && out[0] == 0);
  CHECK(!normalizeName(nullptr, out));
  char path[kPathMax];
  CHECK(buildProjectPath(kMountInt, "ACIDJAM", path, sizeof(path)) && std::strcmp(path, "/int/pg/ACIDJAM.pgv") == 0);
  CHECK(buildProjectPath(kMountSd, "X", path, sizeof(path)) && std::strcmp(path, "/sd/pg/X.pgv") == 0);
  CHECK(!buildProjectPath(kMountSd, "X", path, 10));
  CHECK(!buildProjectPath("/foo", "X", path, sizeof(path)));
  MemFs fs;
  auto p = newProject();
  initProject(*p);
  char n[kNameBuf];
  CHECK(nextFreeName(fs, n) && std::strcmp(n, "GROOVE01") == 0);
  saveProject(fs, kMountInt, "GROOVE01", *p);
  saveProject(fs, kMountSd, "GROOVE02", *p);
  CHECK(nextFreeName(fs, n) && std::strcmp(n, "GROOVE03") == 0);
  fs.sdPresent = false;
  CHECK(nextFreeName(fs, n) && std::strcmp(n, "GROOVE02") == 0);
}

static void testListing() {
  MemFs fs;
  auto p = newProject();
  initProject(*p);
  const char* names[] = {"ZED", "ACID", "MID", "B-2"};
  const uint8_t bpm[] = {130, 124, 99, 200};
  for (int i = 0; i < 4; ++i) {
    p->globals[kGTempo] = bpm[i];
    CHECK_ST(saveProject(fs, kMountInt, names[i], *p), Status::Ok);
  }
  saveSession(fs, *p);
  fs.files["/int/pg/README.txt"] = {1, 2};
  fs.files["/int/pg/lower.pgv"] = {1, 2};
  fs.files["/int/pg/JUNK.pgv"] = std::vector<uint8_t>(100, 7);
  fs.files["/int/other/X.pgv"] = std::vector<uint8_t>(100, 7);
  fs.files["/int/pg/sub/Y.pgv"] = std::vector<uint8_t>(100, 7);
  // make the bodies unreadable: listing must not need them
  for (const char* n : {"/int/pg/ACID.pgv", "/int/pg/ZED.pgv"}) fs.truncate(n, kHeaderSize);
  ProjectEntry e[8];
  fs.arm();
  const int n = listProjects(fs, kMountInt, e, 8);
  CHECK(n == 5);
  const char* want[] = {"ACID", "B-2", "JUNK", "MID", "ZED"};
  for (int i = 0; i < n && i < 5; ++i) CHECK(std::strcmp(e[i].name, want[i]) == 0);
  CHECK(e[0].valid && e[0].tempo == 124);
  CHECK(e[1].valid && e[1].tempo == 200);
  CHECK(!e[2].valid && e[2].status == Status::NotPgFile);
  CHECK(e[3].valid && e[3].tempo == 99);
  CHECK(e[4].valid && e[4].tempo == 130 && e[4].size == uint32_t(kHeaderSize));
  CHECK(fs.readCalls <= 2 * 5 + 2);  // 64-byte header reads only
  CHECK(listProjects(fs, kMountInt, e, 2) == 2);
  CHECK(listProjects(fs, kMountSd, e, 8) == 0);
}

static void testSessionAndUi() {
  MemFs fs;
  auto p = newProject();
  auto d = newProject();
  typicalProject(*p);
  CHECK_ST(loadSession(fs, *d), Status::NotFound);
  CHECK_ST(saveSession(fs, *p), Status::Ok);
  CHECK(fs.has(kSessionPath));
  sentinel(*d);
  CHECK_ST(loadSession(fs, *d), Status::Ok);
  CHECK(same(*p, *d));
  HeaderInfo h;
  CHECK_ST(readHeader(fs, kSessionPath, &h), Status::Ok);
  CHECK(std::strcmp(h.stem, "_last") == 0);

  uint8_t data[8] = {3, 1, 2, 9, 9, 9, 9, 9};
  uint8_t buf[16];
  uint8_t ver = 0;
  size_t len = 0;
  CHECK_ST(loadUiSettings(fs, &ver, buf, sizeof(buf), &len), Status::NotFound);
  CHECK_ST(saveUiSettings(fs, 2, data, 8), Status::Ok);
  CHECK_ST(loadUiSettings(fs, &ver, buf, sizeof(buf), &len), Status::Ok);
  CHECK(ver == 2 && len == 8 && std::memcmp(buf, data, 8) == 0);
  CHECK_ST(loadUiSettings(fs, &ver, buf, 4, &len), Status::BufferTooSmall);
  CHECK_ST(saveUiSettings(fs, 3, data, 3), Status::Ok);  // overwrite
  CHECK_ST(loadUiSettings(fs, &ver, buf, sizeof(buf), &len), Status::Ok);
  CHECK(ver == 3 && len == 3);
  CHECK(!fs.has("/int/pg/_ui.tmp"));
  auto& f = fs.files[kUiSettingsPath];
  for (size_t i = 0; i < f.size(); ++i) {
    f[i] ^= 0x10;
    CHECK(loadUiSettings(fs, &ver, buf, sizeof(buf), &len) != Status::Ok);
    f[i] ^= 0x10;
  }
  for (size_t l = 0; l < f.size(); ++l) {
    auto copy = f;
    fs.truncate(kUiSettingsPath, l);
    CHECK(loadUiSettings(fs, &ver, buf, sizeof(buf), &len) != Status::Ok);
    f = copy;
  }
  f.push_back(0);
  CHECK_ST(loadUiSettings(fs, &ver, buf, sizeof(buf), &len), Status::Corrupt);
  f.pop_back();
  // UI save fault sweep: old blob or new blob, never broken
  for (int k = 0; k < 12; ++k) {
    fs.arm();
    fs.failOp = k;
    saveUiSettings(fs, 4, data, 5);
    fs.clearFaults();
    recoverAfterPowerLoss(fs, kMountInt);
    CHECK_ST(loadUiSettings(fs, &ver, buf, sizeof(buf), &len), Status::Ok);
    CHECK((ver == 3 && len == 3) || (ver == 4 && len == 5));
    if (ver == 4) saveUiSettings(fs, 3, data, 3);
  }
}

template <class Fs>
static void testRenameOver(Fs& fs, const char* label) {
  auto a = newProject();
  auto b = newProject();
  auto d = newProject();
  typicalProject(*a, 1);
  typicalProject(*b, 2);
  CHECK_ST(saveProject(fs, kMountInt, "OVER", *a), Status::Ok);
  CHECK_ST(saveProject(fs, kMountInt, "OVER", *b), Status::Ok);
  sentinel(*d);
  CHECK_ST(loadProject(fs, kMountInt, "OVER", *d), Status::Ok);
  CHECK(same(*b, *d));
  // rename() itself replaces an existing target
  CHECK(fs.write("/int/pg/R1.bin", reinterpret_cast<const uint8_t*>("new"), 3));
  CHECK(fs.write("/int/pg/R2.bin", reinterpret_cast<const uint8_t*>("oldold"), 6));
  CHECK(fs.rename("/int/pg/R1.bin", "/int/pg/R2.bin"));
  uint8_t buf[16];
  size_t n = 0;
  CHECK(fs.read("/int/pg/R2.bin", buf, sizeof(buf), &n) && n == 3 && std::memcmp(buf, "new", 3) == 0);
  CHECK(!fs.exists("/int/pg/R1.bin"));
  CHECK(fs.read("/int/pg/OVER.pgv", buf, 16, &n) && n == 16);  // partial read with cap < size
  // SD round trip too
  CHECK_ST(saveProject(fs, kMountSd, "SDONE", *a), Status::Ok);
  ProjectEntry e[4];
  CHECK(listProjects(fs, kMountSd, e, 4) == 1 && e[0].valid && std::strcmp(e[0].name, "SDONE") == 0);
  std::printf("  rename-over on %s ok\n", label);
}

static void testHostFs() {
  char tmpl[] = "/tmp/pgio_XXXXXX";
  const char* base = getenv("TMPDIR");
  std::string root;
  if (base && *base) {
    std::string t = std::string(base) + "/pgio_XXXXXX";
    std::vector<char> v(t.begin(), t.end());
    v.push_back(0);
    if (mkdtemp(v.data())) root = v.data();
  }
  if (root.empty() && mkdtemp(tmpl)) root = tmpl;
  CHECK(!root.empty());
  if (root.empty()) return;
  pgtest::HostFs fs(root);
  CHECK(!fs.mounted(kMountInt));
  auto p = newProject();
  initProject(*p);
  CHECK_ST(saveProject(fs, kMountInt, "X", *p), Status::NoMount);
  CHECK(fs.createMounts(true));
  CHECK(fs.freeBytes(kMountInt) > 0);
  testRenameOver(fs, "hostfs");
  CHECK_ST(recoverAfterPowerLoss(fs, kMountInt), Status::Ok);
  std::string cmd = "rm -rf '" + root + "'";
  CHECK(std::system(cmd.c_str()) == 0);
}

int main() {
  testCrc();
  testRoundTrips();
  testSaveRules();
  testVersions();
  testNames();
  testListing();
  testSessionAndUi();
  MemFs mfs;
  testRenameOver(mfs, "memfs");
  MemFs nfs;
  nfs.renameAtomic = false;
  testRenameOver(nfs, "memfs (non-atomic rename)");
  testHostFs();
  return finish("test_io_roundtrip");
}
