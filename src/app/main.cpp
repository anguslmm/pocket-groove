// Pocket Groove -- the instrument firmware (env:cardputer-adv).
//
// Tasks (FreeRTOS, ESP32-S3 dual core; CONFIG_FREERTOS_HZ = 1000):
//   pg_audio  core 0, prio 20, 6 KB stack (plat::audio): render = Instrument::process,
//             paced by the blocking I2S write. Touches no I2C, file, heap or lock.
//   pg_keys   core 1, prio 10, 4 KB (plat::keys): drains the TCA8418 FIFO over I2C.
//   pg_imu    core 1, prio 9, 3 KB from the heap (plat::imu): reads the BMI270 at 50 Hz
//             ONLY while G0 is held (tilt), runs the tilt tracker (tilt_input.cpp);
//             asleep otherwise.
//   loopTask  core 1, prio 1, 12 KB (Arduino loop, this file): UI, display,
//             codec volume (I2C), files, serial. Sleeps in keys::waitEvent()
//             between ~30 fps frames, so keys are handled as they arrive.
//   pg_sdmount core 1, prio 1, 8 KB, transient (plat::storage), at boot.
// Big objects are static (.bss/.data): Instrument (+ the engine's state block),
// Ui, Framebuffer. Nothing large is allocated on the heap or a task stack.
//
// Gestures the firmware handles itself (the UI never sees the key):
//   G0 held + tilt        the selected track's tilt macros (docs/UX.md 10.3 "Tilt"): the
//                         Ui gets the gesture (tiltPress / tiltInput / tiltRelease)
//   G0 held + '-' / '='   codec (analog) output volume -/+ 2 dB, persisted (cancels the tilt)
//   G0 double-tap         diagnostics strip on/off (a tap never tilts: Ui::kTiltEngageMs)
//   first key after the backlight dimmed: only wakes the screen
// Boot menu: hold any key at power-on (or after a crash: automatic).
// Serial (USB CDC): 'r' full report, 'h' help; a stats line every 5 s.
// Project transfer on the same port (tools/pg ls / pull / push): lines that
// start with "~PGX ", handled by pg::io::Xfer (protocol in pg/io/xfer.h).
//
// USB audio build (env:cardputer-adv-usb, PG_USB_AUDIO): the engine renders
// STEREO; the audio callback hands the frames to plat::usbaudio (the
// "Pocket Groove" USB input) and the jack plays (L+R)/2. The serial port is
// the TinyUSB CDC port, started in setup() once the sample rate is known.
// Extra serial commands there: 'l' boot log again, 'u' USB descriptors,
// 'm' then 'y' restart into the boot menu, 'B' then 'y' download mode; the
// 1200-baud touch of `pio run -t upload` also reboots into download mode.
//
// This file includes only platform and pg headers: no Arduino / M5 / ESP-IDF.
#include <esp_heap_caps.h>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../platform/audio_out.h"
#include "../platform/display.h"
#include "../platform/imu.h"
#include "../platform/keys.h"
#include "../platform/storage.h"
#include "../platform/system.h"
#if PG_USB_AUDIO
#include <atomic>

#include "../platform/usb_audio.h"
#include "../platform/usb_desc_check.h"
#endif
#include "app_settings.h"
#include "boot_menu.h"
#include "overlay.h"
#include "tilt_input.h"
#include "pg/instrument/instrument.h"
#include "pg/io/project_io.h"
#include "pg/io/xfer.h"
#include "pg/ui/gfx.h"
#include "pg/ui/ui.h"

// Arduino core hook (weak in cores/esp32/main.cpp): stack of the loop task.
// 8 KB is the default; the UI + LittleFS/FATFS paths (FATFS keeps long-file-
// name buffers on the stack) + snprintf get 12 KB. The high-water mark is in
// the diagnostics report, so this can be trimmed once measured on hardware.
size_t getArduinoLoopTaskStackSize() { return 12 * 1024; }

