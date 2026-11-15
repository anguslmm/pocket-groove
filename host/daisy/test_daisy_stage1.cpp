// The firmware's whole App (daisy/app, as on the device) on a simulated Hal with a
// manual clock: one 96-frame block per 2 ms, the main loop once per block. Stage 1's
// paths end to end, driven over the serial port as the computer would:
//   recording  PERC set to a sampler instrument by keys (SHIFT+h, 9, '.' ...), ">down
//              l" with a tone on the line input's L, ">up l": the slot's header says
//              48,000 Hz; after a reboot (the App booted again from the same flash) the
//              slot is still there; the [linein] report line says what happened
//   pads       a synthetic hard hit on pad 1 through Hal::readPads (from render): the
//              core's padDown (the Ui holds pad 1, the kick fires), then padUp; a
//              slow press on pad 2: its pressure merged to at most one per 8 ms on the
//              way to the Ui, which ends at the press's offset and returns to 0 at the
//              release; ">hit 3 0.5 hold 50" from the computer: the same path; the job
//              by ">tap padjob" (the desktop's F4) and ">pads job drums", kept over a
//              reboot
//   settings   ">pad 1 thr 20 ceil 500 curve 0.5" changes the scanner and survives a
//              reboot (/int's snapshot); ">display turn" reaches Hal::applySettings
//   touch      ">touch 10 20 50" with the mirror on: "~PGT D 10 20", then "~PGT U"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "check.h"
#include "core/layout.h"
#include "core/nor_sim.h"
#include "core/ring.h"
#include "pg/ui/pads.h"

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
  std::string in, out;
  size_t inPos = 0;
  float pad[4] = {0, 0, 0, 0};
  int applied = 0;
  bool turned = false;
  uint32_t millis() override { return uint32_t(us / 1000); }
  uint32_t micros() override { return uint32_t(us); }
  uint32_t cycles() override { return uint32_t(us * 480); }
  uint32_t cpuHz() override { return 480000000u; }
  void serialWrite(const char* p, size_t n) override { out.append(p, n); }
  size_t serialRoom() override { return 1 << 20; }
  int serialRead() override { return inPos < in.size() ? static_cast<unsigned char>(in[inPos++]) : -1; }
  void setLed(bool) override {}
  void restart(bool) override {}
  bool audioRunning() override { return true; }
  uint32_t sampleRate() override { return kRate; }
  uint32_t blockFrames() override { return kBlock; }
  bool readPads(float* o) override {
    for (int i = 0; i < 4; ++i) o[i] = pad[i];
    return true;
  }
  bool lineInput() override { return true; }
  void applySettings(const DaisySettings& s) override {
    ++applied;
    turned = s.displayTurned;
  }
  const char* stageName() override { return "stage 1 (test)"; }
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
TestHal g_hal;
AppMemory g_mem;
AppConfig g_cfg;

// The line input: l(t) on L, R digital silence; t in seconds since boot.
double (*g_left)(double) = nullptr;
uint64_t g_frames = 0;

void boot() {
  g_hal.in.clear();
  g_hal.inPos = 0;
  g_app.boot(g_hal, g_mem, g_cfg);
}

// Runs `ms` of device time: per 2 ms block, the "interrupt" then a main-loop pass.
void run(uint32_t ms) {
  float blk[kBlock * 2];
  float in[kBlock * 2];
  for (uint32_t t = 0; t < ms; t += 2) {
    for (uint32_t i = 0; i < kBlock; ++i, ++g_frames) {
      const double s = double(g_frames) / kRate;
      in[2 * i] = g_left ? float(g_left(s)) : 0.0f;
      in[2 * i + 1] = 0.0f;
    }
    g_app.render(blk, int(kBlock), in);
    g_hal.us += 2000;
    g_app.loopOnce();
  }
}

void send(const std::string& s, uint32_t ms = 60) {
  g_hal.in += s;
  run(ms);
}

double quiet(double) { return 0.0002; }
double toneL(double t) { return 0.25 * std::sin(2.0 * 3.14159265358979 * 440.0 * t); }

