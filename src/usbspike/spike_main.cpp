// Pocket Groove -- USB audio hardware spike (env:usb-audio-spike).
//
// Proves on the device that the instrument can appear on a Mac as a
// class-compliant stereo USB audio INPUT ("Pocket Groove", UAC1, 16-bit,
// 44.1 kHz or 32 kHz) next to the usual CDC serial port, while the codec keeps
// playing the mono sum. See docs/usb-audio.md for the why and the test steps.
//
// Audio path (the shape the instrument will use):
//   plat::audio task (core 0, prio 20, 128-frame blocks, paced by I2S)
//     render():  test signal, stereo
//                -> usbaudio::push()   stereo frames into TinyUSB's IN FIFO (never blocks)
//                -> mono sum (L+R)/2 into both slots -> I2S -> ES8311 -> jack
//   USB ISR (1 kHz): TinyUSB takes 44/45 frames per packet out of the FIFO.
//
// Test signal: left 440 Hz, right 660 Hz, both -12 dBFS sines, plus a 2 ms
// click on both channels once per second of *I2S time* (every `rate` frames),
// so a recording shows drift (click spacing != rate samples) and dropouts.
//
// LCD + serial (once a second): enumeration, streaming, rate, packet counts and
// sizes, FIFO level, under/overruns, I2S rate, heap. Serial commands: h d r b s.
// G0 button: tap = switch 44.1 <-> 32 kHz (restart), hold 2 s = download mode.
#include <Arduino.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../platform/audio_out.h"
#include "../platform/display.h"
#include "usb_audio.h"
#include "usb_desc_check.h"

extern "C" {
extern int _data_start, _bss_end;   // ESP-IDF sections.ld: start of .dram0.data, end of .bss
}