namespace {

namespace audio = plat::audio;
namespace keys = plat::keys;
namespace sys = plat::sys;
namespace disp = plat::display;
namespace storage = plat::storage;

// ---- static objects ------------------------------------------------------------
pg::Instrument g_inst;          // ~70 KB + the engine's static state block
pg::ui::Ui g_ui;
// The 32.9 KB frame buffer lives on the heap, taken once before anything else
// allocates. Static data plus the code placed in IRAM had filled the linker's
// data segment (dram0_0_seg, 341.7 KB); the heap continues above that segment,
// where static data cannot go, so this moves the buffer to where the free RAM
// is. Total RAM use is unchanged.
pg::Framebuffer* allocFramebuffer() {
  void* p = heap_caps_calloc(1, sizeof(pg::Framebuffer), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) abort();
  return static_cast<pg::Framebuffer*>(p);
}
pg::Framebuffer& g_fb = *allocFramebuffer();
app::Overlays g_ov;
app::Settings g_set;
pg::io::Xfer g_xfer;            // USB project transfer (~0.4 KB)

// ---- constants -----------------------------------------------------------------
constexpr uint32_t kFrameMs = 33;              // ~30 fps
// Full = 255 = DC on the backlight pin (no PWM edges to couple into the audio
// line); dim = 39 kHz PWM, only while nothing can be heard (see display.h).
constexpr uint8_t kBrightFull = disp::kBrightnessFull, kBrightDim = 16;
constexpr uint32_t kDimAfterMs = 60000;
constexpr uint32_t kDimTailMs = 10000;          // after stop: delay / reverb tails may still ring
constexpr uint32_t kVolShowMs = 1500;
constexpr uint32_t kVolSaveDelayMs = 2000;
constexpr uint32_t kStatsEveryMs = 5000;
// While a transfer runs, wait up to this long per frame for the host's next
// request, so a file moves at several chunks per frame instead of one.
constexpr uint32_t kXferSpinMs = 8;
constexpr uint32_t kTapMaxMs = 350, kDoubleTapGapMs = 450;
constexpr uint32_t kTiltSliceMs = 5;            // the loop's wait while G0 tilts (IMU readings every 20 ms)
constexpr uint8_t kKMinus = 11, kKEquals = 12;  // pg/platform.h key ids
constexpr uint64_t kMatrixMask = (1ull << pg::kNumKeys) - 1;

// ---- state -----------------------------------------------------------------------
sys::ChipInfo g_chip;
sys::HeapInfo g_heapBoot, g_heapReady;
bool g_abnormal = false;
bool g_keysOk = false;
bool g_imuOk = false;
bool g_audioOk = false;
char g_audioErr[64] = "";
uint64_t g_swallow = 0;          // keys whose next key-up must not reach the UI
bool g_diagOn = false;
bool g_dimmed = false;
bool g_playing = false;
uint32_t g_lastKeyMs = 0;
uint32_t g_lastPlayingMs = 0;     // last frame the transport ran
bool g_g0Held = false, g_g0UsedForVol = false, g_g0HaveTap = false;
uint32_t g_g0DownUs = 0, g_g0LastTapUs = 0;   // keys::Event::tIntUs (wraps; compared by difference)
bool g_sdRecovered = false;      // pg::io::recoverAfterPowerLoss has run on the current SD mount
uint32_t g_volShowUntil = 0, g_volChangedMs = 0;
bool g_volDirty = false;
uint8_t g_volReg = app::kVolDefault;   // current codec volume (may differ from g_set at boot: ceiling)
uint32_t g_overSinceMs = 0, g_underSinceMs = 0;
bool g_overloadWarn = false, g_overloadAck = false;
float g_frameUs = 0, g_frameMaxUs = 0;
uint32_t g_lastDiagMs = 0, g_lastStatsMs = 0, g_lastBattMs = 0;
int g_battPct = -1, g_battMv = -1;
uint32_t g_loopStackFree = 0;
#if PG_USB_AUDIO
namespace usb = plat::usbaudio;
uint32_t g_usbRate = 0;               // the rate in the USB descriptor (fixed for this boot)
bool g_usbOk = false;
int g_usbDescErrors = -1;             // descriptor self-check at boot (-1: not run)
bool g_hostWasListening = false, g_bootLogReplayed = false;
std::atomic<bool> g_dlReq{false};     // 1200-baud touch (USB event task) -> loop
char g_pending = 0;                   // 'm' or 'B' waiting for 'y'
uint32_t g_pendingMs = 0;
// Heap minimum-ever as boot went on, to find what takes the heap at its low
// point: [0] after sys::begin, [1] storage, [2] USB, [3] boot menu + SD,
// [4] UI boot (session load), [5] audio. Runtime drops are logged.
uint32_t g_minAt[6] = {};
uint32_t g_minSeen = 0, g_minDropMs = 0;
// For the strip: counters one second ago, so the colour shows what is happening now.
uint32_t g_usbPrevBad = 0, g_usbBadMs = 0;
#endif

void sp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void sp(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sys::serialWrite(buf);
}

// ---- audio -------------------------------------------------------------------------
#if PG_USB_AUDIO
// Stereo for USB, (L+R)/2 for the codec (it plays the left slot).
void render(int16_t* out, int frames, void*) {
  g_inst.process(out, frames);
  plat::usbaudio::pushAndFoldToMono(out, frames);
}
#else
void render(int16_t* out, int frames, void*) { g_inst.process(out, frames); }
#endif

uint32_t sampleRate() { return app::sampleRateHz(g_set.rate); }

const char* keyMode() {
  const keys::Stats ks = keys::stats();
  if (!ks.controllerPresent) return "NOKBD";
  if (ks.polledEvents > 0 && ks.intLowDrains == 0) return "POLL";
  if (ks.polledEvents > 0) return "MIXED";
  if (ks.intLowDrains > 0) return "INT";
  return "INT?";
}

// "-39.5" (no float printf in the hot formatting paths, and keeps -Wdouble-promotion quiet)
void fmtDb(char* out, size_t cap, uint8_t reg) {
  const int half = static_cast<int>(reg) - 0xBF;   // 0.5 dB units
  const int a = half < 0 ? -half : half;
  snprintf(out, cap, "%s%d.%d", half < 0 ? "-" : "", a / 2, (a & 1) ? 5 : 0);
}
int tenths(float v) { return static_cast<int>(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f)); }

// ---- volume --------------------------------------------------------------------------
void setVolume(int reg, uint32_t now) {
  if (reg < 0) reg = 0;
  if (reg > app::kVolMax) reg = app::kVolMax;
  g_volReg = static_cast<uint8_t>(reg);
  if (g_audioOk) audio::setVolumeReg(g_volReg);   // I2C, UI thread
  g_set.volReg = g_volReg;
  g_volDirty = true;
  g_volChangedMs = now;
  g_volShowUntil = (now + kVolShowMs) | 1u;
}

// ---- keys ------------------------------------------------------------------------------
void wake() {
  if (!g_dimmed) return;
  g_dimmed = false;
  disp::setBrightness(kBrightFull);
}

void handleKey(const keys::Event& e, uint32_t now) {
  const uint8_t k = e.ev.key;
  if (k >= keys::kNumIds) return;
  const uint64_t bit = 1ull << k;
  if (!e.ev.down) {
    if (k == keys::kKeyG0) {
      // Timed with the reader task's timestamps, not the UI clock: a frame
      // stalled by a flash write would otherwise make a hold look like a tap.
      const uint32_t t = e.tIntUs;
      if (g_g0Held && !g_g0UsedForVol && t - g_g0DownUs < kTapMaxMs * 1000u) {
        if (g_g0HaveTap && t - g_g0LastTapUs < kDoubleTapGapMs * 1000u) {
          g_diagOn = !g_diagOn;
          if (!g_diagOn && g_overloadWarn) g_overloadAck = true;   // seen it: stop nagging outside the strip
          g_lastDiagMs = 0;
          g_g0HaveTap = false;
        } else {
          g_g0HaveTap = true;
          g_g0LastTapUs = t;
        }
      } else {
        g_g0HaveTap = false;     // a hold (or a volume gesture) in between cancels a pending first tap
      }
      g_g0Held = false;
      g_ui.tiltRelease(now);
      app::tilt::release();
      g_swallow &= ~bit;
      return;
    }
    if (g_swallow & bit) { g_swallow &= ~bit; return; }
    pg::KeyEvent ev = e.ev;
    ev.timeUs = e.tIntUs;
    g_ui.onKey(ev, now);
    return;
  }

  g_lastKeyMs = now;
  if (g_dimmed) {               // the first key only wakes the screen
    wake();
    if (k == keys::kKeyG0) { g_g0Held = true; g_g0UsedForVol = true; g_g0DownUs = e.tIntUs; }   // not a tap
    else g_swallow |= bit;
    return;
  }
  if (k == keys::kKeyG0) {
    g_g0Held = true;
    g_g0UsedForVol = false;
    g_g0DownUs = e.tIntUs;
    if (g_imuOk) g_ui.tiltPress(now);   // the pose now is the zero; it engages after a tap's time
    if (g_ui.tiltHeld()) {
      const pg::ui::Ui::TiltSpec ts = g_ui.tiltSpec();
      if (ts.active) app::tilt::press(ts.response[0], ts.response[1]);   // the IMU wakes only for this
    }
    return;
  }
  if (g_g0Held && (k == kKMinus || k == kKEquals)) {
    if (g_ui.tiltHeld()) {          // the volume gesture takes the hold: the tilt glides back
      g_ui.tiltCancel(now);
      app::tilt::release();
    }
    g_g0UsedForVol = true;
    g_swallow |= bit;
    setVolume(static_cast<int>(g_volReg) + (k == kKEquals ? app::kVolStep : -static_cast<int>(app::kVolStep)), now);
    return;
  }
  if (g_swallow & bit) return;   // e.g. still the boot-menu key: its down was eaten, keep its up too
  pg::KeyEvent ev = e.ev;
  ev.timeUs = e.tIntUs;
  g_ui.onKey(ev, now);
}

