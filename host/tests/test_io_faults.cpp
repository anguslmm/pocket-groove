// Fault injection: every mutating op of a save fails / loses power; every
// read of a load fails; the file changes between load passes; the Project is
// edited while it is being saved.
#include "tests/test_io_util.h"

using namespace pg;
using namespace pg::io;
using namespace iot;
using pgtest::MemFs;

static const char* kTarget = "/int/pg/SONG.pgv";
static const char* kTmp = "/int/pg/_save.tmp";

// Loads the target; returns 0 = absent, 1 = equals a, 2 = equals b, -1 = garbage.
static int classify(MemFs& fs, const Project& a, const Project& b) {
  if (!fs.has(kTarget)) return 0;
  auto d = newProject();
  sentinel(*d);
  const Status s = loadFile(fs, kTarget, *d);
  if (s != Status::Ok) return -1;
  if (same(*d, a)) return 1;
  if (same(*d, b)) return 2;
  return -1;
}

static void sweepSave(bool oldExists, bool powerLoss, MemFs::LossMode mode, bool atomic, uint16_t chunk, int& points) {
  auto a = newProject();
  auto b = newProject();
  typicalProject(*a, 1);
  typicalProject(*b, 2);
  IoOptions o;
  o.writeChunk = chunk;
  MemFs base;
  base.renameAtomic = atomic;
  base.lossMode = mode;
  if (oldExists) CHECK_ST(saveFile(base, kTarget, "SONG", *a), Status::Ok);
  base.files["/int/pg/OTHER.pgv"] = base.files.count(kTarget) ? base.files[kTarget] : std::vector<uint8_t>{1};
  const auto other = base.files["/int/pg/OTHER.pgv"];

  MemFs probe = base;
  probe.arm();
  CHECK_ST(saveFile(probe, kTarget, "SONG", *b, &o), Status::Ok);
  const int total = probe.ops;
  CHECK(total >= 4);
  for (int k = 0; k <= total; ++k) {
    MemFs fs = base;
    fs.arm();
    if (powerLoss) fs.powerLossOp = k;
    else fs.failOp = k;
    const Status s = saveFile(fs, kTarget, "SONG", *b, &o);
    if (k == total) CHECK_ST(s, Status::Ok);
    const bool tmpLeft = fs.has(kTmp);
    if (powerLoss) fs.powerCycle();
    else fs.clearFaults();
    CHECK_ST(recoverAfterPowerLoss(fs, kMountInt), Status::Ok);  // as at the next boot
    const int c = classify(fs, *a, *b);
    if (s == Status::Ok) {
      CHECK(c == 2);
      CHECK(!tmpLeft);
    } else if (!powerLoss) {
      // A reported failure leaves the old target, or (non-atomic rename that
      // removed the target, then failed twice) the verified new file in the
      // temp, which boot recovery puts in place.
      if (tmpLeft) CHECK(!atomic && c == 2);
      else CHECK(c == (oldExists ? 1 : 0));
    } else {
      CHECK(c == 2 || c == (oldExists ? 1 : 0));
    }
    if (c < 0) std::printf("    garbage at op %d (old=%d loss=%d mode=%d atomic=%d chunk=%d) status %s\n", k, oldExists,
                           powerLoss, int(mode), atomic, chunk, statusName(s));
    CHECK(!fs.has(kTmp));
    CHECK(fs.files["/int/pg/OTHER.pgv"] == other);
    CHECK(fs.openHandles() == 0 || powerLoss);
    ++points;
  }
}

static void testSaveFaults() {
  int points = 0;
  const MemFs::LossMode modes[] = {MemFs::LossMode::Discard, MemFs::LossMode::KeepPrefix, MemFs::LossMode::KeepPrefixGarbage};
  for (int old = 0; old < 2; ++old)
    for (int atomic = 0; atomic < 2; ++atomic)
      for (uint16_t chunk : {uint16_t(64), uint16_t(13)}) {
        sweepSave(old, false, MemFs::LossMode::Discard, atomic, chunk, points);
        for (auto m : modes) sweepSave(old, true, m, atomic, chunk, points);
      }
  std::printf("save fault points checked: %d\n", points);
}

static void testLoadReadFaults() {
  auto a = newProject();
  typicalProject(*a);
  MemFs fs;
  CHECK_ST(saveFile(fs, kTarget, "SONG", *a), Status::Ok);
  for (int sr = 0; sr < 2; ++sr) {
    MemFs probe = fs;
    probe.shortReadMax = sr ? 7 : 0;
    probe.arm();
    auto d = newProject();
    CHECK_ST(loadFile(probe, kTarget, *d), Status::Ok);
    const int reads = probe.readCalls;
    int clobbered = 0, untouched = 0;
    for (int k = 0; k < reads; ++k) {
      MemFs f = fs;
      f.shortReadMax = sr ? 7 : 0;
      f.arm();
      f.readFailCall = k;
      sentinel(*d);
      const Status s = loadFile(f, kTarget, *d);
      CHECK(s != Status::Ok);
      if (s == Status::DestClobbered) { CHECK(allZero(*d)); ++clobbered; }
      else { CHECK_ST(s, Status::ReadFailed); CHECK(isSentinel(*d)); ++untouched; }
      if (k < reads / 2) CHECK(isSentinel(*d));  // pass 1 never touches the destination
      CHECK(f.openHandles() == 0);
    }
    CHECK(clobbered > 0 && untouched > 0);
  }
}

