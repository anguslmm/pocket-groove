// Pocket Groove on the Daisy Seed: the whole firmware above the hardware, as one
// portable object. The device (daisy/src/main.cpp, libDaisy) and the host
// simulation (host/daisy/daisy_sim.cpp, the tests) each implement Hal and call
//   boot()            once (again to simulate a reboot: everything restarts from
//                     the flash images)
//   loopOnce()        forever, from the main loop (no operating system: this is the
//                     cooperative scheduler, daisy/core/scheduler.h)
//   render()          from the audio callback (the interrupt): the pads' scan, the
//                     line input into its ring, Instrument::processFloat, the output
//                     level and the load meter, nothing else
// The same single-writer contract as the Cardputer and the desktop: the main loop
// changes the engine only through Engine::post (the UI does), and the audio
// interrupt is the only code that runs the engine.
//
// What it does (stage 0: a Seed and a headphone amp, all control over USB serial;
// docs/DAISY.md):
//   keys        from the computer (key_script.h: the desktop's script format, lines
//               starting '>'), and from the hardware when it exists (Hal::pollKey:
//               G0 on the breadboard, stage 1); G0 as on the Cardputer:
//               held = tilt (if there is an IMU), held + '-' / '=' = output level
//   pads        four force sensors (pads.h), scanned in render(), into the core's
//               pad API (pg/ui/pads.h: velocity, pressure, the drums / notes job;
//               App::padEvent); also from the computer (">hit N VEL ..."), the same way
//   touch       the 2090's FT6206 (Hal::pollPointer): reported, mirrored as ~PGT
//   recording   hold L on a sampler page: the codec's line in (line_mic.h)
//   transfer    tools/pg's protocol (pg::io::Xfer) on the same port: projects on /int
//               and /sd, samples in the pool
//   report      'r' prints the report (load now / peak, longest block, underruns,
//               memory map, board version), 'h' the commands, a stats line every 5 s
//   screen      'f' sends one frame, 'm' streams the changed rows (fb_stream.h);
//               the display when there is one (Hal::present, stage 1)
//   storage     /int in RAM with snapshots on the flash (ramfs.h, fs_persist.h); the
//               sample pool in RAM with a flash mirror (pool_mirror.h)
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../core/fb_stream.h"
#include "../core/fs_persist.h"
#include "../core/key_script.h"
#include "../core/load_meter.h"
#include "../core/led.h"
#include "../core/line_mic.h"
#include "../core/pads.h"
#include "../core/pool_mirror.h"
#include "../core/pots.h"
#include "../core/settings.h"
#include "../core/ramfs.h"
#include "../core/scheduler.h"
#include "../core/serial_link.h"
#include "bench.h"
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

  virtual bool audioRunning() = 0;
  virtual uint32_t sampleRate() = 0;
  virtual uint32_t blockFrames() = 0;

  // Physical keys (stage 1: G0; the key layout is undecided). false: none waiting.
  virtual bool pollKey(pg::KeyEvent* /*e*/) { return false; }
  // Touch / pointer input (stage 1: the 2090's FT6206). The UI does not take pointer
  // events yet: the App counts them, reports them ([touch]) and sends them to the
  // screen mirror (~PGT lines). false: none.
  struct Pointer {
    int16_t x, y;        // in the UI's 240x135 canvas (the platform undoes the panel's
                         // rotation, offset and scale); outside it on the border
    int16_t rawX, rawY;  // the controller's own coordinates (native, portrait)
    bool down;           // false: the touch ended (x, y: where it was last)
    uint32_t timeMs;
  };
  virtual bool pollPointer(Pointer* /*p*/) { return false; }
  struct TouchInfo {
    const char* state;   // "present", "NOT FOUND at 0x38", ...
    bool present;
    uint8_t chipId, firmware;
    uint32_t reads, errors;
    int irq;             // the IRQ pin's level (0 = low: touched), -1 not wired / unknown
    uint8_t orientation; // the TouchMap in use (touch.h)
  };
  virtual bool touchInfo(TouchInfo* /*t*/) { return false; }
  // The force-sensor pads (stage 1). CALLED FROM THE AUDIO INTERRUPT (App::render),
  // once per block: the four ADC readings, 0..1. false: no pads.
  virtual bool readPads(float* /*out4*/) { return false; }
  // The codec's line input reaches render() (the recording path). false: none.
  virtual bool lineInput() { return false; }
  // The Daisy's settings changed (or were loaded at boot): the display's orientation
  // and SPI speed, the touch mapping. The pads' calibration is the App's own.
  virtual void applySettings(const DaisySettings& /*s*/) {}
  // The display (stage 1). false: none (present is then never called).
  virtual bool displayPresent() { return false; }
  virtual void present(const pg::Framebuffer& /*fb*/, int /*y0*/, int /*y1*/) {}
  // Tilt (stage 1, the BMI270): press with the two axes' responses, read the axes.
  virtual bool imuPresent() { return false; }
  virtual void tiltPress(int /*responseX*/, int /*responseY*/) {}
  virtual void tiltRelease() {}
  virtual bool tiltRead(float* /*x*/, float* /*y*/) { return false; }
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

  // CALLED FROM THE AUDIO INTERRUPT when a block is noteworthy or for every block's
  // record (cheap reads only): LoadMeter flags of what the platform has under way
  // (kFlagQspi, kFlagUsbDown, kFlagSerialRx).
  virtual uint16_t contextFlags() { return 0; }
  // The bench (bench.h): empty both caches (audio interrupt, outside the timed span),
  // and the lines that say what determines speed (build, caches, where things are).
  virtual void flushCaches() {}
  virtual void benchInfo(LineFn fn, void* user) { (void)fn; (void)user; }

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
  int16_t* lineRing = nullptr;             // the line input's ring (a power of two samples)
  uint32_t lineRingSamples = 0;
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
  // Audio interrupt: frames (a multiple of 16) of interleaved stereo float for the
  // codec, +-1.0 full scale: the engine's full-resolution output
  // (Instrument::processFloat) times the output level, clamped to +-1.0 (only a level
  // above 0 dB can reach it: the engine's limiter keeps its own output within +-1).
  // No 16-bit step on the way: the codec's 24 bits get the mix as the master made
  // it. in: the codec's input block (interleaved stereo float), nullptr when there
  // is none. Also scans the pads (Hal::readPads), before the engine runs.
  void render(float* out, int frames, const float* in = nullptr);

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
  // The output level (the codec's line out has no analog volume in libDaisy, so it is
  // digital: render() applies it in float). -40 .. +6 dB; G0 + '-' / '=' steps it.
  void setOutputGainDb(float db);
  bool mirroring() const { return mirror_on_; }
  PotMacros& pots() { return pots_; }
  PadScanner& pads() { return pads_; }
  LineMic& lineMic() { return lineMic_; }
  const DaisySettings& settings() const { return settings_; }
  uint32_t padNotes() const { return padNotes_; }
  uint32_t padUps() const { return padUps_; }
  uint32_t padPressures() const { return padPressures_; }   // handed to the UI (after merging)
  uint32_t pointerEvents() const { return pointerEvents_; }
  // The largest |sample| of the output (after the output level) since the last report
  // (or the last call), on a 0..32767 scale.
  uint32_t takeOutputPeak() { return outPeak_.exchange(0, std::memory_order_relaxed); }

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
  static void tPads(void* u, uint32_t now) { static_cast<App*>(u)->servicePads(now); }
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
  void servicePads(uint32_t now);
  void padEvent(const PadEvent& e);
  bool hitCommand(char** w, int n);
  void printPadPlay();
  void servicePointer(uint32_t now);
  void pointer(const Hal::Pointer& p);
  bool settingsCommand(const char* line);
  void setPots(bool on);   // the pots on or off, saved (DaisySettings::potsOn)
  void printPots();
  bool benchCommand(char** w, int n);
  void serviceBench();
  void printBench();
  void printAudioEvents();
  void describeEvent(const LoadMeter::Event& e, char* out, size_t cap) const;
  void saveSettings();
  void printPads(bool withCal);
  void printTouch();
  void printLineIn();
  void serviceXfer(uint32_t now);
  void letter(char c, uint32_t now);
  void scriptLine(const char* line, uint32_t now);
  void handleKey(const pg::KeyEvent& e, uint32_t now);
  static void emitKey(void* u, const pg::KeyEvent& e) {
    App* a = static_cast<App*>(u);
    ++a->injected_;
    a->handleKey(e, a->hal_->millis());
  }
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
  PadScanner pads_;
  LineMic lineMic_;
  DaisySettings settings_;

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
  volatile float gain_ = 1.0f;        // render()'s output level (linear), from setOutputGainDb
  char pending_ = 0;                // 'B' or 'R' waiting for 'y'
  uint32_t pendingMs_ = 0;
  uint32_t injected_ = 0, physical_ = 0;
  uint32_t serialHeld_ = 0;          // passes that left serial input waiting (key queue nearly full)
  uint32_t frameUs_ = 0, frameMaxUs_ = 0;
  uint32_t lastReportMs_ = 0;
  uint32_t samplesChanged_ = 0;
  FsPersist::Load fsLoad_ = FsPersist::Load::Empty;
  bool poolLoaded_ = false;
  int poolDropped_ = 0;
  uint32_t bootMs_ = 0;
  // pads, touch
  bool padsOn_ = false;               // the Hal has pads (render scans them)
  volatile bool padsSeen_ = false;    // readPads answered (set in the interrupt)
  uint32_t padsWatchMs_ = 0;          // >pads watch: a [pads] line this often (0: off)
  uint32_t padsWatchLast_ = 0;
  uint32_t padNotes_ = 0, padUps_ = 0, padPressures_ = 0;
  uint32_t padPressuresIn_ = 0;       // pressure events popped (before merging)
  // Pressure waiting to go to the UI: the newest per pad, at most one per pad every
  // pg::ui::kPressEveryMs (servicePads).
  static constexpr int kAppPads = pg::ui::kMaxPads > 0 ? pg::ui::kMaxPads : 1;
  float pendPress_[kAppPads] = {};
  uint32_t pendMask_ = 0;
  uint32_t pressMs_[kAppPads] = {};
  // >hit: pads hit from the computer, released at upAt (0: held until ">hit N up")
  struct InjPad {
    bool down;
    uint32_t upAt;
  };
  InjPad inj_[kAppPads] = {};
  uint32_t padInjected_ = 0;
  int8_t lastPad_ = -1;               // the last pad hit, its velocity (the report)
  float lastPadVel_ = 0.0f;
  uint32_t pointerEvents_ = 0, pointerDowns_ = 0;
  Hal::Pointer lastPointer_{};
  bool pointerSeen_ = false;
  int16_t injX_ = 0, injY_ = 0;       // >touch X Y [ms]: an injected touch, lifted at injUpMs_
  uint32_t injUpMs_ = 0;
  bool injDown_ = false;
  const char* settingsLoad_ = "defaults";
  bool lineIn_ = false;               // the codec's input reaches render() (Hal::lineInput)
  // the load meter's context (render() reads these; the main loop writes them)
  volatile uint32_t lastMacroMoveMs_ = 0, lastKeyMs_ = 0;
  bool macroMoved_ = false, keySeen_ = false;
  bool wasPlaying_ = false;
  uint32_t playStartBlock_ = 0xFFFFFFFFu;
  // the bench (">bench", bench.h)
  Bench bench_;
  bool benchQuick_ = false, benchRestart_ = true;
  uint32_t benchLiveAud_ = 0;          // AUD now (permille) when the bench started
  uint32_t benchStartMs_ = 0;
  uint32_t benchDoneMs_ = 0;           // the table printed: restart after the serial drains
  // the UI frame's longest: what it did
  uint32_t frameMaxFsWrites_ = 0;
  bool frameMaxFull_ = false;
  std::atomic<uint32_t> outPeak_{0};  // render(): the output's largest |sample| (the report)

  // The boot log, replayed by 'l' (the computer usually opens the port after boot).
  static constexpr size_t kBootLog = 3072;
  char bootLog_[kBootLog] = {};
  size_t bootLogLen_ = 0;
  bool logging_ = false;
};

}  // namespace pgd
