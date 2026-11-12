// Pocket Groove -- hardware spike for tilt (BMI270) and sampling (ES8311 mic
// to flash, playback from memory-mapped flash). env:hw-spike, docs/hw-spike.md.
//
// Two pages, switched with Tab (or the G0 button):
//   TILT     IMU rates, noise, drift, latency, key impact; a playable filter
//            sweep (hold Space and tilt); shake counter.
//   SAMPLER  mic level, gain presets, record to flash, play back at pitches
//            with 1..8 voices, playback-cost benchmark.
// Enter prints the current page's report block to USB serial (115200);
// serial 'T' / 'S' / 'A' print the tilt / sampler / both blocks.
//
// Uses src/platform for board bring-up (sys), the LCD (display) and the keys;
// its own full-duplex codec driver (duplex_audio), IMU reader (imu) and
// flash store (sample_store). Does not link lib/pg and never mounts LittleFS.
#include <Arduino.h>

#include <esp_heap_caps.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../platform/display.h"
#include "../platform/keys.h"
#include "../platform/system.h"
#include "app.h"
#include "duplex_audio.h"
#include "mic_matrix.h"
#include "pg/platform.h"

namespace sys = plat::sys;
namespace keys = plat::keys;
namespace disp = plat::display;

namespace hws {
namespace app {

namespace {
pg::Framebuffer g_fb;
char g_msg[48] = "";
uint32_t g_msgUntil = 0;
}  // namespace

smoke::Canvas g_cv(&g_fb);
uint32_t g_sampleRate = 44100;

void sp(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sys::serialWrite(buf);
}

void msg(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_msg, sizeof(g_msg), fmt, ap);
  va_end(ap);
  g_msgUntil = millis() + 3000;
}

const char* currentMsg() { return (static_cast<int32_t>(millis() - g_msgUntil) < 0) ? g_msg : ""; }

void reportSink(const char* line, void*) {
  sys::serialWrite(line);
  sys::serialWrite("\n");
  // A whole block is ~30 lines; give the 4 KB TX ring time to drain so
  // nothing is dropped by the non-blocking serial writer.
  delay(2);
}

void fillMemory(uint32_t* heapFree, uint32_t* heapMin, uint32_t* largest, uint32_t* staticDram) {
  const sys::HeapInfo h = sys::heap();
  if (heapFree) *heapFree = h.freeBytes;
  if (heapMin) *heapMin = h.minEverFree;
  if (largest) *largest = h.largestBlock;
  if (staticDram) *staticDram = sys::staticDramBytes();
}

}  // namespace app
}  // namespace hws

using namespace hws;

namespace {

enum Page : uint8_t { kTilt = 0, kSampler = 1 };
volatile uint8_t g_page = kTilt;
uint32_t g_lastDraw = 0;

void process(const int16_t* in, int16_t* out, int frames, void*) {
  sampler::input(in, frames);
  if (micmatrix::running()) micmatrix::process(in, out, frames);   // test tone + capture
  else if (g_page == kTilt) tilt::render(out, frames);
  else sampler::render(out, frames);
}

void setPage(uint8_t p) {
  if (p == g_page) return;
  if (g_page == kTilt) tilt::leave(); else sampler::leave();
  g_page = p;
  if (g_page == kTilt) tilt::enter(); else sampler::enter();
}

constexpr uint8_t kTab = app::key(1, 0), kEnter = app::key(2, 13), kMinus = app::key(0, 11), kEquals = app::key(0, 12);

void handleKey(const keys::Event& e) {
  const uint8_t k = e.ev.key;
  if (k < pg::kNumKeys && e.raw != 0) {
    tilt::noteKeyEvent(e.ev.down);
    if (e.ev.down) tilt::noteKeyLatency(e.tReadUs - e.tIntUs);
  }
  if (e.ev.down) {
    if (k == kTab || k == keys::kKeyG0) { setPage(g_page == kTilt ? kSampler : kTilt); return; }
    if (k == kEnter) {
      if (g_page == kTilt) tilt::report(); else sampler::report();
      app::msg("report printed to serial");
      return;
    }
    if (k == kMinus || k == kEquals) {
      if (micmatrix::running()) { app::msg("mic matrix running"); return; }
      int v = duplex::volumeReg() + (k == kEquals ? 4 : -4);
      if (v > 0xBF) v = 0xBF;
      if (v < 0x40) v = 0x40;
      duplex::setVolumeReg(static_cast<uint8_t>(v));
      app::msg("volume %.1f dB", static_cast<double>((v - 0xBF) * 0.5f));
      return;
    }
  }
  if (g_page == kTilt) tilt::key(k, e.ev.down);
  else sampler::key(k, e.ev.down);
}

void pollSerial() {
  for (int n = 0; n < 8; ++n) {
    const int c = sys::serialRead();
    if (c < 0) return;
    switch (c) {
      case 'T': tilt::report(); break;
      case 'S': sampler::report(); break;
      case 'A': tilt::report(); sampler::report(); break;   // the sampler block includes the matrix block
      case '>': setPage(g_page == kTilt ? kSampler : kTilt); break;
      case 'M':
        setPage(kSampler);
        sampler::startMatrix();
        break;
      case 'Q': micmatrix::report(); break;
      case '?':
        app::sp("[hwspike] serial: T tilt report, S sampler report, A all, > next page, M mic matrix, Q matrix report\n");
        break;
      default: break;
    }
  }
}

}  // namespace

void setup() {
  const sys::BootInfo boot = sys::begin(115200);
  disp::begin();
  app::g_cv.initPalette();
  app::g_cv.clear(smoke::kBlack);
  app::g_cv.title("POCKET GROOVE hw-spike", kReportVersion);
  app::g_cv.line(2, smoke::kWhite, "board %s", boot.boardName);
  app::g_cv.line(3, smoke::kYellow, "starting...");
  disp::present(*app::g_cv.fb());
  app::sp("\n\n===== POCKET GROOVE HW-SPIKE %s (built %s %s) =====\n", kReportVersion, __DATE__, __TIME__);
  app::sp("[boot] board %s (%d)\n", boot.boardName, boot.boardId);

  keys::begin();

  // Audio before the big allocations (DMA buffers want internal RAM).
  duplex::Config cfg;
  cfg.sampleRate = app::g_sampleRate;
  const bool aok = duplex::begin(cfg, process, nullptr);
  app::sp("[boot] audio full duplex: %s %s\n", aok ? "OK" : "FAILED", duplex::lastError());

  tilt::begin();
  sampler::begin();
  tilt::enter();
  app::sp("[boot] done. Tab/G0 page, Enter report, SAMPLER m = mic matrix. Serial: T S A M Q > ?\n");
}

void loop() {
  keys::Event e;
  for (int n = 0; n < 32 && keys::poll(&e); ++n) handleKey(e);
  pollSerial();
  tilt::tick();
  sampler::tick();
  const uint32_t now = millis();
  if (now - g_lastDraw >= 66) {
    g_lastDraw = now;
    if (g_page == kTilt) tilt::draw(); else sampler::draw();
    disp::present(*app::g_cv.fb());
  }
  delay(1);
}