// ---- diagnostics -------------------------------------------------------------------------
#if PG_USB_AUDIO
const char* phaseName(const usb::Stats& u) {
  if (!u.started) return "OFF";
  if (!u.mounted) return "NO HOST";
  if (u.suspended) return "SUSPENDED";
  if (!u.streaming) return "IDLE";
  return u.phase == usb::Phase::Running ? "STREAMING" : "OPENING";
}

void descLine(const char* line, void*) { sp("[desc] %s\n", line); }

int checkUsbDescriptors(bool verbose, bool dump) {
  size_t devLen = 0, cfgLen = 0;
  const uint8_t* dev = usb::deviceDescriptor(&devLen);
  const uint8_t* cfg = usb::configDescriptor(&cfgLen);
  if (!cfg) {
    sp("[desc] USB stack not running: nothing to check\n");
    return -1;
  }
  if (dump) {
    // The format host/usbspike/run_checks.sh --dump reads.
    sp("[usb] rate %lu\n", static_cast<unsigned long>(g_usbRate));
    const struct { const char* tag; const uint8_t* d; size_t n; } parts[] = {{"dev", dev, dev ? devLen : 0}, {"cfg", cfg, cfgLen}};
    for (const auto& part : parts) {
      for (size_t o = 0; o < part.n; o += 16) {
        char line[80];
        int n = snprintf(line, sizeof(line), "[usb] %s %04x:", part.tag, static_cast<unsigned>(o));
        for (size_t i = o; i < part.n && i < o + 16 && n < static_cast<int>(sizeof(line)) - 4; ++i)
          n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %02x", part.d[i]);
        sp("%s\n", line);
      }
    }
  }
  pgusb::CheckResult r;
  const int errors = pgusb::checkConfiguration(cfg, cfgLen, g_usbRate, &r, descLine, nullptr, verbose);
  sp("[desc] self-check of the %u-byte configuration the host receives: %s (%d errors, %d warnings); "
     "AC itf %u, EP 0x%02x, max packet %u B, %lu Hz, %u ch x %u bit\n",
     static_cast<unsigned>(cfgLen), errors ? "FAIL" : "PASS", errors, r.warnings, r.audioItfAc, r.audioEpIn,
     r.audioMaxPacket, static_cast<unsigned long>(r.audioRate), r.audioChannels, r.audioBits);
  return errors;
}

void printUsbReport() {
  const usb::Stats u = usb::stats();
  sp("[usb] %s, audio %s, rate %lu Hz (host set %lu), stream opens %lu, serial port %s, "
     "descriptor self-check %s\n",
     u.started ? (u.mounted ? (u.suspended ? "SUSPENDED" : "ENUMERATED") : "NOT ENUMERATED") : "NOT STARTED",
     phaseName(u), static_cast<unsigned long>(u.rate), static_cast<unsigned long>(u.hostSetRate),
     static_cast<unsigned long>(u.streamOpens), u.cdcOpen ? "open" : "closed",
     g_usbDescErrors == 0 ? "PASS" : (g_usbDescErrors < 0 ? "n/a" : "FAIL"));
  sp("[usb] packets %lu (x%u %lu, x%u %lu, x%u %lu, other %lu), frames sent %lu pushed %lu | FIFO %u/%u B target %u | "
     "underruns (empty packets) %lu, overruns %lu frames, start discards %lu frames\n",
     static_cast<unsigned long>(u.packets), static_cast<unsigned>(u.rate / 1000u), static_cast<unsigned long>(u.pktNominal),
     static_cast<unsigned>(u.rate / 1000u + 1u), static_cast<unsigned long>(u.pktLarge),
     static_cast<unsigned>(u.rate / 1000u - 1u), static_cast<unsigned long>(u.pktSmall),
     static_cast<unsigned long>(u.pktOther), static_cast<unsigned long>(u.framesSent),
     static_cast<unsigned long>(u.framesPushed), u.fifoLevel, u.fifoDepth, u.fifoTarget,
     static_cast<unsigned long>(u.pktZero), static_cast<unsigned long>(u.overrunFrames),
     static_cast<unsigned long>(u.openingDiscards));
  char cs[300];
  if (sys::crashSummary(cs, sizeof(cs)) > 0) {
    sp("%s", cs);
    sp("[crash] decode: xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/cardputer-adv-usb/firmware.elf <the addresses>\n");
  } else {
    sp("[crash] no core dump in flash\n");
  }
}

void enterDownloadMode(const char* why) {
  sp("[usb] rebooting into the ROM download mode (%s): flash now with any env; power-cycle to leave\n", why);
  if (g_audioOk) audio::end();   // ramps the codec down: no click in the headphones
  app::drawMessage(g_fb, "DOWNLOAD MODE", "FLASH NOW FROM THE COMPUTER:", "PIO RUN -E <ENV> -T UPLOAD",
                   "POWER OFF AND ON TO LEAVE");
  sys::delayMs(100);
  usb::rebootToDownloadMode();
}

void onDownloadRequest(void*) { g_dlReq.store(true); }
#endif

