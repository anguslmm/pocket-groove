// Keys from the computer in bursts (the owner's second report: "[keys] from the
// computer 135 (script queue 0, dropped 30)" from the mirror window).
//   * a burst far larger than the key queue, arriving at once (a stalled main loop, a
//     client sending ahead): nothing is dropped; the App leaves the rest in the receive
//     ring and takes it as the queue drains, in order;
//   * auto-repeat (down down down ... up, and release + press pairs): one press, one
//     release reaches the UI per held key;
//   * a script's waits in the burst still play with their timing.
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

struct KeyHal final : Hal {
  uint64_t us = 0;
  std::string out, in;
  size_t inPos = 0;
  uint32_t millis() override { return uint32_t(us / 1000); }
  uint32_t micros() override { return uint32_t(us); }
  uint32_t cycles() override { return uint32_t(us * 480u); }
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
KeyHal g_hal;

void boot() {
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
  cfg.version = "test";
  g_app.boot(g_hal, mem, cfg);
}

// Runs the App for ms milliseconds of fake time (a block every 2 ms, a pass every 0.25 ms).
void run(uint32_t ms) {
  float buf[2 * kBlock];
  const uint64_t end = g_hal.us + uint64_t(ms) * 1000u;
  for (; g_hal.us < end; g_hal.us += 250) {
    if (g_hal.us % 2000 == 0) g_app.render(buf, int(kBlock), nullptr);
    g_app.loopOnce();
  }
}

std::string reportLine(const char* tag) {
  g_hal.out.clear();
  g_app.printReport();
  const size_t p = g_hal.out.find(tag);
  if (p == std::string::npos) return "";
  return g_hal.out.substr(p, g_hal.out.find('\n', p) - p);
}

void testBurst() {
  boot();
  run(500);
  // 1000 key events in one burst (500 presses of "q", each down then up), 4x the queue.
  for (int i = 0; i < 500; ++i) g_hal.in += ">down q\n>up q\n";
  run(2000);
  CHECK_EQ(g_app.keyScript().dropped(), 0u);
  CHECK(g_hal.inPos == g_hal.in.size());   // all of it read in the end
  const std::string k = reportLine("[keys]");
  CHECK(k.find("from the computer 1000 ") != std::string::npos);
  CHECK(k.find("dropped 0,") != std::string::npos);
  CHECK(k.find("input held back 0 times") == std::string::npos);   // it did hold back
  if (k.find("from the computer 1000 ") == std::string::npos) std::printf("  %s\n", k.c_str());
}

void testRepeatAndWaits() {
  // Auto-repeat as Tk on macOS sends it (presses while held), then a release.
  const uint32_t before = g_app.keyScript().eventsEmitted();
  for (int i = 0; i < 30; ++i) g_hal.in += ">down w\n";
  g_hal.in += ">up w\n";
  run(200);
  CHECK_EQ(g_app.keyScript().eventsEmitted() - before, 2u);
  CHECK(g_app.keyScript().repeats() >= 29u);
  CHECK_EQ(g_app.keyScript().held(), 0ull);
  // A script with waits, bigger than the queue: its timing is kept (each tap 40 ms).
  const uint32_t b2 = g_app.keyScript().eventsEmitted();
  for (int i = 0; i < 100; ++i) g_hal.in += ">tap e 40\n";   // 300 commands, 4 s of taps
  run(1000);
  const uint32_t mid = g_app.keyScript().eventsEmitted() - b2;
  CHECK(mid >= 40u && mid <= 52u);   // ~25 taps a second: 25 downs and 25 ups, not all at once
  run(4000);
  CHECK_EQ(g_app.keyScript().eventsEmitted() - b2, 200u);
  CHECK_EQ(g_app.keyScript().dropped(), 0u);
}

}  // namespace

int main() {
  testBurst();
  testRepeatAndWaits();
  return pgdt::finish("test_daisy_keys");
}
