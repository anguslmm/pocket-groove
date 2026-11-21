// Pocket Groove on the Electrosmith Daisy Seed (STM32H750, libDaisy; docs/DAISY.md).
//
// No operating system. Two contexts:
//   the audio interrupt   libDaisy's SAI DMA callback, every 96 frames (2 ms at
//                         48 kHz): App::render (the engine's float output with the
//                         output level, straight into libDaisy's buffer; libDaisy
//                         converts it to the codec's 24 bits). Nothing else runs the
//                         engine.
//   the main loop         App::loopOnce, forever: the scheduler's tasks (USB serial,
//                         keys, the UI frame, the screen, the flash writes, the LED,
//                         the pots). It changes the engine only through Engine::post.
// The USB receive callback (an interrupt) only fills a ring buffer.
//
// Stages (make STAGE=0|1, daisy/Makefile):
//   0  a Seed (and a headphone amp, or the Erica Synths Labor's output amplifier):
//      line out, control over USB serial (tools/pg-daisy), four pots on A0..A3.
//      The transport starts by itself once booted: the demo plays.
//   1  the breadboard prototype: also the Adafruit 2090 display (ILI9341) and its
//      FT6206 touch, G0, the BMI270, the microSD card, four force-sensor pads on
//      A5..A8, the battery (daisy/src/stage1.cpp; compile-checked, nothing has run).
// Both stages record from the codec's line input (hold L on a sampler page).
#include <cstdio>
#include <cstring>

#include "../app/app.h"
#include "../core/layout.h"
#include "../core/pins.h"
#include "bench_info.h"
#include "clock.h"
#include "daisy_seed.h"
#include "memory.h"
#include "qspi_flash.h"
#include "stage1.h"
#include "usb_serial.h"

#ifndef PG_DAISY_STAGE
#define PG_DAISY_STAGE 0
#endif
#ifndef PG_VERSION
#define PG_VERSION "daisy-dev"
#endif

daisy::DaisySeed g_hw;

