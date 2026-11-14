// The Daisy's audio callback path (App::render, what daisy/src/main.cpp's interrupt
// calls with libDaisy's float buffer): the engine's full-resolution float output
// (Instrument::processFloat) times the output level, clamped to +-1.0.
//   * the demo plays (autoplay, stage 0): sound, and never beyond +-1.0;
//   * no 16-bit step: most samples are not on the int16 grid (x * 32767 an integer);
//   * the level is a float multiply on that output: the same boot rendered at 0 dB
//     and at -20 dB gives B == A * g for every sample, and the reported peak follows;
//   * at +6 dB the hot samples clip at exactly 1.0 (what reaches libDaisy's f2s24 is
//     never out of range), and the report's peak is at most 32767.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "check.h"
#include "core/layout.h"
#include "core/nor_sim.h"

using namespace pgd;

namespace {

constexpr uint32_t kRate = 48000, kBlock = 96;
constexpr uint32_t kFlashBytes = 2 * layout::kFsBankBytes + layout::kPoolBytes;

struct Region final : pg::sampler::FlashDriver {
  pg::sampler::FlashDriver* base = nullptr;
  uint32_t off = 0;
  bool erase(uint32_t o, uint32_t n) override { return base->erase(off + o, n); }
  bool write(uint32_t o, const void* d, uint32_t n) override { return base->write(off + o, d, n); }
  bool read(uint32_t o, void* d, uint32_t n) override { return base->read(off + o, d, n); }
};

struct TestHal final : Hal {
  uint64_t us = 0;
  std::string out;
  uint32_t millis() override { return uint32_t(us / 1000); }
  uint32_t micros() override { return uint32_t(us); }
  uint32_t cycles() override { return uint32_t(us * 480); }
  uint32_t cpuHz() override { return 480000000u; }
  void serialWrite(const char* p, size_t n) override { out.append(p, n); }
  size_t serialRoom() override { return 1 << 20; }
  int serialRead() override { return -1; }
  void setLed(bool) override {}
  void restart(bool) override {}
  bool audioRunning() override { return true; }
  uint32_t sampleRate() override { return kRate; }
  uint32_t blockFrames() override { return kBlock; }
  const char* stageName() override { return "stage 0 (test)"; }
};

std::vector<uint8_t> g_flash;
NorSim* g_nor = nullptr;
Region g_fsRegion, g_poolRegion;
std::vector<uint8_t> g_fsArena(size_t(RamFs::kMaxFiles) * layout::kFsSlotBytes);
std::vector<uint8_t> g_fsStage(layout::kFsBankBytes);
std::vector<uint8_t> g_poolImage(layout::kPoolBytes);
std::vector<uint8_t> g_poolStage(4096);
std::vector<int16_t> g_lineRing(65536);
App g_app;
TestHal g_hal;

// A fresh device (erased flash, clock at 0), booted with the demo playing, at `db`;
// then `ms` of device time: per 2 ms block, the "interrupt" then a main-loop pass.
std::vector<float> runFresh(float db, uint32_t ms) {
  g_flash.assign(kFlashBytes, 0xFF);
  delete g_nor;
  g_nor = new NorSim(g_flash.data(), kFlashBytes);
  g_fsRegion.base = g_nor;
  g_poolRegion.base = g_nor;
  g_poolRegion.off = 2 * layout::kFsBankBytes;
  g_hal.us = 0;
  g_hal.out.clear();
  AppMemory mem;
  mem.fsArena = g_fsArena.data();
  mem.fsSlotBytes = layout::kFsSlotBytes;
  mem.fsStaging = g_fsStage.data();
  mem.fsBankBytes = layout::kFsBankBytes;
  mem.fsFlash = &g_fsRegion;
  mem.poolImage = g_poolImage.data();
  mem.poolStaging = g_poolStage.data();
  mem.poolFlash = &g_poolRegion;
  mem.lineRing = g_lineRing.data();
  mem.lineRingSamples = uint32_t(g_lineRing.size());
  AppConfig cfg;
  cfg.sampleRate = float(kRate);
  cfg.autoplay = true;
  cfg.version = "test";
  g_app.boot(g_hal, mem, cfg);
  g_app.setOutputGainDb(db);
  g_app.takeOutputPeak();
  std::vector<float> all;
  float blk[kBlock * 2];
  for (uint32_t t = 0; t < ms; t += 2) {
    g_app.render(blk, int(kBlock), nullptr);
    all.insert(all.end(), blk, blk + 2 * kBlock);
    g_hal.us += 2000;
    g_app.loopOnce();
  }
  return all;
}

}  // namespace

int main() {
  const uint32_t ms = 6000;
  const std::vector<float> a = runFresh(0.0f, ms);
  const uint32_t peakA = g_app.takeOutputPeak();
  long beyond = 0, offGrid = 0, loud = 0;
  float pkA = 0.0f;
  for (float x : a) {
    if (!(x >= -1.0f && x <= 1.0f)) ++beyond;
    const double q = double(x) * 32767.0;
    if (std::fabs(q - std::nearbyint(q)) > 0.01) ++offGrid;
    if (std::fabs(x) > 0.01f) ++loud;
    pkA = std::fabs(x) > pkA ? std::fabs(x) : pkA;
  }
  CHECK(beyond == 0);
  CHECK(loud > long(a.size()) / 10);                  // the demo plays
  CHECK(offGrid > long(a.size()) / 2);               // full resolution: not on the int16 grid
  CHECK(peakA > 0 && peakA <= 32767);
  CHECK(std::fabs(double(peakA) - double(pkA) * 32767.0) <= 1.0);
  std::printf("  0 dB: %zu samples, peak %.6f (report %u), %.1f %% off the int16 grid\n", a.size(), double(pkA),
              unsigned(peakA), 100.0 * double(offGrid) / double(a.size()));

  // The level: the same boot at -20 dB is the 0 dB render times g, sample for sample.
  const float g = powf(10.0f, -20.0f / 20.0f);
  const std::vector<float> b = runFresh(-20.0f, ms);
  const uint32_t peakB = g_app.takeOutputPeak();
  CHECK_EQ(long(b.size()), long(a.size()));
  long differ = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    if (b[i] != a[i] * g) ++differ;
  CHECK_EQ(differ, 0L);
  CHECK(std::fabs(double(peakB) - double(pkA * g) * 32767.0) <= 1.0);

  // +6 dB: the loudest samples clip at exactly 1.0, nothing beyond.
  const std::vector<float> c = runFresh(6.0f, ms);
  const uint32_t peakC = g_app.takeOutputPeak();
  const float g6 = powf(10.0f, 6.0f / 20.0f);
  long clipped = 0, wrong = 0;
  beyond = 0;
  for (size_t i = 0; i < c.size() && i < a.size(); ++i) {
    if (!(c[i] >= -1.0f && c[i] <= 1.0f)) ++beyond;
    const float want = a[i] * g6;
    const float w = want > 1.0f ? 1.0f : (want < -1.0f ? -1.0f : want);
    if (c[i] != w) ++wrong;
    if (std::fabs(c[i]) == 1.0f) ++clipped;
  }
  CHECK(beyond == 0);
  CHECK_EQ(wrong, 0L);
  CHECK(peakC <= 32767);
  std::printf("  +6 dB: peak report %u, %ld samples at the ceiling (the 0 dB peak x 2.0 = %.3f)\n", unsigned(peakC),
              clipped, double(pkA * g6));
  delete g_nor;
  return pgdt::finish("test_daisy_audio");
}