const pg::sampler::SlotHeader* anySlot(int* slot) {
  for (int i = 0; i < pg::sampler::kPoolSlots; ++i)
    if (g_app.pool().header(i)) {
      *slot = i;
      return g_app.pool().header(i);
    }
  return nullptr;
}

void testRecording() {
  // PERC (track 6) selected silently, its SOUND page, the instrument stepped until it is a sampler
  send(">down sft\n>tap h\n>up sft\n>tap 9\n", 300);
  bool sampler = false;
  int taps = 0;
  for (; taps < 20; ++taps) {
    const pg::macro::MacroSet* s = g_app.ui().instrumentOf(pg::kTrackPerc);
    sampler = s && s->voice == pg::kVoiceSampler;
    if (sampler) break;
    send(">tap .\n", 400);
  }
  CHECK(sampler);
  std::printf("  PERC is a sampler instrument after %d taps of '.' (tools/test_pg_daisy.py relies on it)\n", taps);
  CHECK_EQ(g_app.ui().selTrack(), int(pg::kTrackPerc));
  int slot = -1;
  CHECK(anySlot(&slot) == nullptr);   // an empty pool
  // hold L: the room, then the tone, then let go
  g_left = quiet;
  send(">down l\n", 400);
  CHECK(g_app.lineMic().armed());
  g_left = toneL;
  run(1200);
  send(">up l\n", 300);
  g_left = quiet;
  run(300);
  CHECK(!g_app.lineMic().armed());
  const pg::sampler::SlotHeader* h = anySlot(&slot);
  CHECK(h != nullptr);
  if (!h) return;
  CHECK_EQ(h->rate, kRate);
  CHECK_EQ(slot, g_app.ui().sampleSlotOf(pg::kTrackPerc));
  const double secs = double(h->samples) / kRate;
  std::printf("  recorded slot %d: %u samples, %.3f s at %u Hz\n", slot + 1, unsigned(h->samples), secs, unsigned(h->rate));
  CHECK(secs > 1.0 && secs < 1.6);
  const uint32_t crc = h->dataCrc, samples = h->samples;
  g_hal.out.clear();
  send("r", 100);
  CHECK(g_hal.out.find("[linein] last: slot") != std::string::npos);
  CHECK(g_hal.out.find("mono L alone (R silent)") != std::string::npos);
  CHECK(g_hal.out.find("at 48000 Hz") != std::string::npos);
  // the copy on the flash: give the store task time (68 sectors, one per 5 ms), then reboot
  run(2000);
  CHECK(!g_app.poolMirror().dirty());
  boot();
  run(100);
  const pg::sampler::SlotHeader* h2 = g_app.pool().header(slot);
  CHECK(h2 && h2->rate == kRate && h2->samples == samples && h2->dataCrc == crc);
}