// The file is replaced between pass 1 and pass 2 (e.g. another save, or SD swap).
struct SwapFs : MemFs {
  int opens = 0;
  std::vector<uint8_t> replacement;
  int open(const char* path, bool w) override {
    if (!w && ++opens == 2) files[path] = replacement;
    return MemFs::open(path, w);
  }
};

static void testChangedBetweenPasses() {
  auto a = newProject();
  auto b = newProject();
  typicalProject(*a, 1);
  typicalProject(*b, 2);
  auto fa = encode(*a, "SONG");
  auto fb = encode(*b, "SONG");
  auto d = newProject();
  {  // different valid file: header mismatch is caught before dest is touched
    SwapFs fs;
    fs.files[kTarget] = fa;
    fs.replacement = fb;
    sentinel(*d);
    CHECK_ST(loadFile(fs, kTarget, *d), Status::Corrupt);
    CHECK(isSentinel(*d));
  }
  {  // same header, body corrupted after pass 1: dest clobbered (zeroed)
    SwapFs fs;
    fs.files[kTarget] = fa;
    fs.replacement = fa;
    fs.replacement[fa.size() / 2] ^= 0x40;
    sentinel(*d);
    CHECK_ST(loadFile(fs, kTarget, *d), Status::DestClobbered);
    CHECK(allZero(*d));
  }
  {  // truncated after pass 1
    SwapFs fs;
    fs.files[kTarget] = fa;
    fs.replacement = fa;
    fs.replacement.resize(fa.size() - 10);
    sentinel(*d);
    CHECK_ST(loadFile(fs, kTarget, *d), Status::DestClobbered);
    CHECK(allZero(*d));
  }
}

// The "engine" edits the Project while the saver streams it out.
struct EditingFs : MemFs {
  Project* p = nullptr;
  int editsLeft = 0;
  Rng r{99};
  bool writeSome(int h, const uint8_t* b, size_t n) override {
    if (editsLeft > 0) {
      --editsLeft;
      p->patterns[r.below(8)].tracks[r.below(8)].steps[r.below(16)].vel = uint8_t(1 + r.below(127));
    }
    return MemFs::writeSome(h, b, n);
  }
};

static void testRacingEdits() {
  for (int edits : {1, 3, 10, 40, 100000}) {
    auto p = newProject();
    typicalProject(*p);
    EditingFs fs;
    fs.p = p.get();
    fs.editsLeft = edits;
    const Status s = saveFile(fs, kTarget, "SONG", *p);
    if (edits >= 100000) {
      CHECK_ST(s, Status::Busy);
      CHECK(!fs.has(kTarget) && !fs.has(kTmp));
      continue;
    }
    if (s == Status::Busy) { CHECK(!fs.has(kTarget)); continue; }
    CHECK_ST(s, Status::Ok);
    auto d = newProject();
    CHECK_ST(loadFile(fs, kTarget, *d), Status::Ok);
    CHECK(same(*d, *p));  // the file holds the final state, not a mix that fails its CRC
  }
}

// rename() that removes the target and then fails, every time (worst FAT case).
struct BrokenRenameFs : MemFs {
  bool broken = true;
  bool rename(const char* from, const char* to) override {
    if (!broken) return MemFs::rename(from, to);
    files.erase(to);
    return false;
  }
};

static void testBrokenRename() {
  auto a = newProject(), b = newProject(), d = newProject();
  typicalProject(*a, 1);
  typicalProject(*b, 2);
  BrokenRenameFs fs;
  fs.broken = false;
  CHECK_ST(saveFile(fs, kTarget, "SONG", *a), Status::Ok);
  fs.broken = true;
  CHECK_ST(saveFile(fs, kTarget, "SONG", *b), Status::RenameFailed);
  CHECK(fs.has(kTmp) && !fs.has(kTarget));  // the verified new file survives in the temp
  fs.broken = false;
  CHECK_ST(recoverAfterPowerLoss(fs, kMountInt), Status::Ok);
  CHECK(!fs.has(kTmp));
  CHECK_ST(loadFile(fs, kTarget, *d), Status::Ok);
  CHECK(same(*d, *b));
}

int main() {
  testBrokenRename();
  testSaveFaults();
  testLoadReadFaults();
  testChangedBetweenPasses();
  testRacingEdits();
  return finish("test_io_faults");
}
