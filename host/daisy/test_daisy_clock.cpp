// The firmware's clocks across their wraps (the owner's second report: every task's
// "max 4277071601us", because libDaisy's System::GetUs() is TIM2's 32-bit count / 240
// and so jumps back to 0 every 17.9 s; daisy/core/us_clock.h).
//   * UsClock (the device's micros()): from a 240 MHz 32-bit counter, right across the
//     counter's wraps, to the microsecond, and its own wrap at 2^32 us is a plain step;
//   * the App across the wraps of all three of its clocks: millis() (2^32 ms), micros()
//     (2^32 us) and cycles() (480 MHz, 8.9 s) all wrap during a 14 s run. The scheduler
//     keeps its rhythm (the UI frame every 33 ms, the stats line every 5 s), no task's or
//     pass's time reads as billions of microseconds, the load meter's figures stay
//     sane, a key tap from the computer whose 40 ms hold spans the millis() wrap is
//     released on time, and the report shows no wrapped value.
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "check.h"
#include "core/layout.h"
#include "core/nor_sim.h"
#include "core/us_clock.h"

using namespace pgd;

namespace {

void testUsClock() {
  UsClock c;
  const uint32_t perUs = 240;
  uint64_t ticks = 0xFFFFF000u;   // the counter just before its wrap
  c.start(uint32_t(ticks), perUs, 0);
  const uint64_t t0 = ticks;
  uint32_t seed = 12345;
  int bad = 0;
  for (int i = 0; i < 200000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    ticks += 1 + (seed >> 8) % 2000000u;   // steps up to ~8 ms, far below the 17.9 s period
    if (c.update(uint32_t(ticks)) != uint32_t((ticks - t0) / perUs)) ++bad;
  }
  CHECK_EQ(bad, 0);
  // 200000 steps of ~4 ms: ~800 s, ~45 wraps of the 240 MHz counter, all exact.
  CHECK((ticks - t0) / perUs > 700000000ull);
  // Its own wrap at 2^32 us: a difference across it is right.
  UsClock w;
  w.start(0, 1, 0xFFFFFF00u);
  const uint32_t a = w.update(0);
  const uint32_t b = w.update(1000);
  CHECK_EQ(uint32_t(b - a), 1000u);
  CHECK(b < a);   // it did wrap
}

constexpr uint32_t kRate = 48000, kBlock = 96;
constexpr uint32_t kFlashBytes = 2 * layout::kFsBankBytes + layout::kPoolBytes;

struct Region final : pg::sampler::FlashDriver {
  pg::sampler::FlashDriver* base = nullptr;
  uint32_t off = 0;
  bool erase(uint32_t o, uint32_t n) override { return base->erase(off + o, n); }
  bool write(uint32_t o, const void* d, uint32_t n) override { return base->write(off + o, d, n); }
  bool read(uint32_t o, void* d, uint32_t n) override { return base->read(off + o, d, n); }
};

// A fake clock that starts so that each of the three wraps within the run.
struct WrapHal final : Hal {
  uint64_t us = 0;                       // simulated time since the start
  uint32_t msOff = 0, usOff = 0;
  std::string out, in;
  size_t inPos = 0;
  uint32_t millis() override { return uint32_t(us / 1000) + msOff; }
  uint32_t micros() override { return uint32_t(us) + usOff; }
  uint32_t cycles() override { return uint32_t((us + usOff) * 480u); }
  uint32_t cpuHz() override { return 480000000u; }
  void serialWrite(const char* p, size_t n) override { out.append(p, n); }
  size_t serialRoom() override { return 1 << 20; }
  int serialRead() override { return inPos < in.size() ? static_cast<unsigned char>(in[inPos++]) : -1; }
  void setLed(bool) override {}
  void restart(bool) override {}
  bool audioRunning() override { return true; }
  uint32_t sampleRate() override { return kRate; }
  uint32_t blockFrames() override { return kBlock; }
  const char* stageName() override { return "stage 0 (test)"; }
};

std::vector<uint8_t> g_flash(kFlashBytes, 0xFF);
NorSim g_nor(g_flash.data(), kFlashBytes);
Region g_fsRegion, g_poolRegion;
std::vector<uint8_t> g_fsArena(size_t(RamFs::kMaxFiles) * layout::kFsSlotBytes);
std::vector<uint8_t> g_fsStage(layout::kFsBankBytes);
std::vector<uint8_t> g_poolImage(layout::kPoolBytes);
std::vector<uint8_t> g_poolStage(4096);
std::vector<int16_t> g_lineRing(65536);
App g_app;

int count(const std::string& s, const char* what) {
  int n = 0;
  for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
  return n;
}

void testAppAcrossTheWraps() {
  WrapHal hal;
  // millis() wraps 6 s in, micros() 3 s in; cycles() every 8.9 s (at about 5.9 s here).
  hal.msOff = uint32_t(0x100000000ull - 6000ull);
  hal.usOff = uint32_t(0x100000000ull - 3000000ull);
  g_fsRegion.base = &g_nor;
  g_poolRegion.base = &g_nor;
  g_poolRegion.off = 2 * layout::kFsBankBytes;
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
  cfg.autoplay = true;
  cfg.version = "test";
  g_app.boot(hal, mem, cfg);
  hal.out.clear();
  g_app.printReport();   // starts the report window
  float buf[2 * kBlock];
  const uint64_t blockUs = uint64_t(kBlock) * 1000000u / kRate;   // 2000
  uint64_t nextBlock = 0;
  bool tapped = false, stoppedSeen = false;
  for (hal.us = 0; hal.us < 14000000u; hal.us += 250) {
    if (hal.us >= nextBlock) {
      g_app.render(buf, int(kBlock), nullptr);
      nextBlock += blockUs;
    }
    // A key tap whose 40 ms hold spans the millis() wrap (6.000 s): space stops the demo.
    if (!tapped && hal.us >= 5980000u) {
      hal.in += ">tap spc 40\n";
      tapped = true;
    }
    g_app.loopOnce();
    if (tapped && !stoppedSeen && !g_app.instrument().engine().isPlaying()) stoppedSeen = true;
  }
  CHECK(stoppedSeen);
  // The stats line every 5 s through the wraps: at the start, 5 s and 10 s; no burst
  // and no stall at a wrap.
  CHECK_EQ(count(hal.out, "[stats]"), 3);
  CHECK_EQ(g_app.keyScript().held(), 0ull);   // the tap's up came (its wait ran across the wrap)
  CHECK(g_app.keyScript().idle());
  // The report: nothing wrapped. Every task's max and the pass's max are short.
  hal.out.clear();
  g_app.printReport();
  const std::string& rep = hal.out;
  CHECK(rep.find("4294") == std::string::npos && rep.find("4277") == std::string::npos);
  CHECK(rep.find("[task] ui ") != std::string::npos);
  // The UI task kept its 33 ms rhythm across the wraps: 14 s / 33 ms = 424 frames.
  {
    const size_t p = rep.find("[task] ui ");
    const size_t r = rep.find("runs", p);
    const long runs = std::strtol(rep.c_str() + r + 4, nullptr, 10);
    CHECK(runs >= 400 && runs <= 430);
    if (runs < 400 || runs > 430) std::printf("  ui runs %ld\n%s\n", runs, rep.c_str());
  }
  {
    const size_t p = rep.find("longest pass ");
    const long us = std::strtol(rep.c_str() + p + 13, nullptr, 10);
    CHECK(us >= 0 && us < 100000);
  }
  // The load meter: no late block (the fake clock gives every block exactly its period).
  CHECK(rep.find("late blocks: none since boot") != std::string::npos);
  CHECK(rep.find("late 0") != std::string::npos);
}

}  // namespace

int main() {
  testUsClock();
  testAppAcrossTheWraps();
  return pgdt::finish("test_daisy_clock");
}