namespace {

// ---------------------------------------------------------------------------
// Test signal (audio task only)
// ---------------------------------------------------------------------------
struct Osc {   // rotating phasor: 4 mul + 2 add per sample, renormalised per block
  float c = 1.0f, s = 0.0f, cr = 1.0f, sr = 0.0f;
  void set(float hz, float fs) {
    const float w = 6.28318531f * hz / fs;
    cr = cosf(w);
    sr = sinf(w);
    c = 1.0f;
    s = 0.0f;
  }
  inline float next() {
    const float c2 = c * cr - s * sr;
    s = c * sr + s * cr;
    c = c2;
    return s;
  }
  inline void renorm() {
    const float g = 1.5f - 0.5f * (c * c + s * s);
    c *= g;
    s *= g;
  }
};

struct TestSignal {
  Osc      left, right;
  uint32_t rate = 44100;
  uint32_t pos = 0;           // frame position inside the current second
  uint32_t clickLen = 88;     // 2 ms
  float    amp = 0.2512f;     // -12 dBFS
  float    clickAmp = 0.5f;   // -6 dBFS peak, decaying
  void init(uint32_t fs) {
    rate = fs;
    left.set(440.0f, static_cast<float>(fs));
    right.set(660.0f, static_cast<float>(fs));
    clickLen = fs / 500u;
    pos = 0;
  }
  void IRAM_ATTR fill(int16_t* out, int frames) {
    const float invLen = 1.0f / static_cast<float>(clickLen);
    for (int i = 0; i < frames; ++i) {
      float l = amp * left.next();
      float r = amp * right.next();
      if (pos < clickLen) {
        // ~5.5 kHz square burst with a linear decay: easy to see and to hear
        const float env = clickAmp * (1.0f - static_cast<float>(pos) * invLen);
        const float sq = ((pos >> 2) & 1u) ? env : -env;
        l += sq;
        r += sq;
      }
      if (++pos >= rate) pos = 0;
      out[2 * i]     = static_cast<int16_t>(l * 32767.0f);   // |l|, |r| <= 0.76: no clipping
      out[2 * i + 1] = static_cast<int16_t>(r * 32767.0f);
    }
    left.renorm();
    right.renorm();
  }
};

TestSignal g_sig;
volatile uint32_t g_framesRendered = 0;
volatile int64_t  g_audioT0 = 0;

void IRAM_ATTR render(int16_t* out, int frames, void*) {
  if (g_audioT0 == 0) g_audioT0 = esp_timer_get_time();
  g_sig.fill(out, frames);
  usbaudio::push(out, frames);                 // stereo to USB (no-op when no stream is open)
  for (int i = 0; i < frames; ++i) {           // mono sum to the codec (it plays the LEFT slot)
    const int32_t m = (static_cast<int32_t>(out[2 * i]) + out[2 * i + 1]) / 2;
    out[2 * i] = out[2 * i + 1] = static_cast<int16_t>(m);
  }
  g_framesRendered = g_framesRendered + static_cast<uint32_t>(frames);
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------
void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

constexpr int kCols = 40;   // 240 px / 6 px (font size 1)
void lcdLine(int row, uint16_t color, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void lcdLine(int row, uint16_t color, const char* fmt, ...) {
  char buf[kCols + 1];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if (n > kCols) n = kCols;
  for (int i = n; i < kCols; ++i) buf[i] = ' ';   // overwrite what the previous text left
  buf[kCols] = 0;
  plat::display::directText(0, row * 8 + 2, buf, color, 1);
}

constexpr uint16_t kWhite = 0xFFFF, kGreen = 0x07E0, kYellow = 0xFFE0, kRed = 0xF800, kCyan = 0x07FF, kGrey = 0x8410;

void descLine(const char* line, void*) { say("[desc] %s\n", line); }

int g_descErrors = -1;      // -1: not checked (baseline build)

void hexDump(const char* tag, const uint8_t* d, size_t len) {
  for (size_t o = 0; o < len; o += 16) {
    char line[80];
    int n = snprintf(line, sizeof(line), "[usb] %s %04x:", tag, static_cast<unsigned>(o));
    for (size_t i = o; i < len && i < o + 16 && n < static_cast<int>(sizeof(line)) - 4; ++i)
      n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %02x", d[i]);
    say("%s\n", line);
  }
}

int checkDescriptors(bool verbose, bool dump) {
  size_t devLen = 0, cfgLen = 0;
  const uint8_t* dev = usbaudio::deviceDescriptor(&devLen);
  const uint8_t* cfg = usbaudio::configDescriptor(&cfgLen);
  if (!cfg) {
    say("[desc] no TinyUSB in this build (baseline): nothing to check\n");
    return -1;
  }
  if (dump) {
    say("[usb] device descriptor (%u B):\n", static_cast<unsigned>(devLen));
    if (dev) hexDump("dev", dev, devLen);
    say("[usb] configuration descriptor (%u B):\n", static_cast<unsigned>(cfgLen));
    hexDump("cfg", cfg, cfgLen);
  }
  usbspike::CheckResult r;
  const int errors = usbspike::checkConfiguration(cfg, cfgLen, usbaudio::sampleRate(), &r, descLine, nullptr, verbose);
  say("[desc] self-check of the %u-byte configuration the host receives: %s (%d errors, %d warnings); "
      "AC itf %u, AS itf %u, EP 0x%02x, max packet %u B, %lu Hz, %u ch x %u bit, %d IN EPs, FIFO %d/256 words\n",
      static_cast<unsigned>(cfgLen), errors ? "FAIL" : "PASS", errors, r.warnings, r.audioItfAc, r.audioItfAs,
      r.audioEpIn, r.audioMaxPacket, static_cast<unsigned long>(r.audioRate), r.audioChannels, r.audioBits,
      r.inEndpoints, r.fifoWordsUsed);
  return errors;
}

uint32_t stackFree(const char* taskName) {
  TaskHandle_t h = xTaskGetHandle(taskName);
  return h ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(h)) : 0;
}

// ---------------------------------------------------------------------------
// Periodic status
// ---------------------------------------------------------------------------
struct Snapshot {
  uint32_t ms = 0, packets = 0, framesSent = 0, pktNominal = 0, pktLarge = 0, pktSmall = 0, pktZero = 0;
};
Snapshot g_prev;
uint32_t g_openSeen = 0;
Snapshot g_atOpen;          // counters when the current stream was opened (for the long-run rate)

void status(bool lcd) {
  const uint32_t now = millis();
  const usbaudio::Stats u = usbaudio::stats();
  const plat::audio::Stats a = plat::audio::stats();

  if (u.streamOpens != g_openSeen) {
    g_openSeen = u.streamOpens;
    g_atOpen.packets = u.packets;
    g_atOpen.framesSent = u.framesSent;
  }
  const float dt = static_cast<float>(now - g_prev.ms) * 0.001f;
  const uint32_t dPk = u.packets - g_prev.packets;
  const uint32_t dFr = u.framesSent - g_prev.framesSent;
  const float pps = dt > 0.0f ? static_cast<float>(dPk) / dt : 0.0f;
  // Frames per USB frame x 1000 = device rate measured in the HOST's clock.
  const uint32_t lpk = u.packets - g_atOpen.packets, lfr = u.framesSent - g_atOpen.framesSent;
  const float hostRate = lpk > 200 ? static_cast<float>(lfr) * 1000.0f / static_cast<float>(lpk) : 0.0f;
  const float hostPpm = hostRate > 0.0f ? (hostRate / static_cast<float>(u.rate) - 1.0f) * 1.0e6f : 0.0f;
  // I2S rate measured against the ESP32's crystal (esp_timer).
  const int64_t t0 = g_audioT0;
  const float audioSec = t0 ? static_cast<float>(esp_timer_get_time() - t0) * 1.0e-6f : 0.0f;
  const float i2sRate = audioSec > 1.0f ? static_cast<float>(g_framesRendered) / audioSec : 0.0f;
  const float i2sPpm = i2sRate > 0.0f ? (i2sRate / static_cast<float>(u.rate) - 1.0f) * 1.0e6f : 0.0f;
  const uint32_t heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t heapMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  say("[usb] t=%lus %s%s%s rate=%lu (host set %lu) opens=%lu | pkts %lu/s: %lu x%u %lu x%u %lu x%u zero %lu other %lu | "
      "sent %.1f f/s (%+.0f ppm vs host) | fifo %u/%u tgt %u | underrun pkts %lu, overrun frames %lu | "
      "i2s %.1f Hz (%+.0f ppm vs xtal) i2s-underruns %lu cpu %.1f%% | heap %lu min %lu | stack usbd %lu\n",
      static_cast<unsigned long>(now / 1000), u.mounted ? "MOUNTED" : "not-mounted", u.suspended ? " SUSPENDED" : "",
      u.streaming ? " STREAMING" : " idle", static_cast<unsigned long>(u.rate), static_cast<unsigned long>(u.hostSetRate),
      static_cast<unsigned long>(u.streamOpens), static_cast<unsigned long>(dPk),
      static_cast<unsigned long>(u.pktNominal - g_prev.pktNominal), static_cast<unsigned>(u.rate / 1000u),
      static_cast<unsigned long>(u.pktLarge - g_prev.pktLarge), static_cast<unsigned>(u.rate / 1000u + 1u),
      static_cast<unsigned long>(u.pktSmall - g_prev.pktSmall), static_cast<unsigned>(u.rate / 1000u - 1u),
      static_cast<unsigned long>(u.pktZero - g_prev.pktZero), static_cast<unsigned long>(u.pktOther),
      static_cast<double>(dt > 0.0f ? static_cast<float>(dFr) / dt : 0.0f), static_cast<double>(hostPpm),
      u.fifoLevel, u.fifoDepth, u.fifoTarget, static_cast<unsigned long>(u.pktZero),
      static_cast<unsigned long>(u.overrunFrames), static_cast<double>(i2sRate), static_cast<double>(i2sPpm),
      static_cast<unsigned long>(a.underruns), static_cast<double>(a.cpuPercent), static_cast<unsigned long>(heapFree),
      static_cast<unsigned long>(heapMin), static_cast<unsigned long>(stackFree("usbd")));

  if (lcd) {
    int row = 0;
    lcdLine(row++, kCyan, "POCKET GROOVE  USB AUDIO SPIKE");
    if (!u.usbCompiled) {
      lcdLine(row++, kYellow, "BASELINE BUILD: no TinyUSB");
    } else {
      lcdLine(row++, u.mounted ? kGreen : kRed, "USB  %s%s", u.mounted ? "enumerated" : "NOT enumerated",
              u.suspended ? " (suspended)" : "");
      lcdLine(row++, u.streaming ? kGreen : kYellow, "Audio %s  opens %lu", u.streaming ? "STREAMING" : "idle (no app)",
              static_cast<unsigned long>(u.streamOpens));
    }
    lcdLine(row++, kWhite, "Rate %lu Hz  host set %lu", static_cast<unsigned long>(u.rate),
            static_cast<unsigned long>(u.hostSetRate));
    lcdLine(row++, kWhite, "Pkts %lu  %.0f/s", static_cast<unsigned long>(u.packets), static_cast<double>(pps));
    lcdLine(row++, kWhite, " x%u:%lu x%u:%lu x%u:%lu", static_cast<unsigned>(u.rate / 1000u), static_cast<unsigned long>(u.pktNominal),
            static_cast<unsigned>(u.rate / 1000u + 1u), static_cast<unsigned long>(u.pktLarge), static_cast<unsigned>(u.rate / 1000u - 1u),
            static_cast<unsigned long>(u.pktSmall));
    lcdLine(row++, kWhite, "Host-clock rate %.1f (%+.0fppm)", static_cast<double>(hostRate), static_cast<double>(hostPpm));
    lcdLine(row++, kWhite, "FIFO %u/%u B  target %u", u.fifoLevel, u.fifoDepth, u.fifoTarget);
    lcdLine(row++, (u.pktZero || u.pktOther) ? kRed : kGreen, "Underruns %lu (empty pkts)",
            static_cast<unsigned long>(u.pktZero));
    lcdLine(row++, u.overrunFrames ? kRed : kGreen, "Overruns %lu frames dropped", static_cast<unsigned long>(u.overrunFrames));
    lcdLine(row++, a.underruns ? kRed : kWhite, "I2S %.1f Hz (%+.0fppm) undr %lu", static_cast<double>(i2sRate),
            static_cast<double>(i2sPpm), static_cast<unsigned long>(a.underruns));
    lcdLine(row++, kWhite, "Heap %luk min %luk  cpu %.1f%%", static_cast<unsigned long>(heapFree / 1024),
            static_cast<unsigned long>(heapMin / 1024), static_cast<double>(a.cpuPercent));
    lcdLine(row++, g_descErrors == 0 ? kGreen : (g_descErrors < 0 ? kGrey : kRed), "Descriptor self-check: %s",
            g_descErrors == 0 ? "PASS" : (g_descErrors < 0 ? "n/a" : "FAIL"));
    lcdLine(row++, kGrey, "L 440Hz R 660Hz -12dB, click 1/s");
    lcdLine(row++, kGrey, "G0 tap: %s  hold 2s: flash mode", u.rate == 44100u ? "32k" : "44.1k");
  }

  g_prev.ms = now;
  g_prev.packets = u.packets;
  g_prev.framesSent = u.framesSent;
  g_prev.pktNominal = u.pktNominal;
  g_prev.pktLarge = u.pktLarge;
  g_prev.pktSmall = u.pktSmall;
  g_prev.pktZero = u.pktZero;
}

void help() {
  say("[usb] commands: h help | s status now | d dump + check USB descriptors | r switch 44100 <-> 32000 Hz (restarts) | "
      "b reboot into download mode (for flashing)\n"
      "[usb] G0 button: tap = switch rate, hold 2 s = download mode\n");
}

void toggleRate() {
  const uint32_t next = usbaudio::sampleRate() == 44100u ? 32000u : 44100u;
  say("[usb] restarting at %lu Hz\n", static_cast<unsigned long>(next));
  lcdLine(15, kYellow, "Restarting at %lu Hz...", static_cast<unsigned long>(next));
  delay(100);
  usbaudio::requestRateAndRestart(next);
}

void downloadMode() {
  say("[usb] rebooting into the ROM download mode: flash now (pio run -e <env> -t upload)\n");
  plat::display::setBrightness(255);
  M5.Display.fillScreen(0);
  plat::display::directText(4, 40, "DOWNLOAD MODE", kYellow, 2);
  plat::display::directText(4, 70, "flash now; power-cycle to leave", kWhite, 1);
  delay(200);
  usbaudio::rebootToDownloadMode();
}

uint32_t g_lastStatus = 0;
uint32_t g_g0Down = 0;
bool g_g0Was = false;

}  // namespace

void setup() {
  Serial.setTxTimeoutMs(100);
  Serial.begin(115200);

  auto cfg = M5.config();            // same bring-up as plat::sys::begin()
  cfg.serial_baudrate = 0;
  cfg.clear_display = true;
  cfg.output_power = true;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  cfg.internal_spk = true;           // stores the speaker config only; I2S stays ours
  cfg.external_imu = false;
  cfg.external_rtc = false;
  cfg.led_brightness = 0;
  M5.begin(cfg);
  plat::display::begin();
  plat::display::setBrightness(plat::display::kBrightnessFull);
  pinMode(0, INPUT_PULLUP);

  usbaudio::begin();
  const uint32_t rate = usbaudio::sampleRate();
  g_sig.init(rate);

  plat::audio::Config ac;
  ac.sampleRate = rate;
  const bool audioOk = plat::audio::begin(ac, render, nullptr);

  say("\n[usb] Pocket Groove USB audio spike, %s build, rate %lu Hz, audio %s%s\n",
      usbaudio::stats().usbCompiled ? "TinyUSB (UAC1 + CDC)" : "baseline (no USB audio)",
      static_cast<unsigned long>(rate), audioOk ? "running" : "FAILED: ", audioOk ? "" : plat::audio::lastError());
  say("[usb] static DRAM %lu B (.data+.bss), heap free %lu B, min-ever %lu B, largest block %lu B\n",
      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&_bss_end) - reinterpret_cast<uintptr_t>(&_data_start)),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  g_descErrors = checkDescriptors(false, false);
  help();
  status(true);
}

void loop() {
  int c;
  while ((c = Serial.read()) >= 0) {
    switch (c) {
      case 'h': case '?': help(); break;
      case 's': status(false); break;
      case 'd': g_descErrors = checkDescriptors(true, true); break;
      case 'r': toggleRate(); break;
      case 'b': downloadMode(); break;
      default: break;
    }
  }

  // G0: active low. Tap (< 1 s) = switch rate, hold 2 s = download mode.
  const uint32_t now = millis();
  const bool down = digitalRead(0) == LOW;
  if (down && !g_g0Was) g_g0Down = now;
  if (down && now - g_g0Down >= 2000) downloadMode();
  if (!down && g_g0Was && now - g_g0Down >= 30 && now - g_g0Down < 1000) toggleRate();
  g_g0Was = down;

  if (now - g_lastStatus >= 1000) {
    g_lastStatus = now;
    status(true);
  }
  delay(10);
}
