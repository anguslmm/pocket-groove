// Host tests for the Daisy's sample pool (daisy/core/pool_mirror.h): pg's
// SampleWriter and FlashPool, unchanged, over the SDRAM image with NOR rules; the
// flash copy written in the background, loaded at boot, and safe against a power
// cut at any flush operation; and the voice reading a slot through the pool.
#include <cstring>
#include <vector>

#include "check.h"
#include "core/nor_sim.h"
#include "core/pool_mirror.h"
#include "pg/sampler/flash_pool.h"
#include "pg/sampler/sample_format.h"
#include "pg/sampler/sample_pool.h"

using namespace pgd;
namespace ps = pg::sampler;

namespace {

std::vector<uint8_t> g_img(ps::kPoolBytes), g_img2(ps::kPoolBytes), g_flash(ps::kPoolBytes);
uint8_t g_stage[4096];

std::vector<int16_t> tone(uint32_t n, int seed) {
  std::vector<int16_t> v(n);
  for (uint32_t i = 0; i < n; ++i) v[i] = int16_t(((i * 37u + uint32_t(seed) * 101u) % 2000u) * 10 - 10000);
  return v;
}

bool putSample(ps::SampleWriter& w, int slot, const std::vector<int16_t>& s, const char* name) {
  const uint32_t bytes = uint32_t(s.size() * 2);
  if (w.begin(slot, uint32_t(s.size())) != ps::SampleWriter::kOk) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
  for (uint32_t off = 0; off < bytes; off += 128) {   // the transfer's chunks
    const uint32_t n = bytes - off < 128 ? bytes - off : 128;
    if (w.write(off, p + off, n) != ps::SampleWriter::kOk) return false;
  }
  return w.commit(ps::crc32(p, bytes), name) == ps::SampleWriter::kOk;
}

int flushAll(MirrorFlash& m, NorSim* nor = nullptr) {
  int n = 0;
  while (m.flushStep() && !(nor && nor->dead())) ++n;
  return n;
}

void testWriteFlushReload() {
  NorSim nor(g_flash.data(), uint32_t(g_flash.size()));
  nor.wipe();
  MirrorFlash m;
  m.init(g_img.data(), &nor, g_stage);
  CHECK(m.loadFrom());
  ps::FlashPool pool;
  pool.attach(m.image());
  CHECK_EQ(m.dropBadSlots(pool), 0);
  ps::SampleWriter w;
  w.attach(&m, &pool);
  CHECK(w.ready());
  const std::vector<int16_t> a = tone(30000, 1);
  CHECK(putSample(w, 2, a, "KICKY"));
  CHECK(pool.header(2) != nullptr);
  // The voice's view: the slot resolves to the RAM image.
  ps::SampleRef ref;
  CHECK(pool.user(2, &ref));
  CHECK_EQ(ref.length, 30000u);
  CHECK(std::memcmp(ref.data, a.data(), a.size() * 2) == 0);
  // Nothing on the flash yet; the dirty sectors cover the slot's header + 60000 bytes.
  CHECK_EQ(m.dirtySectors(), 1 + int((60000 + 4095) / 4096));
  const int flushed = flushAll(m);
  CHECK_EQ(flushed, 1 + int((60000 + 4095) / 4096));
  CHECK(!m.dirty());
  // Boot again from the flash copy.
  MirrorFlash m2;
  m2.init(g_img2.data(), &nor, g_stage);
  CHECK(m2.loadFrom());
  ps::FlashPool pool2;
  pool2.attach(m2.image());
  CHECK_EQ(m2.dropBadSlots(pool2), 0);
  CHECK(pool2.user(2, &ref) && ref.length == 30000u && std::memcmp(ref.data, a.data(), 60000) == 0);
  CHECK(std::memcmp(pool2.header(2)->name, "KICKY", 5) == 0);
  // Remove: every sector of the slot erased (in RAM at once, on the flash after the flushes).
  ps::SampleWriter w2;
  w2.attach(&m2, &pool2);
  CHECK(w2.remove(2) == ps::SampleWriter::kOk);
  CHECK(pool2.header(2) == nullptr);
  CHECK(w2.erased(2));
  CHECK_EQ(m2.dirtySectors(), int(ps::kSlotSectors));
  flushAll(m2);
  MirrorFlash m3;
  m3.init(g_img.data(), &nor, g_stage);
  m3.loadFrom();
  ps::FlashPool pool3;
  pool3.attach(m3.image());
  CHECK(pool3.header(2) == nullptr);
  // NOR rules: writing over unerased bytes fails, as on the device.
  const uint8_t z[4] = {0, 0, 0, 0}, f[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(m3.write(4096 * 5, z, 4));
  CHECK(!m3.write(4096 * 5, f, 4));
  CHECK_EQ(m3.stats().writeFailures, 1u);
}

void testPowerCutDuringFlush() {
  // Slot 0 holds A on the flash; B is written into RAM and the flush is cut at every
  // flash operation in turn. After the reboot slot 0 is A, B, or empty, never a
  // header with the other one's audio.
  const std::vector<int16_t> a = tone(20000, 1), b = tone(25000, 2);
  int cuts = 0, sawA = 0, sawB = 0, sawEmpty = 0;
  for (int64_t k = 0; k < 200; ++k) {
    NorSim nor(g_flash.data(), uint32_t(g_flash.size()));
    nor.wipe();
    MirrorFlash m;
    m.init(g_img.data(), &nor, g_stage);
    m.loadFrom();
    ps::FlashPool pool;
    pool.attach(m.image());
    ps::SampleWriter w;
    w.attach(&m, &pool);
    putSample(w, 0, a, "AAAA");
    flushAll(m);
    CHECK(w.remove(0) == ps::SampleWriter::kOk);
    CHECK(putSample(w, 0, b, "BBBB"));
    nor.cutAfter = int64_t(nor.ops()) + k;
    flushAll(m, &nor);
    const bool finished = !nor.dead();
    nor.revive();
    MirrorFlash m2;
    m2.init(g_img2.data(), &nor, g_stage);
    m2.loadFrom();
    ps::FlashPool p2;
    p2.attach(m2.image());
    m2.dropBadSlots(p2);
    ps::SampleRef ref;
    if (!p2.user(0, &ref)) {
      ++sawEmpty;
    } else if (ref.length == a.size() && std::memcmp(ref.data, a.data(), a.size() * 2) == 0) {
      ++sawA;
    } else if (ref.length == b.size() && std::memcmp(ref.data, b.data(), b.size() * 2) == 0) {
      ++sawB;
    } else {
      CHECK(!"slot 0 holds a header with the wrong audio");
    }
    if (finished) {
      CHECK(p2.user(0, &ref) && ref.length == b.size());
      break;
    }
    ++cuts;
  }
  CHECK(cuts > 10);
  CHECK(sawB >= 1);
  CHECK(sawA + sawEmpty >= 1);
  std::printf("pool power cuts: %d cut points: %d old sample, %d empty, %d new\n", cuts, sawA, sawEmpty, sawB);
}

void testRewrittenDuringFlush() {
  // A sector changed again after it was staged is flushed again.
  NorSim nor(g_flash.data(), uint32_t(g_flash.size()));
  nor.wipe();
  MirrorFlash m;
  m.init(g_img.data(), &nor, g_stage);
  m.loadFrom();
  const uint8_t x[2] = {0x12, 0x34};
  CHECK(m.erase(8192, 4096));
  CHECK(m.write(8192, x, 2));
  CHECK(m.flushStep());
  CHECK(!m.dirty());
  CHECK(m.write(8194, x, 2));   // erased bytes there: allowed
  CHECK(m.dirty());
  CHECK(m.flushStep());
  uint8_t back[4];
  CHECK(nor.read(8192, back, 4) && back[0] == 0x12 && back[3] == 0x34);
  m.hold(true);
  CHECK(m.write(8196, x, 2));
  CHECK(!m.flushStep());
  m.hold(false);
  CHECK(m.flushStep());
}

}  // namespace

int main() {
  testWriteFlushReload();
  testPowerCutDuringFlush();
  testRewrittenDuringFlush();
  return pgdt::finish("test_daisy_pool");
}