void printReport() {
  const audio::Stats as = audio::stats();
  const sys::HeapInfo h = sys::heap();
  const keys::Stats ks = keys::stats();
  const disp::Stats ds = disp::stats();
  storage::refreshUsage();   // walks the LittleFS metadata: on request only, never per frame
  const storage::Info si = storage::info();
  sp("\n===== POCKET GROOVE REPORT %s =====\n", PG_VERSION);
  sp("[sys] uptime %lus, reset %s%s, board %s, %s %luMHz, PSRAM %s, IDF %s, arduino %s\n",
     static_cast<unsigned long>(sys::millis() / 1000), g_chip.resetReason, g_abnormal ? " (ABNORMAL)" : "",
     sys::bootInfo().boardName, g_chip.model, static_cast<unsigned long>(g_chip.cpuMhz),
     g_chip.psramFound ? "yes" : "no", g_chip.idfVersion, g_chip.coreVersion);
  sp("[ram] static DRAM %lu B (instrument %lu, ui %lu, fb %lu); heap total %lu\n",
     static_cast<unsigned long>(sys::staticDramBytes()), static_cast<unsigned long>(pg::Instrument::staticRamBytes()),
     static_cast<unsigned long>(sizeof(g_ui)), static_cast<unsigned long>(sizeof(g_fb)),
     static_cast<unsigned long>(h.totalBytes));
  sp("[ram] heap free %lu, min-ever %lu, largest %lu (boot %lu, after init %lu)\n",
     static_cast<unsigned long>(h.freeBytes), static_cast<unsigned long>(h.minEverFree),
     static_cast<unsigned long>(h.largestBlock), static_cast<unsigned long>(g_heapBoot.freeBytes),
     static_cast<unsigned long>(g_heapReady.freeBytes));
#if PG_USB_AUDIO
  sp("[ram] min-ever free during boot: begin %lu, storage %lu, usb %lu, menu+sd %lu, ui %lu, audio %lu; "
     "last runtime drop at t=%lus\n",
     static_cast<unsigned long>(g_minAt[0]), static_cast<unsigned long>(g_minAt[1]),
     static_cast<unsigned long>(g_minAt[2]), static_cast<unsigned long>(g_minAt[3]),
     static_cast<unsigned long>(g_minAt[4]), static_cast<unsigned long>(g_minAt[5]),
     static_cast<unsigned long>(g_minDropMs / 1000));
#endif
  sp("[stack] free min: audio %lu, keys %lu, loop %lu (of 6144 / 4096 / 12288)\n",
     static_cast<unsigned long>(as.stackFreeMin), static_cast<unsigned long>(keys::taskStackFree()),
     static_cast<unsigned long>(g_loopStackFree));
  char db[12];
  fmtDb(db, sizeof(db), g_volReg);
  sp("[audio] %s, %lu Hz, %s, core %d, codec %s, vol 0x%02X (%s dB)%s%s\n", g_audioOk ? "RUNNING" : "NOT RUNNING",
     static_cast<unsigned long>(sampleRate()), app::backendName(g_set.backend), as.taskCore,
     audio::codecPresent() ? "ACK" : "NO-ACK", g_volReg, db, g_audioErr[0] ? " err: " : "", g_audioErr);
  sp("[audio] block avg %dus max %dus (ever) recent %dus period %dus cpu %d.%d%% | blocks %lu underruns %lu%s late %lu wrErr %lu "
     "yields %lu maxGap %dus latency %lu fr\n",
     static_cast<int>(as.avgUs), static_cast<int>(as.maxUs), static_cast<int>(as.recentMaxUs),
     static_cast<int>(as.blockPeriodUs), tenths(as.cpuPercent) / 10, tenths(as.cpuPercent) % 10,
     static_cast<unsigned long>(as.blocks),
     static_cast<unsigned long>(as.underruns), as.underrunsValid ? "" : "(n/a)",
     static_cast<unsigned long>(as.lateBlocks), static_cast<unsigned long>(as.writeErrors),
     static_cast<unsigned long>(as.overloadYields), static_cast<int>(as.maxPeriodUs),
     static_cast<unsigned long>(audio::latencyFrames()));
  sp("[audio] overload warning %s\n", g_overloadWarn ? "ON (avg >90% of the block period for >1 s; clears after 5 s <70%)" : "no");
  {
    char pl[240];
    audio::placementLine(pl, sizeof(pl));
    sp("%s", pl);
  }
  {
    const plat::imu::Stats is = plat::imu::stats();
    sp("[imu] %s @0x%02X err 0x%02X, %s, reads %lu, bus errors %lu, 12-byte read avg %luus max %luus, "
       "stack free %lu of %lu (heap)\n",
       is.present ? "BMI270" : "NONE", is.addr, is.err, is.running ? "reading (G0 held)" : "asleep",
       static_cast<unsigned long>(is.reads), static_cast<unsigned long>(is.errors),
       static_cast<unsigned long>(is.busAvgUs), static_cast<unsigned long>(is.busMaxUs),
       static_cast<unsigned long>(is.stackFree), static_cast<unsigned long>(plat::imu::kTaskStack));
  }
  sp("[keys] controller %s, mode %s, events %lu, INT drains %lu, polled events %lu, drops %lu, fifo ovf %lu, "
     "i2c err %lu, INT->read max %luus\n",
     ks.controllerPresent ? "OK" : "MISSING", keyMode(), static_cast<unsigned long>(ks.events),
     static_cast<unsigned long>(ks.intLowDrains), static_cast<unsigned long>(ks.polledEvents),
     static_cast<unsigned long>(ks.queueDrops), static_cast<unsigned long>(ks.fifoOverflows),
     static_cast<unsigned long>(ks.i2cErrors), static_cast<unsigned long>(ks.maxIntToReadUs));
  sp("[ui] frame avg %dus max %dus, full redraws %lu, status redraws %lu, dropped cmds %lu, panel push avg %dus\n",
     static_cast<int>(g_frameUs), static_cast<int>(g_frameMaxUs), static_cast<unsigned long>(g_ui.fullRedraws()),
     static_cast<unsigned long>(g_ui.statusRedraws()), static_cast<unsigned long>(g_ui.droppedCommands()),
     static_cast<int>(ds.avgUs));
  sp("[fs] /int %s%s %lu/%lu B used (mount %lu ms); /sd %s %s (mount %lu ms, %lu MB, %lu MB used)\n",
     si.intMounted ? "mounted" : "NOT MOUNTED", si.intFormatted ? " (formatted this boot)" : "",
     static_cast<unsigned long>(si.intUsedBytes), static_cast<unsigned long>(si.intTotalBytes),
     static_cast<unsigned long>(si.intMountMs), storage::sdStateName(si.sd), si.sdType,
     static_cast<unsigned long>(si.sdMountMs), static_cast<unsigned long>(si.sdTotalBytes >> 20),
     static_cast<unsigned long>(si.sdUsedBytes >> 20));
  sp("[batt] %d%% %dmV, backlight %s\n", g_battPct, g_battMv, g_dimmed ? "dim" : "full");
  {
    const pg::io::Xfer::Stats& xs = g_xfer.stats();
    sp("[xfer] lines %lu, gets %lu, puts %lu, errors %lu, timeouts %lu, bad crc %lu, bad lines %lu, overflows %lu\n",
       static_cast<unsigned long>(xs.lines), static_cast<unsigned long>(xs.gets), static_cast<unsigned long>(xs.puts),
       static_cast<unsigned long>(xs.errors), static_cast<unsigned long>(xs.timeouts),
       static_cast<unsigned long>(xs.badCrc), static_cast<unsigned long>(xs.badLines),
       static_cast<unsigned long>(xs.overflows));
  }
#if PG_USB_AUDIO
  printUsbReport();
#endif
  sp("===== END REPORT =====\n");
}

void printStatsLine() {
  const audio::Stats as = audio::stats();
  const float winMaxUs = audio::takeMaxUs();   // max since the previous stats line, not since boot
  const sys::HeapInfo h = sys::heap();
#if PG_USB_AUDIO
  const usb::Stats u = usb::stats();
  sp("[stats] t=%lus cpu %d.%d%% avg %dus max %dus/%dus und %lu late %lu heap %luk min %luk key %s ui %dus%s | "
     "usb %s fifo %u und %lu ovr %lu\n",
     static_cast<unsigned long>(sys::millis() / 1000), tenths(as.cpuPercent) / 10, tenths(as.cpuPercent) % 10,
     static_cast<int>(as.avgUs), static_cast<int>(winMaxUs), static_cast<int>(as.blockPeriodUs),
     static_cast<unsigned long>(as.underruns), static_cast<unsigned long>(as.lateBlocks),
     static_cast<unsigned long>(h.freeBytes / 1024), static_cast<unsigned long>(h.minEverFree / 1024), keyMode(),
     static_cast<int>(g_frameUs), g_overloadWarn ? " OVERLOAD" : "", phaseName(u), u.fifoLevel,
     static_cast<unsigned long>(u.pktZero), static_cast<unsigned long>(u.overrunFrames));
#else
  sp("[stats] t=%lus cpu %d.%d%% avg %dus max %dus/%dus und %lu late %lu heap %luk min %luk key %s ui %dus%s\n",
     static_cast<unsigned long>(sys::millis() / 1000), tenths(as.cpuPercent) / 10, tenths(as.cpuPercent) % 10,
     static_cast<int>(as.avgUs), static_cast<int>(winMaxUs), static_cast<int>(as.blockPeriodUs),
     static_cast<unsigned long>(as.underruns), static_cast<unsigned long>(as.lateBlocks),
     static_cast<unsigned long>(h.freeBytes / 1024), static_cast<unsigned long>(h.minEverFree / 1024), keyMode(),
     static_cast<int>(g_frameUs), g_overloadWarn ? " OVERLOAD" : "");
#endif
}

