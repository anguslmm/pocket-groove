// Pocket Groove on the Daisy Seed: the whole firmware above the hardware, as one
// portable object. The device (daisy/src/main.cpp, libDaisy) and the host
// simulation (host/daisy/daisy_sim.cpp, the tests) each implement Hal and call
//   boot()            once (again to simulate a reboot: everything restarts from
//                     the flash images)
//   loopOnce()        forever, from the main loop (no operating system: this is the
//                     cooperative scheduler, daisy/core/scheduler.h)
//   render()          from the audio callback (the interrupt): Instrument::process
//                     and the load meter, nothing else
// The same single-writer contract as the Cardputer and the desktop: the main loop
// changes the engine only through Engine::post (the UI does), and the audio
// interrupt is the only code that runs the engine.
//
// What it does (stage 0: a Seed and a headphone amp, all control over USB serial;
// docs/DAISY.md):
//   keys        from the computer (key_script.h: the desktop's script format, lines
//               starting '>'), and from the hardware when it exists (Hal::pollKey:
//               the TCA8418 on the breadboard, stage 1); G0 as on the Cardputer:
//               held = tilt (if there is an IMU), held + '-' / '=' = output level
//   transfer    tools/pg's protocol (pg::io::Xfer) on the same port: projects on /int
//               and /sd, samples in the pool
//   report      'r' prints the report (load now / peak, longest block, underruns,
//               memory map, board version), 'h' the commands, a stats line every 5 s
//   screen      'f' sends one frame, 'm' streams the changed rows (fb_stream.h);
//               the display when there is one (Hal::present, stage 1)
//   storage     /int in RAM with snapshots on the flash (ramfs.h, fs_persist.h); the
//               sample pool in RAM with a flash mirror (pool_mirror.h)
#pragma once
#include <cstddef>
#include <cstdint>

#include "../core/fb_stream.h"
#include "../core/fs_persist.h"
#include "../core/key_script.h"
#include "../core/load_meter.h"
#include "../core/led.h"
#include "../core/pool_mirror.h"
#include "../core/pots.h"
#include "../core/ramfs.h"
#include "../core/scheduler.h"
#include "../core/serial_link.h"
#include "pg/instrument/instrument.h"
#include "pg/io/xfer.h"
#include "pg/sampler/flash_pool.h"
#include "pg/ui/ui.h"

namespace pgd {

// Line output for reports (the device's memory map lines come from the Hal).
using LineFn = void (*)(void* user, const char* line);

class Hal {
 public:
  virtual uint32_t millis() = 0;
  virtual uint32_t micros() = 0;
  virtual uint32_t cycles() = 0;            // the audio meter's clock (cpuHz per second)
  virtual uint32_t cpuHz() = 0;

  // USB serial. write() never blocks: what does not fit is dropped (and counted).
  virtual void serialWrite(const char* p, size_t n) = 0;
  virtual size_t serialRoom() = 0;          // bytes write() would take now
  virtual int serialRead() = 0;             // a received byte, or -1
  virtual void serialFlush() {}             // hand queued bytes to the USB stack

  virtual void setLed(bool on) = 0;
  // 'B' + 'y': into the Daisy bootloader (DFU) for flashing; 'R' + 'y': a reset.
  virtual void restart(bool intoBootloader) = 0;

  // Output level (the codec's line out has no analog volume in libDaisy: digital).
  virtual void setOutputGain(float linear) = 0;
  virtual bool audioRunning() = 0;
  virtual uint32_t sampleRate() = 0;
  virtual uint32_t blockFrames() = 0;

  // Physical keys (stage 1: the TCA8418 matrix and G0). false: none waiting.
  virtual bool pollKey(pg::KeyEvent* /*e*/) { return false; }
  // The display (stage 1). false: none (present is then never called).
  virtual bool displayPresent() { return false; }
  virtual void present(const pg::Framebuffer& /*fb*/, int /*y0*/, int /*y1*/) {}
  // Tilt (stage 1, the BMI270): press with the two axes' responses, read the axes.
  virtual bool imuPresent() { return false; }
  virtual void tiltPress(int /*responseX*/, int /*responseY*/) {}
  virtual void tiltRelease() {}
  virtual bool tiltRead(float* /*x*/, float* /*y*/) { return false; }
  // The microphone (stage 1, MAX9814 into the codec's left input). nullptr: none.
  virtual pg::sampler::MicInput* mic() { return nullptr; }
  // /sd (stage 1, FatFS on the microSD card). nullptr: no card.
  virtual pg::FileSystem* sdCard() { return nullptr; }
  // The four pots (stage 0 on the breadboard): 0..1 each, < 0 = no pot. false: none.
  virtual bool readPots(float* /*out4*/) { return false; }
  virtual uint8_t batteryPercent() { return pg::ui::kBatteryUnknown; }
  virtual int batteryMillivolts() { return -1; }

  // The report's [sys] / [mem] / [board] lines the platform knows (board version,
  // boot type, memory regions, stack).
  virtual void reportLines(LineFn fn, void* user) { (void)fn; (void)user; }
  virtual const char* boardName() { return "?"; }
  virtual const char* stageName() { return "?"; }

  // Main loop housekeeping, once per pass (the device: the SD card's mount state).
  virtual void service(uint32_t /*nowMs*/) {}