namespace {

constexpr uint32_t kBlock = 96;   // frames per callback: 6 engine quanta, 2.0 ms at 48 kHz
constexpr uint32_t kRate = 48000;

// ---- placement (daisy/pg_daisy.lds) ----------------------------------------------------
// The App (instrument ~70 KB, UI, framebuffer, transfer, file table) in D2 SRAM; the
// engine's own state block is in DTCM (.bss). Constructed before main(), after
// memory.cpp's preinit has zeroed .d2_bss.
__attribute__((section(".d2_bss"))) pgd::App g_app;
// SDRAM: never constructed by the startup (SDRAM works only after DaisySeed::Init):
// plain bytes, set up by the code that uses them.
DSY_SDRAM_BSS uint8_t g_fsArena[pgd::RamFs::kMaxFiles * pgd::layout::kFsSlotBytes];
DSY_SDRAM_BSS uint8_t g_fsStaging[pgd::layout::kFsBankBytes];
DSY_SDRAM_BSS uint8_t g_poolImage[pgd::layout::kPoolBytes];
DSY_SDRAM_BSS uint8_t g_poolStaging[4096];
DSY_SDRAM_BSS uint8_t g_txRing[pgd::layout::kTxRing];
DSY_SDRAM_BSS uint8_t g_rxRing[pgd::layout::kRxRing];
// The line input's ring (line_mic.h): 65536 samples, 1.37 s at 48 kHz.
constexpr uint32_t kLineRing = 65536;
DSY_SDRAM_BSS int16_t g_lineRing[kLineRing];
volatile bool g_audioOn = false;
// FPSCR as the audio interrupt finds it (the report's [fpu] line), and how many
// callbacks found flush-to-zero / default-NaN off and set them.
constexpr uint32_t kFpscrFzDn = (1u << 24) | (1u << 25);
volatile uint32_t g_isrFpscr = 0;
volatile uint32_t g_isrFpscrFixes = 0;

pgdev::QspiRegion g_fsFlash, g_poolFlash;
daisy::DaisySeed::BoardVersion g_board = daisy::DaisySeed::BoardVersion::DAISY_SEED;
daisy::AdcChannelConfig g_adcCfg[9];
bool g_adcOn = false;
int g_adcPads = -1;   // the first pad's ADC channel (stage 1), -1: none
char g_boardName[96] = "?";
pgdev::memory::Fault g_fault = {};

const char* boardVersionName(daisy::DaisySeed::BoardVersion v) {
  switch (v) {
    case daisy::DaisySeed::BoardVersion::DAISY_SEED: return "Daisy Seed rev4 (AK4556 codec)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_1_1: return "Daisy Seed 1.1 (WM8731 codec)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_2_DFM: return "Daisy Seed2 DFM (PCM3060 codec)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_1_2: return "Daisy Seed 1.2 (PCM3060 codec)";
    case daisy::DaisySeed::BoardVersion::DAISY_SEED_3: return "Daisy Seed3 (TAC5242 codec)";
  }
  return "unknown Daisy board";
}

const char* bootloaderName() {
  switch (daisy::System::GetBootloaderVersion()) {
    case daisy::System::BootInfo::Version::LT_v6_0: return "older than v6.0";
    case daisy::System::BootInfo::Version::NONE: return "none";
    case daisy::System::BootInfo::Version::v6_0: return "v6.0";
    case daisy::System::BootInfo::Version::v6_1: return "v6.1 or later";
    default: return "?";
  }
}

const char* regionName(daisy::System::MemoryRegion r) {
  switch (r) {
    case daisy::System::INTERNAL_FLASH: return "internal flash (NOT the Daisy bootloader: wrong build?)";
    case daisy::System::SRAM_D1: return "AXI SRAM (BOOT_SRAM, as intended)";
    case daisy::System::QSPI: return "QSPI (BOOT_QSPI: not this build's layout)";
    default: return "an unexpected region";
  }
}

void audioCallback(daisy::AudioHandle::InterleavingInputBuffer in, daisy::AudioHandle::InterleavingOutputBuffer out,
                   size_t size) {
  // Flush-to-zero and default-NaN in this context too (memory.cpp sets them for the
  // main loop and in FPDSCR, which the core copies into an interrupt's FPSCR; this
  // checks that it did).
  const uint32_t fpscr = __get_FPSCR();
  g_isrFpscr = fpscr;
  if ((fpscr & kFpscrFzDn) != kFpscrFzDn) {
    __set_FPSCR(fpscr | kFpscrFzDn);
    g_isrFpscrFixes = g_isrFpscrFixes + 1;
  }
  (void)pgdev::clock::micros();   // keeps the microsecond clock's count fresh (clock.h)
  const int frames = static_cast<int>(size / 2);
  if (frames > static_cast<int>(kBlock) || frames % 16) {   // not a block the engine renders: silence
    for (size_t i = 0; i < size; ++i) out[i] = 0.0f;
    return;
  }
  // The pads' scan, the line input, the engine (float, full resolution) and the
  // output level, into libDaisy's float buffer; its f2s24 (x 2^23, truncated) is the
  // only conversion to the codec's format.
  g_app.render(out, frames, in);
}

uint32_t uptimeMs() { return daisy::System::GetNow(); }

struct DaisyHal final : pgd::Hal {
  uint32_t lastRx_ = 0;
  uint32_t millis() override { return daisy::System::GetNow(); }
  uint32_t micros() override { return pgdev::clock::micros(); }   // not GetUs: clock.h
  uint32_t cycles() override { return DWT->CYCCNT; }
  uint32_t cpuHz() override { return HAL_RCC_GetSysClockFreq(); }
  void serialWrite(const char* p, size_t n) override { pgdev::usbserial::write(p, n); }
  size_t serialRoom() override { return pgdev::usbserial::room(); }
  int serialRead() override { return pgdev::usbserial::read(); }
  void serialFlush() override { pgdev::usbserial::flush(); }
  void setLed(bool on) override { g_hw.SetLed(on); }
  void restart(bool intoBootloader) override {
    if (g_audioOn) g_hw.StopAudio();
    daisy::System::Delay(50);
    if (intoBootloader) daisy::System::ResetToBootloader(daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
    NVIC_SystemReset();
  }
  bool audioRunning() override { return g_audioOn; }
  uint32_t sampleRate() override { return kRate; }
  uint32_t blockFrames() override { return kBlock; }
  bool readPots(float* out) override {
    if (!g_adcOn) return false;
    for (int i = 0; i < 4; ++i) out[i] = g_hw.adc.GetFloat(static_cast<uint8_t>(i));
    return true;
  }
  int batteryMillivolts() override {
    if (!g_adcOn || PG_DAISY_STAGE < 1) return -1;
    return static_cast<int>(g_hw.adc.GetFloat(4) * 3300.0f * 2.0f);   // the 1:2 divider
  }
  uint8_t batteryPercent() override {
    const int mv = batteryMillivolts();
    if (mv < 2500) return pg::ui::kBatteryUnknown;   // no battery (USB power)
    const int pct = (mv - 3300) * 100 / (4150 - 3300);
    return static_cast<uint8_t>(pct < 0 ? 0 : pct > 100 ? 100 : pct);
  }
  // The audio interrupt: the DMA buffer's latest value per pad (the ADC converts
  // continuously, every channel ~0.5 ms; docs/DAISY.md "Force-sensor pads").
  bool readPads(float* out) override {
    if (!g_adcOn || g_adcPads < 0) return false;
    for (int i = 0; i < 4; ++i) out[i] = g_hw.adc.GetFloat(static_cast<uint8_t>(g_adcPads + i));
    return true;
  }
  bool lineInput() override { return true; }   // the codec's input, on every Seed
  bool pollPointer(Pointer* p) override { return pgdev::stage1::pollPointer(p); }
  bool touchInfo(TouchInfo* t) override { return pgdev::stage1::touchInfo(t); }
  void applySettings(const pgd::DaisySettings& s) override { pgdev::stage1::applySettings(s); }
  bool pollKey(pg::KeyEvent* e) override { return pgdev::stage1::pollKey(e); }
  bool displayPresent() override { return pgdev::stage1::displayPresent(); }
  void present(const pg::Framebuffer& fb, int y0, int y1) override { pgdev::stage1::present(fb, y0, y1); }
  bool imuPresent() override { return pgdev::stage1::imuPresent(); }
  void tiltPress(int rx, int ry) override { pgdev::stage1::tiltPress(rx, ry); }
  void tiltRelease() override { pgdev::stage1::tiltRelease(); }
  bool tiltRead(float* x, float* y) override { return pgdev::stage1::tiltRead(x, y); }
  pg::FileSystem* sdCard() override { return pgdev::stage1::sdCard(); }
  const char* boardName() override { return g_boardName; }
  const char* stageName() override {
    return PG_DAISY_STAGE == 0 ? "stage 0 (Seed + line out, USB control, pots)" : "stage 1 (breadboard prototype)";
  }
  void service(uint32_t nowMs) override { pgdev::stage1::service(nowMs); }
  uint16_t contextFlags() override {   // the audio interrupt: cheap reads
    uint16_t f = 0;
    if (pgdev::qspiBusy()) f |= pgd::LoadMeter::kFlagQspi;
    if (!pgdev::usbserial::configured()) f |= pgd::LoadMeter::kFlagUsbDown;
    const uint32_t rx = pgdev::usbserial::rxCount();
    if (rx != lastRx_) f |= pgd::LoadMeter::kFlagSerialRx;
    lastRx_ = rx;
    return f;
  }
  void flushCaches() override {   // the bench's cold pass (audio interrupt)
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
  }
  void benchInfo(pgd::LineFn fn, void* user) override { pgdev::benchInfo(fn, user, &g_app.instrument()); }
  void reportLines(pgd::LineFn fn, void* user) override {
    char l[400];
    std::snprintf(l, sizeof(l), "[board] %s; running from %s; Daisy bootloader %s; CPU %lu MHz; libDaisy %s",
                  g_boardName, regionName(daisy::System::GetProgramMemoryRegion()), bootloaderName(),
                  static_cast<unsigned long>(HAL_RCC_GetSysClockFreq() / 1000000u), PG_LIBDAISY_COMMIT);
    fn(user, l);
    if (g_fault.valid) {
      std::snprintf(l, sizeof(l),
                    "[crash] the previous run FAULTED at %lu ms: pc 0x%08lx lr 0x%08lx cfsr 0x%08lx hfsr 0x%08lx "
                    "bfar 0x%08lx mmfar 0x%08lx (%lu in a row); decode: arm-none-eabi-addr2line -pfiaC -e "
                    "build/daisy/stage%d/pocket-groove-daisy.elf 0x%08lx 0x%08lx",
                    static_cast<unsigned long>(g_fault.uptimeMs), static_cast<unsigned long>(g_fault.pc),
                    static_cast<unsigned long>(g_fault.lr), static_cast<unsigned long>(g_fault.cfsr),
                    static_cast<unsigned long>(g_fault.hfsr), static_cast<unsigned long>(g_fault.bfar),
                    static_cast<unsigned long>(g_fault.mmfar), static_cast<unsigned long>(g_fault.count),
                    PG_DAISY_STAGE, static_cast<unsigned long>(g_fault.pc), static_cast<unsigned long>(g_fault.lr));
    } else {
      std::snprintf(l, sizeof(l), "[crash] no fault recorded before this boot");
    }
    fn(user, l);
    const uint32_t mainFpscr = __get_FPSCR();
    std::snprintf(l, sizeof(l),
                  "[fpu] FPSCR main loop 0x%08lx, audio interrupt 0x%08lx, FPDSCR 0x%08lx: flush-to-zero %s, default "
                  "NaN %s; the interrupt had to set them %lu times",
                  static_cast<unsigned long>(mainFpscr), static_cast<unsigned long>(g_isrFpscr),
                  static_cast<unsigned long>(FPU->FPDSCR),
                  (mainFpscr & g_isrFpscr & (1u << 24)) ? "on" : "OFF", (mainFpscr & g_isrFpscr & (1u << 25)) ? "on" : "OFF",
                  static_cast<unsigned long>(g_isrFpscrFixes));
    fn(user, l);
    pgdev::memory::reportLines(fn, user);
    const pgdev::usbserial::Stats us = pgdev::usbserial::stats();
    std::snprintf(l, sizeof(l), "[usb] %s; rx %lu B (dropped %lu), tx %lu B in %lu packets (dropped %lu, busy %lu)",
                  pgdev::usbserial::configured() ? "configured" : "NOT CONFIGURED",
                  static_cast<unsigned long>(us.rxBytes), static_cast<unsigned long>(us.rxDropped),
                  static_cast<unsigned long>(us.txBytes), static_cast<unsigned long>(us.txPackets),
                  static_cast<unsigned long>(us.txDropped), static_cast<unsigned long>(us.txBusy));
    fn(user, l);
    const pgdev::QspiRegion::Stats& a = g_fsFlash.stats();
    const pgdev::QspiRegion::Stats& b = g_poolFlash.stats();
    std::snprintf(l, sizeof(l),
                  "[qspi] /int: erases %lu writes %lu reads %lu failures %lu remaps %lu, longest erase %lu us write "
                  "%lu us; pool: erases %lu writes %lu failures %lu, longest erase %lu us",
                  static_cast<unsigned long>(a.erases), static_cast<unsigned long>(a.writes),
                  static_cast<unsigned long>(a.reads), static_cast<unsigned long>(a.failures),
                  static_cast<unsigned long>(a.remaps), static_cast<unsigned long>(a.maxEraseUs),
                  static_cast<unsigned long>(a.maxWriteUs), static_cast<unsigned long>(b.erases),
                  static_cast<unsigned long>(b.writes), static_cast<unsigned long>(b.failures),
                  static_cast<unsigned long>(b.maxEraseUs));
    fn(user, l);
    pgdev::stage1::reportLines(fn, user);
  }
};

DaisyHal g_hal;

void startCycleCounter() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55u;   // the Cortex-M7's DWT lock (harmless where there is none)
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void startAdc() {
  // Four pots (A0..A3) and, in stage 1, the battery divider (A4) and the four
  // force-sensor pads (A5..A8): pins.h. The pads sample for 32.5 ADC cycles instead of
  // 8.5: their source is the 10 k divider (the pots' is at most 2.5 k), and a short
  // sampling time with a high source impedance lets the previous channel's charge
  // leak into the reading (crosstalk between channels).
  int n = 0;
  for (int i = 0; i < 4; ++i) g_adcCfg[n++].InitSingle(daisy::DaisySeed::GetPin(pgd::pins::kPotSeed[i]));
  if (PG_DAISY_STAGE >= 1) {
    g_adcCfg[n++].InitSingle(daisy::DaisySeed::GetPin(pgd::pins::kBatterySeed));
    g_adcPads = n;
    for (int i = 0; i < 4; ++i)
      g_adcCfg[n++].InitSingle(daisy::DaisySeed::GetPin(pgd::pins::kPadSeed[i]),
                               daisy::AdcChannelConfig::SPEED_32CYCLES_5);
  }
  g_hw.adc.Init(g_adcCfg, static_cast<size_t>(n));
  g_hw.adc.Start();
  g_adcOn = true;
}

}  // namespace

int main() {
  // 480 MHz (libDaisy's Boost: VOS0). Also brings up SDRAM, QSPI (memory-mapped),
  // the codec and SAI1, the LED.
  g_hw.Init(true);
  pgdev::clock::begin();
  g_hw.SetLed(true);
  pgdev::memory::setUptimeHook(uptimeMs);
  g_fault = pgdev::memory::lastFault();
  pgdev::memory::installFaultHandlers();
  startCycleCounter();
  g_board = g_hw.CheckBoardVersion();
  std::snprintf(g_boardName, sizeof(g_boardName), "%s", boardVersionName(g_board));
  const bool romOk = pgdev::memory::copyRom();

  pgdev::usbserial::begin(g_rxRing, sizeof(g_rxRing), g_txRing, sizeof(g_txRing));
  g_fsFlash.attach(&g_hw.qspi, pgd::layout::kFsAddr, 2 * pgd::layout::kFsBankBytes);
  g_poolFlash.attach(&g_hw.qspi, pgd::layout::kPoolAddr, pgd::layout::kPoolBytes);
  startAdc();
  pgdev::stage1::begin(PG_DAISY_STAGE >= 1);

  pgd::AppMemory mem;
  mem.fsArena = g_fsArena;
  mem.fsSlotBytes = pgd::layout::kFsSlotBytes;
  mem.fsStaging = g_fsStaging;
  mem.fsBankBytes = pgd::layout::kFsBankBytes;
  mem.fsFlash = &g_fsFlash;
  mem.poolImage = g_poolImage;
  mem.poolStaging = g_poolStaging;
  mem.poolFlash = &g_poolFlash;
  mem.lineRing = g_lineRing;
  mem.lineRingSamples = kLineRing;
  pgd::AppConfig cfg;
  cfg.sampleRate = static_cast<float>(kRate);
  cfg.autoplay = PG_DAISY_STAGE == 0;
  cfg.version = PG_VERSION;
  g_app.setRomMissing(!romOk);
  g_app.boot(g_hal, mem, cfg);
  if (g_fault.valid)
    g_app.sp("[boot] THE PREVIOUS RUN CRASHED (pc 0x%08lx, cfsr 0x%08lx): 'r' has the details\n",
             static_cast<unsigned long>(g_fault.pc), static_cast<unsigned long>(g_fault.cfsr));

  // Audio last: the session is loaded, the first block is already the real thing.
  g_hw.SetAudioBlockSize(kBlock);
  g_hw.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
  g_hw.StartAudio(audioCallback);
  g_audioOn = true;
  g_app.sp("[boot] audio started: %lu Hz, %lu-frame blocks; %s\n", static_cast<unsigned long>(kRate),
           static_cast<unsigned long>(kBlock), romOk ? "ROM ok" : "ROM PART MISSING");

  for (;;) g_app.loopOnce();
}