struct SerialLines final : pg::io::XferOut {
  void send(const char* line, size_t len) override { sys::serialWrite(line, len); }
};
SerialLines g_xferOut;

void serviceXfer() {
  pg::io::XferEnv env;
  env.nowMs = sys::millis();
  env.playing = g_inst.engine().isPlaying();
  const pg::io::Xfer::Stats before = g_xfer.stats();
  g_xfer.service(env, g_xferOut);
  const pg::io::Xfer::Stats& after = g_xfer.stats();
  if (after.timeouts != before.timeouts) sp("[xfer] transfer abandoned by the host: timed out, nothing saved\n");
}

// Single-letter commands (at most 16 per frame, as before) and protocol lines
// (any number; replies go out at once).
void pollSerial() {
  const uint32_t t0 = sys::millis();
  int letters = 0;
  for (int n = 0; n < 4096; ++n) {
    const int ch = sys::serialRead();
    if (ch < 0) {
      if (g_xfer.busy() && sys::millis() - t0 < kXferSpinMs) {
        sys::delayMs(1);
        continue;
      }
      break;
    }
    if (g_xfer.rx(static_cast<uint8_t>(ch), sys::millis())) {
      if (g_xfer.lineReady()) serviceXfer();
      continue;
    }
#if PG_USB_AUDIO
    // Commands that restart the instrument need a confirmation within 3 s.
    if (g_pending && sys::millis() - g_pendingMs > 3000) g_pending = 0;
    if (g_pending && (ch == 'y' || ch == 'Y')) {
      const char what = g_pending;
      g_pending = 0;
      if (what == 'B') enterDownloadMode("serial B + y");
      sp("[boot] restarting into the boot menu\n");
      if (g_audioOk) audio::end();
      sys::restartIntoBootMenu();
    } else if (ch == 'B' || ch == 'm' || ch == 'M') {
      g_pending = (ch == 'B') ? 'B' : 'm';
      g_pendingMs = sys::millis();
      sp("[usb] %s: send y within 3 s to confirm (anything else cancels)\n",
         g_pending == 'B' ? "reboot into the ROM download mode (for flashing)" : "restart into the boot menu (unsaved changes are lost)");
    } else {
      if (g_pending && ch != '\r' && ch != '\n') {
        g_pending = 0;
        sp("[usb] cancelled\n");
      }
      if (ch == 'r' || ch == 'R') printReport();
      else if (ch == 'l' || ch == 'L') sys::bootLogReplay();
      else if (ch == 'u' || ch == 'U') checkUsbDescriptors(true, true);
      else if (ch == 'h' || ch == '?')
        sp("Pocket Groove %s (USB audio build) serial: r = full report, h = help, l = boot log, u = USB descriptors, "
           "m + y = boot menu, B + y = download mode. Stats line every 5 s. Project transfer: tools/pg on the computer.\n",
           PG_VERSION);
    }
#else
    if (ch == 'r' || ch == 'R') printReport();
    else if (ch == 'h' || ch == '?')
      sp("Pocket Groove %s serial: r = full report, h = help. Stats line every 5 s. "
         "Project transfer: tools/pg on the computer.\n", PG_VERSION);
#endif
    if (++letters >= 16) break;
  }
  serviceXfer();   // session and line timeouts
}