void testPads() {
  const uint32_t notes0 = g_app.padNotes(), ups0 = g_app.padUps();
  const uint8_t kickTrig0 = g_app.ui().engineStatus().trig[pg::kTrackKick];
  // a hard hit on pad 1: 900 uS over 2 ms, held 40 ms
  bool held = false;
  for (int i = 0; i < 30; ++i) {
    const float t = float(i) * 2.0f;
    const float g = t < 2.0f ? 450.0f * t : (t < 42.0f ? 900.0f : 0.0f);
    g_hal.pad[0] = PadScanner::readingFor(g);
    run(2);
    held = held || g_app.ui().padHeld(0);
  }
  g_hal.pad[0] = 0.0f;
  run(40);   // a UI frame: the engine's status
  CHECK(held);
  CHECK(!g_app.ui().padHeld(0));
  CHECK_EQ(g_app.padNotes(), notes0 + 1);
  CHECK_EQ(g_app.padUps(), ups0 + 1);
  CHECK(g_app.ui().engineStatus().trig[pg::kTrackKick] != kickTrig0);   // the kick played
  CHECK(g_app.pads().lastVelocity(0) > 0.9f);
  CHECK(g_app.pads().state(0) == PadScanner::State::Idle);
  CHECK(g_app.pads().hits(1) == 0 && g_app.pads().hits(2) == 0);

  // a slow press on pad 2 (the snare): 0 -> 400 uS over 200 ms, held 300 ms. The
  // scanner sends a pressure event at nearly every scan; the Ui gets at most one per 8 ms.
  const uint32_t pIn0 = g_app.padPressures();
  int maxOffset = 0;
  for (int ms = 0; ms < 500; ms += 2) {
    const float g = ms < 200 ? 2.0f * float(ms) : 400.0f;
    g_hal.pad[1] = PadScanner::readingFor(g);
    run(2);
    const int o = g_app.ui().pressOffset(pg::kTrackSnare);
    maxOffset = o > maxOffset ? o : maxOffset;
  }
  const uint32_t toUi = g_app.padPressures() - pIn0;
  const float pEnd = (400.0f - PadCal{}.threshold) / (PadCal{}.ceiling - PadCal{}.threshold);
  const int want = pg::ui::pressOffset16(pEnd, 1);   // BITE: no press line, the default response (smooth)
  std::printf("  slow press: %u pressure values to the Ui in 500 ms, offset %d (want %d), macro %d\n", unsigned(toUi),
              g_app.ui().pressOffset(pg::kTrackSnare) / pg::kOffsetScale, want / pg::kOffsetScale,
              g_app.ui().pressMacro(pg::kTrackSnare));
  CHECK(g_app.ui().padHeld(1));
  CHECK(toUi >= 10 && toUi <= 500 / pg::ui::kPressEveryMs + 2);
  CHECK(g_app.ui().pressMacro(pg::kTrackSnare) >= 0);
  // the last value got through (to the scanner's step of 1/128: 1.25 units at most)
  CHECK(std::abs(g_app.ui().pressOffset(pg::kTrackSnare) - want) <= 2 * pg::kOffsetScale);
  g_hal.pad[1] = 0.0f;
  run(50);
  CHECK(!g_app.ui().padHeld(1));
  CHECK_EQ(g_app.ui().pressOffset(pg::kTrackSnare), 0);

  // from the computer: the same path
  const uint32_t notes1 = g_app.padNotes();
  send(">hit 3 0.5 hold 50\n", 20);
  CHECK(g_app.ui().padHeld(2));
  CHECK_EQ(g_app.padNotes(), notes1 + 1);
  run(60);
  CHECK(!g_app.ui().padHeld(2));
  send(">hit 3 1 hold 0 press 0.5\n", 20);
  CHECK(g_app.ui().padHeld(2) && g_app.ui().pressOffset(pg::kTrackHatClosed) > 0);
  run(200);
  CHECK(g_app.ui().padHeld(2));   // hold 0: until ">hit 3 up"
  send(">hit 3 up\n", 20);
  CHECK(!g_app.ui().padHeld(2) && g_app.ui().pressOffset(pg::kTrackHatClosed) == 0);
  g_hal.out.clear();
  send(">hit 9\n>hit 0 1\n", 20);
  CHECK(g_hal.out.find("[pads] ? >hit N VEL") != std::string::npos);

  // the job: the desktop's key, a command; saved with the Daisy's settings
  CHECK(g_app.ui().padJob() == pg::ui::PadJob::Drums);
  send(">tap padjob\n", 100);
  CHECK(g_app.ui().padJob() == pg::ui::PadJob::Notes);
  CHECK(std::strcmp(g_app.ui().toastText(), "PADS: NOTES") == 0);
  CHECK_EQ(g_app.ui().padTrack(0), g_app.ui().selTrack());
  CHECK_EQ(int(g_app.settings().padJob), 1);
  run(3000);   // /int's snapshot
  boot();
  run(50);
  CHECK(g_app.ui().padJob() == pg::ui::PadJob::Notes);
  g_hal.out.clear();
  send(">pads job drums\n", 50);
  CHECK(g_app.ui().padJob() == pg::ui::PadJob::Drums && g_app.settings().padJob == 0);
  CHECK(g_hal.out.find("[pads] job drums") != std::string::npos);
  CHECK(g_hal.out.find("[pads] 1 plays KICK note 60") != std::string::npos);
}