 protected:
  ~Hal() = default;
};

// RAM and flash the App works in (the device: SDRAM, the QSPI regions).
struct AppMemory {
  uint8_t* fsArena = nullptr;              // RamFs::kMaxFiles * fsSlotBytes
  uint32_t fsSlotBytes = 0;
  uint8_t* fsStaging = nullptr;            // fsBankBytes - FsPersist::kHeaderSector
  uint32_t fsBankBytes = 0;
  pg::sampler::FlashDriver* fsFlash = nullptr;     // 2 * fsBankBytes
  uint8_t* poolImage = nullptr;            // pg::sampler::kPoolBytes
  uint8_t* poolStaging = nullptr;          // 4 KB
  pg::sampler::FlashDriver* poolFlash = nullptr;   // kPoolBytes
};

struct AppConfig {
  float sampleRate = 48000.0f;
  bool autoplay = false;        // stage 0: start the transport once booted (the demo plays)
  const char* version = "?";
};

class App {
 public:
  void boot(Hal& hal, const AppMemory& mem, const AppConfig& cfg);
  void loopOnce();
  // Audio interrupt: frames (a multiple of 16) of interleaved stereo int16.
  void render(int16_t* out, int frames);

  pg::Instrument& instrument() { return inst_; }
  pg::ui::Ui& ui() { return ui_; }
  pg::Framebuffer& framebuffer() { return fb_; }
  LoadMeter& meter() { return meter_; }
  RamFs& intFs() { return ramfs_; }
  FsPersist& persist() { return persist_; }
  MirrorFlash& poolMirror() { return mirror_; }
  const pg::sampler::FlashPool& pool() const { return pool_; }
  const KeyScript& keyScript() const { return script_; }
  bool booted() const { return booted_; }
  LedMode ledMode() const { return led_; }
  void setRomMissing(bool m) { romMissing_ = m; }
  float outputGainDb() const { return gainDb_; }
  bool mirroring() const { return mirror_on_; }
  PotMacros& pots() { return pots_; }

  // printf to the serial port (and the boot log while booting).
  void sp(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void printReport();
  void printStatsLine();
  void printHelp();

 private:
  static void tSerial(void* u, uint32_t now) { static_cast<App*>(u)->serviceSerial(now); }
  static void tKeys(void* u, uint32_t now) { static_cast<App*>(u)->serviceKeys(now); }
  static void tUi(void* u, uint32_t now) { static_cast<App*>(u)->serviceUi(now); }
  static void tScreen(void* u, uint32_t now) { static_cast<App*>(u)->serviceScreen(now); }
  static void tStore(void* u, uint32_t now) { static_cast<App*>(u)->serviceStore(now); }
  static void tLed(void* u, uint32_t now) { static_cast<App*>(u)->serviceLed(now); }
  static void tStats(void* u, uint32_t now) { static_cast<App*>(u)->serviceStats(now); }
  static void tPots(void* u, uint32_t now) { static_cast<App*>(u)->servicePots(now); }
  static uint32_t msClock(void* u) { return static_cast<App*>(u)->hal_->millis(); }
  static uint32_t usClock(void* u) { return static_cast<App*>(u)->hal_->micros(); }

  void serviceSerial(uint32_t now);
  void serviceKeys(uint32_t now);
  void serviceUi(uint32_t now);
  void serviceScreen(uint32_t now);
  void serviceStore(uint32_t now);
  void serviceLed(uint32_t now);
  void serviceStats(uint32_t now);
  void servicePots(uint32_t now);
  void serviceXfer(uint32_t now);
  void letter(char c, uint32_t now);
  void scriptLine(const char* line, uint32_t now);
  void handleKey(const pg::KeyEvent& e, uint32_t now);
  static void emitKey(void* u, const pg::KeyEvent& e) {
    App* a = static_cast<App*>(u);
    ++a->injected_;
    a->handleKey(e, a->hal_->millis());
  }
  void setGainDb(float db);
  void writeRaw(const char* p, size_t n);

  Hal* hal_ = nullptr;
  AppMemory mem_{};
  AppConfig cfg_{};
  bool booted_ = false;

  pg::Instrument inst_;
  pg::ui::Ui ui_;
  pg::Framebuffer fb_;
  pg::io::Xfer xfer_;
  RamFs ramfs_;
  MountFs fs_;
  FsPersist persist_;
  MirrorFlash mirror_;
  pg::sampler::FlashPool pool_;
  pg::sampler::SampleWriter writer_;
  SerialLink link_;
  KeyScript script_;
  FbStream fbs_;
  Scheduler sched_;
  LoadMeter meter_;
  PotMacros pots_;

  // state
  LedMode led_ = LedMode::Booting;
  bool romMissing_ = false;
  bool mirror_on_ = false;
  bool fullFrameWanted_ = false;
  int dirtyY0_ = 0, dirtyY1_ = 0;   // rows changed since the last mirrored frame
  char shotName_[24] = {};
  bool g0Held_ = false, g0UsedForGain_ = false;
  uint64_t swallow_ = 0;
  float gainDb_ = 0.0f;
  char pending_ = 0;                // 'B' or 'R' waiting for 'y'
  uint32_t pendingMs_ = 0;
  uint32_t injected_ = 0, physical_ = 0;
  uint32_t frameUs_ = 0, frameMaxUs_ = 0;
  uint32_t lastReportMs_ = 0;
  uint32_t samplesChanged_ = 0;
  FsPersist::Load fsLoad_ = FsPersist::Load::Empty;
  bool poolLoaded_ = false;
  int poolDropped_ = 0;
  uint32_t bootMs_ = 0;

  // The boot log, replayed by 'l' (the computer usually opens the port after boot).
  static constexpr size_t kBootLog = 3072;
  char bootLog_[kBootLog] = {};
  size_t bootLogLen_ = 0;
  bool logging_ = false;
};

}  // namespace pgd