// ---- overlays ------------------------------------------------------------------------------
void updateOverlays(uint32_t now) {
  // volume bar
  app::OverlayBox& v = g_ov.box(app::kOvVolume);
  if (g_volShowUntil && static_cast<int32_t>(now - g_volShowUntil) >= 0) g_volShowUntil = 0;
  v.on = g_volShowUntil != 0;
  if (v.on) {
    v.x = 40; v.y = 52; v.w = 160; v.h = 24;
    v.bg = app::kRgbPanel; v.border = app::kRgbGrey;
    v.lines = 1;
    char db[12];
    fmtDb(db, sizeof(db), g_volReg);
    if (g_volReg == 0) snprintf(v.text[0], sizeof(v.text[0]), "CODEC VOLUME  MUTED");
    else snprintf(v.text[0], sizeof(v.text[0]), "CODEC VOLUME %s DB%s", db, g_audioOk ? "" : " (NO AUDIO)");
    v.fg[0] = g_volReg >= 0xB0 ? app::kRgbAmber : app::kRgbWhite;
    v.barPct = static_cast<int8_t>((static_cast<int>(g_volReg) * 100) / app::kVolMax);
    v.barY = 13; v.barH = 7;
    v.barFg = g_volReg >= 0xB0 ? app::kRgbAmber : app::kRgbGreen;
    v.barBg = app::kRgbDark;
  }

  // diagnostics strip (text refreshed 4x per second)
  app::OverlayBox& d = g_ov.box(app::kOvDiag);
  d.on = g_diagOn;
  if (g_diagOn && (g_lastDiagMs == 0 || now - g_lastDiagMs >= 250)) {
    g_lastDiagMs = now | 1u;
    const audio::Stats as = audio::stats();
    const sys::HeapInfo h = sys::heap();
#if PG_USB_AUDIO
    d.x = 0; d.y = pg::kScreenH - 35; d.w = pg::kScreenW; d.h = 35;
    d.bg = app::kRgbBlack; d.border = app::kRgbBlack;
    d.lines = 4;
    {
      // USB: state, FIFO level, underruns (empty packets) and overruns. Red
      // for 2 s after either counter moved, green while streaming cleanly.
      const usb::Stats u = usb::stats();
      const uint32_t bad = u.pktZero + u.overrunFrames;
      if (bad != g_usbPrevBad) { g_usbPrevBad = bad; g_usbBadMs = now | 1u; }
      const bool recent = g_usbBadMs && now - g_usbBadMs < 2000;
      snprintf(d.text[3], sizeof(d.text[3]), "USB %s %s F%u/%u U%lu O%lu", phaseName(u),
               g_usbRate == 32000u ? "32K" : "44.1K", u.fifoLevel, u.fifoDepth, static_cast<unsigned long>(u.pktZero),
               static_cast<unsigned long>(u.overrunFrames));
      d.fg[3] = recent ? app::kRgbRed
              : (!u.started || !u.mounted) ? app::kRgbAmber
              : (u.streaming && u.phase == usb::Phase::Running) ? app::kRgbGreen : app::kRgbCyan;
    }
#else
    d.x = 0; d.y = pg::kScreenH - 27; d.w = pg::kScreenW; d.h = 27;
    d.bg = app::kRgbBlack; d.border = app::kRgbBlack;
    d.lines = 3;
#endif
    d.barPct = -1;
    const int cpu = tenths(as.cpuPercent);
    if (g_audioOk) {
      snprintf(d.text[0], sizeof(d.text[0]), "AUD %d.%d%% %d/%d/%dus U%lu", cpu / 10, cpu % 10,
               static_cast<int>(as.avgUs), static_cast<int>(as.recentMaxUs), static_cast<int>(as.blockPeriodUs),
               static_cast<unsigned long>(as.underruns));
      d.fg[0] = cpu >= 900 ? app::kRgbRed : cpu >= 700 ? app::kRgbAmber : app::kRgbGreen;
    } else {
      snprintf(d.text[0], sizeof(d.text[0]), "AUDIO %s", g_set.backend == app::AudioBackend::Off ? "OFF" : "FAILED");
      d.fg[0] = app::kRgbRed;
    }
    snprintf(d.text[1], sizeof(d.text[1]), "HEAP %luK MIN %luK %s %s", static_cast<unsigned long>(h.freeBytes / 1024),
             static_cast<unsigned long>(h.minEverFree / 1024), g_set.rate == app::Rate::k32000 ? "32K" : "44.1K",
             g_set.backend == app::AudioBackend::M5Speaker ? "M5SPK" : "I2S");
    d.fg[1] = h.minEverFree < 16384 ? app::kRgbAmber : app::kRgbWhite;
    if (g_overloadWarn) {
      snprintf(d.text[2], sizeof(d.text[2]), g_set.rate == app::Rate::k44100 ? "CPU>90%%: USE 32KHZ (BOOT MENU)"
                                                                            : "CPU>90%%: AUDIO OVERLOADED");
      d.fg[2] = app::kRgbRed;
    } else {
      const int fu = static_cast<int>(g_frameUs / 100.0f);
      snprintf(d.text[2], sizeof(d.text[2]), "KEY %s  UI %d.%dMS", keyMode(), fu / 10, fu % 10);
      d.fg[2] = app::kRgbCyan;
    }
  }

  // one-line alert while the strip is hidden: no audio, or an unacknowledged overload
  app::OverlayBox& a = g_ov.box(app::kOvAlert);
  const bool overload = g_overloadWarn && !g_overloadAck;
  a.on = !g_diagOn && (!g_audioOk || overload);
  if (a.on) {
    a.x = 0; a.y = pg::kScreenH - 11; a.w = pg::kScreenW; a.h = 11;
    a.bg = app::kRgbDkRed; a.border = app::kRgbDkRed;
    a.lines = 1;
    a.barPct = -1;
    a.fg[0] = app::kRgbWhite;
    if (!g_audioOk && g_set.backend == app::AudioBackend::Off)
      snprintf(a.text[0], sizeof(a.text[0]), "AUDIO OFF - HOLD A KEY AT BOOT: MENU");
    else if (!g_audioOk)
      snprintf(a.text[0], sizeof(a.text[0]), "NO AUDIO: %.26s", g_audioErr[0] ? g_audioErr : "INIT FAILED");
    else
      snprintf(a.text[0], sizeof(a.text[0]), g_set.rate == app::Rate::k44100 ? "CPU>90%% - TRY 32KHZ  (G0 X2 = INFO)"
                                                                            : "CPU>90%% - OVERLOAD  (G0 X2 = INFO)");
  }
}

// Sends the changed rows: the UI's dirty band and the overlays' band, merged
// when they touch, as two partial pushes otherwise (status bar + bottom strip
// should not cost a full frame).
void pushFrame(bool uiChanged) {
  int ua = 0, uz = 0, oa = 0, oz = 0;
  if (uiChanged) {
    int y0 = 0, y1 = 0;
    g_ui.dirtyRows(&y0, &y1);
    ua = y0;
    uz = y1 + 1;
  }
  const bool ovChanged = g_ov.takeDirty(&oa, &oz);
  if (uiChanged && ovChanged && !(oz < ua || uz < oa)) {
    disp::presentRows(g_fb, ua < oa ? ua : oa, uz > oz ? uz : oz, app::Overlays::hook, &g_ov);
    return;
  }
  if (uiChanged) disp::presentRows(g_fb, ua, uz, app::Overlays::hook, &g_ov);
  if (ovChanged) disp::presentRows(g_fb, oa, oz, app::Overlays::hook, &g_ov);
}

// ---- boot ------------------------------------------------------------------------------------
void onFormat(void*) {
  sp("[boot] /int does not mount: formatting (first boot; can take ~5-30 s)\n");
  app::drawMessage(g_fb, "FORMATTING STORAGE", "FIRST BOOT: PREPARING INTERNAL FLASH.", "THIS TAKES UP TO ~30 S.",
                   "DO NOT POWER OFF.");
}

bool startAudio() {
  g_audioOk = false;
  g_audioErr[0] = 0;
  if (g_set.backend == app::AudioBackend::Off) {
    strlcpy(g_audioErr, "off (boot menu)", sizeof(g_audioErr));
    return false;
  }
  audio::Config cfg;
  cfg.sampleRate = sampleRate();
  cfg.backend = (g_set.backend == app::AudioBackend::M5Speaker) ? audio::Backend::M5Speaker : audio::Backend::DirectI2S;
  cfg.core = 0;
  cfg.dmaBufCount = 4;           // the smoke test's default: 5 blocks = 14.5 ms worst-case latency at 44.1 kHz
  cfg.volumeReg = g_volReg;
  g_audioOk = audio::begin(cfg, render, nullptr);
  strlcpy(g_audioErr, audio::lastError(), sizeof(g_audioErr));
  char db[12];
  fmtDb(db, sizeof(db), g_volReg);
  sp("[audio] %s %lu Hz, %s, vol 0x%02X (%s dB): %s%s%s, codec %s\n", app::backendName(g_set.backend),
     static_cast<unsigned long>(cfg.sampleRate), "core 0", g_volReg, db, g_audioOk ? "OK" : "FAILED",
     g_audioErr[0] ? " / " : "", g_audioErr, audio::codecPresent() ? "ACK" : "NO-ACK");
  return g_audioOk;
}

}  // namespace