void testSettings() {
  g_hal.out.clear();
  send(">pad 1 thr 20 ceil 500 curve 0.5\n", 50);
  CHECK(g_app.settings().pads[0].threshold == 20.0f);
  CHECK(g_app.pads().cal(0).ceiling == 500.0f && g_app.pads().cal(0).curve == 0.5f);
  CHECK(g_hal.out.find("thr 20.0 uS ceil 500 uS curve 0.50 on") != std::string::npos);
  send(">pad 4 off\n", 50);
  CHECK(!g_app.settings().pads[3].on);
  send(">pads xtalk 0.3 8\n", 50);
  CHECK(g_app.settings().padGlobal.xtalkRatio > 0.29f && g_app.settings().padGlobal.xtalkScans == 4);
  const int applied = g_hal.applied;
  send(">display turn\n", 50);
  CHECK(g_hal.applied > applied && g_hal.turned);
  // /int's snapshot follows 1.5 s after the last change; then a reboot loads them
  run(3000);
  boot();
  run(50);
  CHECK(g_app.settings().pads[0].threshold == 20.0f && !g_app.settings().pads[3].on);
  CHECK(g_app.pads().cal(0).curve == 0.5f);
  CHECK(g_hal.turned);
  g_hal.out.clear();
  send(">pads\n", 50);
  CHECK(g_hal.out.find("settings loaded") != std::string::npos);
  send(">pads reset\n>display turn\n", 50);
  CHECK(g_app.settings().pads[0].threshold == 10.0f && !g_hal.turned);
  // the watch line
  g_hal.out.clear();
  send(">pads watch 100\n", 450);
  size_t lines = 0;
  for (size_t p = 0; (p = g_hal.out.find("[pads] x ", p)) != std::string::npos; ++p) ++lines;
  CHECK(lines >= 3 && lines <= 5);
  send(">pads watch off\n", 50);
}

void testTouch() {
  send("m", 50);   // the screen mirror on
  CHECK(g_app.mirroring());
  g_hal.out.clear();
  send(">touch 10 20 50\n", 20);
  CHECK(g_hal.out.find("~PGT D 10 20\n") != std::string::npos);
  CHECK(g_hal.out.find("~PGT U") == std::string::npos);
  run(60);
  CHECK(g_hal.out.find("~PGT U 10 20\n") != std::string::npos);
  CHECK_EQ(g_app.pointerEvents(), 2u);
  send("m", 50);
  g_hal.out.clear();
  send("r", 100);
  CHECK(g_hal.out.find("[touch] no touch controller in this build; touches 1, events 2") != std::string::npos);
  CHECK(g_hal.out.find("last canvas (10,20)") != std::string::npos);
}

}  // namespace

int main() {
  g_fsRegion.base = &g_nor;
  g_poolRegion.base = &g_nor;
  g_poolRegion.off = 2 * layout::kFsBankBytes;
  g_mem.fsArena = g_fsArena.data();
  g_mem.fsSlotBytes = layout::kFsSlotBytes;
  g_mem.fsStaging = g_fsStage.data();
  g_mem.fsBankBytes = layout::kFsBankBytes;
  g_mem.fsFlash = &g_fsRegion;
  g_mem.poolImage = g_poolImage.data();
  g_mem.poolStaging = g_poolStage.data();
  g_mem.poolFlash = &g_poolRegion;
  g_mem.lineRing = g_lineRing.data();
  g_mem.lineRingSamples = uint32_t(g_lineRing.size());
  g_cfg.sampleRate = float(kRate);
  g_cfg.version = "test";
  boot();
  run(500);
  testRecording();
  testPads();
  testSettings();
  testTouch();
  return pgdt::finish("test_daisy_stage1");
}