void setup() {
  const sys::BootInfo boot = sys::begin(115200);
  g_heapBoot = sys::heap();
  sys::chipInfo(&g_chip);
  g_abnormal = sys::resetWasAbnormal();
  disp::begin();
  disp::setBrightness(kBrightFull);
  pg::ui::initPalette(g_fb);
  app::drawMessage(g_fb, "POCKET GROOVE", "STARTING...", nullptr, g_abnormal ? "LAST RUN CRASHED: BOOT MENU" : nullptr,
                   pg::ui::WHITE);

  sp("\n\n===== POCKET GROOVE %s (built %s %s) =====\n", PG_VERSION, __DATE__, __TIME__);
  sp("[boot] board %s (%d), %s, reset reason %s%s\n", boot.boardName, boot.boardId, g_chip.model, g_chip.resetReason,
     g_abnormal ? " -> ABNORMAL: boot menu" : "");
  sp("[boot] RAM: static DRAM %lu B (instrument %lu, ui %lu, framebuffer %lu), heap free %lu largest %lu total %lu\n",
     static_cast<unsigned long>(sys::staticDramBytes()), static_cast<unsigned long>(pg::Instrument::staticRamBytes()),
     static_cast<unsigned long>(sizeof(g_ui)), static_cast<unsigned long>(sizeof(g_fb)),
     static_cast<unsigned long>(g_heapBoot.freeBytes), static_cast<unsigned long>(g_heapBoot.largestBlock),
     static_cast<unsigned long>(g_heapBoot.totalBytes));

  {
    const disp::Backlight bl = disp::backlight();
    sp("[boot] backlight: LEDC %lu Hz (M5GFX set %lu Hz)%s, duty %lu/%lu%s\n",
       static_cast<unsigned long>(bl.reconfigured ? bl.pwmHz : bl.stockHz), static_cast<unsigned long>(bl.stockHz),
       bl.reconfigured ? "" : " RETUNE FAILED", static_cast<unsigned long>(bl.duty),
       static_cast<unsigned long>(bl.dutyFull), bl.dc ? " = DC" : "");
  }
  g_keysOk = keys::begin();
  sp("[boot] keyboard TCA8418: %s\n", g_keysOk ? "OK" : "NOT FOUND (G0 and serial still work)");
  g_imuOk = app::tilt::begin();
  {
    const plat::imu::Stats is = plat::imu::stats();
    sp("[boot] IMU: %s @0x%02X err 0x%02X (tilt: hold G0)\n", g_imuOk ? "BMI270" : "NOT FOUND (no tilt)", is.addr,
       is.err);
  }
  const uint32_t tKeys = sys::millis();

  const bool intOk = storage::beginInternal(onFormat, nullptr);
  sp("[boot] /int LittleFS: %s%s (%lu ms)\n", intOk ? "OK" : "FAILED", storage::info().intFormatted ? ", formatted" : "",
     static_cast<unsigned long>(storage::info().intMountMs));
  storage::sdMountAsync();
  pg::FileSystem& fs = storage::fs();
  const bool setOk = app::loadSettings(fs, &g_set);
  sp("[boot] firmware settings: %s (%lu Hz, %s, vol 0x%02X)\n", setOk ? "loaded" : "defaults",
     static_cast<unsigned long>(sampleRate()), app::backendName(g_set.backend), g_set.volReg);
#if PG_USB_AUDIO
  g_minAt[1] = sys::heap().minEverFree;
  // The USB stack, now that the rate for the audio descriptor is known. The
  // interrupt lands on this core (1), away from the audio task.
  g_usbRate = sampleRate();
  usb::onDownloadRequest(onDownloadRequest, nullptr);
  g_usbOk = usb::begin(g_usbRate);
  g_usbDescErrors = checkUsbDescriptors(false, false);
  sp("[boot] USB: %s, \"Pocket Groove\" stereo input %lu Hz + serial port (TinyUSB)\n", g_usbOk ? "started" : "FAILED",
     static_cast<unsigned long>(g_usbRate));
  {
    char cs[300];
    if (sys::crashSummary(cs, sizeof(cs)) > 0) sp("%s", cs);
  }
  g_minAt[2] = sys::heap().minEverFree;
#endif

  // Boot menu: a key held at power-on (give the controller ~300 ms to report
  // it), 'm' on serial, or automatically after a crash / watchdog / brownout.
  // The window is at least 60 ms even when the /int mount (a first-boot
  // format) already used up the 300 ms; a key still held at its end counts too.
  bool menu = g_abnormal;
#if PG_USB_AUDIO
  if (sys::bootMenuRequested()) menu = true;   // serial 'm' + 'y' before the restart
#endif
  const uint32_t tWin = sys::millis();
  while (sys::millis() - tKeys < 300 || sys::millis() - tWin < 60) {
    keys::Event e;
    if (keys::waitEvent(&e, 20) && e.ev.down && e.ev.key < pg::kNumKeys) menu = true;
    const int ch = sys::serialRead();
    if (ch == 'm' || ch == 'M') menu = true;
  }
  if (keys::heldMask() & kMatrixMask) menu = true;
  if (menu) {
    app::BootMenuEnv env{&g_fb, &fs, &g_set, g_chip.resetReason, g_abnormal, storage::info().intMounted};
    g_swallow = app::runBootMenu(env) & kMatrixMask;
  } else {
    g_swallow = keys::heldMask() & kMatrixMask;
  }

  g_volReg = g_set.volReg > app::kVolBootMax ? app::kVolBootMax : g_set.volReg;
  app::drawMessage(g_fb, "POCKET GROOVE", "LOADING SESSION...", nullptr, nullptr, pg::ui::WHITE);

  // Give the SD card a moment so the UI's boot can run power-loss recovery on it too.
  const uint32_t tSd = sys::millis();
  while (storage::sdState() == storage::SdState::Mounting && sys::millis() - tSd < 1500) sys::delayMs(10);
  sp("[boot] /sd: %s\n", storage::sdStateName(storage::sdState()));

#if PG_USB_AUDIO
  g_minAt[3] = sys::heap().minEverFree;
  if (sampleRate() != g_usbRate) {
    // The boot menu changed the rate: the USB descriptor carries the old one.
    sp("[boot] sample rate changed in the boot menu: restarting so that USB audio uses %lu Hz\n",
       static_cast<unsigned long>(sampleRate()));
    app::drawMessage(g_fb, "POCKET GROOVE", "RESTARTING FOR THE NEW", "SAMPLE RATE (USB AUDIO)", nullptr, pg::ui::WHITE);
    sys::delayMs(300);
    sys::restart();
  }
  // Stereo for USB; the jack gets (L+R)/2 in render().
  g_inst.init(static_cast<float>(sampleRate()), /*monoOutput=*/false);
#else
  g_inst.init(static_cast<float>(sampleRate()), /*monoOutput=*/true);
#endif
  g_ui.init(g_inst.engine(), g_inst.project(), &fs);
  g_ui.setSampler(&g_inst.sampler());
  g_xfer.init(&fs, PG_VERSION);
  g_sdRecovered = storage::sdState() == storage::SdState::Mounted;   // Ui::boot recovers mounted cards
  g_ui.boot(sys::millis());
  sp("[boot] UI up, project \"%s\"\n", g_ui.projectName());
#if PG_USB_AUDIO
  g_minAt[4] = sys::heap().minEverFree;
#endif

  // Audio last: the session is loaded, so the first block is already the real thing.
  startAudio();
  g_heapReady = sys::heap();
  sp("[boot] ready. heap free %lu (min-ever %lu, largest %lu). Serial: r = report.\n",
     static_cast<unsigned long>(g_heapReady.freeBytes), static_cast<unsigned long>(g_heapReady.minEverFree),
     static_cast<unsigned long>(g_heapReady.largestBlock));
  const sys::Battery b = sys::battery();
  g_battPct = b.percent;
  g_battMv = b.millivolts;
  g_lastKeyMs = sys::millis();
  g_lastBattMs = g_lastKeyMs;
  g_loopStackFree = sys::stackFreeMinSelf();
#if PG_USB_AUDIO
  g_minAt[0] = g_heapBoot.minEverFree;
  g_minAt[5] = g_heapReady.minEverFree;
  g_minSeen = g_heapReady.minEverFree;
  sys::bootLogEnd();
#endif
}

void loop() {
  const uint32_t frameStartMs = sys::millis();
  const uint64_t t0 = sys::micros();
  const uint32_t now = frameStartMs;

  const audio::Stats as = audio::stats();
  pg::ui::UiEnv env;
  env.nowMs = now;
  env.batteryPct = static_cast<uint8_t>(g_battPct < 0 ? 100 : g_battPct);
  env.sdPresent = storage::sdState() == storage::SdState::Mounted;
  if (env.sdPresent && !g_sdRecovered) {
    // The card finished mounting after Ui::boot (slow card): run the power-loss
    // recovery now, before the UI can list or save there. Otherwise a song
    // whose FAT replace (remove target, rename temp) was cut off would exist
    // only as _save.tmp, and the next save to the card would delete it.
    g_sdRecovered = true;
    const pg::io::Status rs = pg::io::recoverAfterPowerLoss(storage::fs(), pg::io::kMountSd);
    sp("[fs] /sd mounted late (%lu ms): power-loss recovery %s\n", static_cast<unsigned long>(storage::info().sdMountMs),
       pg::io::statusName(rs));
  }
  const float cpu = g_audioOk ? as.cpuPercent : 0.0f;
  env.cpuPct = static_cast<uint8_t>(cpu < 0.0f ? 0 : cpu > 100.0f ? 100 : static_cast<int>(cpu + 0.5f));
  const bool uiChanged = g_ui.update(env, g_fb);

  // Transport state: a side-effect-free read (status() would reset the meter's peak hold).
  g_playing = g_inst.engine().isPlaying();
  if (g_playing) g_lastPlayingMs = now;

  // Overload: moving-average load > 90% of the block period at the CURRENT
  // rate (cpuPercent is avg / (128 / rate)) for > 1 s -> warning; it clears
  // again after 5 s below 70%.
  if (g_audioOk && as.cpuPercent > 90.0f) {
    g_underSinceMs = 0;
    if (!g_overSinceMs) g_overSinceMs = now | 1u;
    else if (now - g_overSinceMs > 1000 && !g_overloadWarn) {
      g_overloadWarn = true;
      sp("[audio] OVERLOAD: >90%% for >1 s at %lu Hz.%s\n", static_cast<unsigned long>(sampleRate()),
         g_set.rate == app::Rate::k44100 ? " Consider 32 kHz (boot menu)." : "");
    }
  } else {
    g_overSinceMs = 0;
    if (g_overloadWarn && (!g_audioOk || as.cpuPercent < 70.0f)) {
      if (!g_underSinceMs) g_underSinceMs = now | 1u;
      else if (now - g_underSinceMs > 5000) {
        g_overloadWarn = false;
        g_overloadAck = false;
        g_underSinceMs = 0;
        sp("[audio] overload cleared\n");
      }
    } else {
      g_underSinceMs = 0;
    }
  }

  updateOverlays(now);
  pushFrame(uiChanged);

  const float us = static_cast<float>(sys::micros() - t0);
  g_frameUs = (g_frameUs == 0.0f) ? us : g_frameUs + (us - g_frameUs) * (1.0f / 16.0f);
  if (us > g_frameMaxUs) g_frameMaxUs = us;

  // Backlight: dim after 60 s without a key while stopped, never while the
  // transport runs or its FX tails can still be heard.
  if (!g_dimmed && !g_playing && now - g_lastKeyMs > kDimAfterMs && now - g_lastPlayingMs > kDimTailMs) {
    g_dimmed = true;
    disp::setBrightness(kBrightDim);
  } else if (g_dimmed && g_playing) {
    wake();
  }

  // Persist the codec volume once it has settled, and only while stopped
  // (a flash write stalls both cores; see storage.h).
  if (g_volDirty && !g_playing && now - g_volChangedMs > kVolSaveDelayMs) {
    g_volDirty = false;
    if (!app::saveSettings(storage::fs(), g_set)) sp("[fs] firmware settings NOT saved\n");
  }

  if (now - g_lastBattMs >= 5000) {
    g_lastBattMs = now;
    const sys::Battery b = sys::battery();
    g_battPct = b.percent;
    g_battMv = b.millivolts;
    g_loopStackFree = sys::stackFreeMinSelf();
#if PG_USB_AUDIO
    const uint32_t m = sys::heap().minEverFree;
    if (m < g_minSeen) {
      g_minSeen = m;
      g_minDropMs = now;
      sp("[ram] heap min-ever fell to %lu B at t=%lus\n", static_cast<unsigned long>(m),
         static_cast<unsigned long>(now / 1000));
    }
#endif
  }
#if PG_USB_AUDIO
  if (g_dlReq.load()) enterDownloadMode("1200-baud touch from the computer");
  {
    // A terminal just opened the port: show it what it missed, once per boot.
    const bool listening = sys::serialHostListening();
    if (listening && !g_hostWasListening && !g_bootLogReplayed) {
      g_bootLogReplayed = true;
      sys::bootLogReplay();
    }
    g_hostWasListening = listening;
  }
#endif
  if (now - g_lastStatsMs >= kStatsEveryMs) {
    g_lastStatsMs = now;
    if (sys::serialHostListening()) printStatsLine();
  }
  pollSerial();

  // Sleep until the next frame, handling keys the moment they arrive. Always
  // give up the CPU at least once per frame (core 1's idle task frees the
  // stacks of finished helper tasks).
  // While G0 tilts, wake every few ms to pass the newest IMU reading (50 Hz) to the
  // Ui, which posts it to the engine at once (no redraw: that waits for the frame).
  bool slept = false;
  for (;;) {
    if (g_ui.tiltHeld()) {
      float tx, ty;
      if (app::tilt::read(&tx, &ty)) g_ui.tiltInput(tx, ty, sys::millis());
    }
    const int32_t rem = static_cast<int32_t>(frameStartMs + kFrameMs - sys::millis());
    keys::Event e;
    if (rem <= 0) {
      for (int n = 0; n < 32 && keys::poll(&e); ++n) handleKey(e, sys::millis());
      if (!slept) sys::delayMs(1);
      break;
    }
    slept = true;
    const uint32_t wait = g_ui.tiltHeld() && rem > static_cast<int32_t>(kTiltSliceMs) ? kTiltSliceMs
                                                                                     : static_cast<uint32_t>(rem);
    if (keys::waitEvent(&e, wait)) handleKey(e, sys::millis());
  }
}
