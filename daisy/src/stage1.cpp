// See stage1.h. Pins: daisy/core/pins.h. Compiled in every build, started only with
// begin(true) (make STAGE=1); none of it has run on hardware.
//
// libDaisy 1289a00 facts relied on: SpiHandle::DmaTransmit does no cache maintenance
// (the strip buffers are cleaned before each transfer: they live in cached AXI SRAM;
// DMA1/2 reach AXI SRAM through the bus matrix); its end callback runs in the DMA
// interrupt. I2CHandle::ReadDataAtAddress / WriteDataAtAddress take 7-bit addresses
// and block (timeout in ms; before it, the HAL waits up to 25 ms for a busy bus:
// i2c_bus.h's guard); the pins are open drain with NO internal pull-ups (the boards
// on the bus bring theirs). FatFSInterface::Init(MEDIA_SD) links the SD driver and
// gives the path "0:/"; SdmmcHandler uses SDMMC1 on D1..D6, 4-bit. SPI1's kernel
// clock is PLL2P = 25 MHz (system.cpp: HSE 16 MHz / M 1 * N 12.5 / P 8), so
// prescaler 2 = 12.5 MHz, 4 = 6.25, 8 = 3.125 MHz.
#include "stage1.h"

#if PG_DAISY_STAGE >= 1

#include <cstdio>
#include <cstring>

#include "../core/bmi270.h"
#include "../core/button.h"
#include "../core/i2c_bus.h"
#include "../core/panel.h"
#include "../core/pins.h"
#include "../core/touch.h"
#include "clock.h"
#include "daisy_seed.h"
#include "fatfs.h"
#include "pg/tilt/tilt.h"

#if __has_include("../vendor/bmi270_config.inc")
#define PGD_HAVE_BMI270_CONFIG 1
#else
#define PGD_HAVE_BMI270_CONFIG 0
#endif

namespace pgdev {
namespace stage1 {

namespace {

daisy::Pin seedPin(uint8_t d) { return daisy::DaisySeed::GetPin(d); }

#if PGD_HAVE_BMI270_CONFIG
// Bosch Sensortec BMI270_SensorAPI bmi270.c bmi270_config_file[] (BSD-3-Clause; the
// notice is in daisy/vendor/BMI270_LICENSE, extracted with it by tools/daisy-setup.sh).
// In the ROM part (QSPI, copied to SDRAM at boot, before begin() runs): read once.
__attribute__((section(".qspi_rom"))) const uint8_t kBmiConfig[] = {
#include "../vendor/bmi270_config.inc"
};
#endif

bool g_on = false;

// ---- display ------------------------------------------------------------------------
// The panel (make STAGE=1 PANEL=...: -DPG_DAISY_PANEL): 0 the Adafruit 2090 (ILI9341
// 320x240, the stage 1 default), 1 a 3.5" HX8357D 480x320, 2 the ST7789 240x135.
#ifndef PG_DAISY_PANEL
#define PG_DAISY_PANEL 0
#endif

pgd::PanelConfig panelConfig(bool turned) {
  if (PG_DAISY_PANEL == 1) return pgd::PanelConfig::hx8357d_480x320();
  if (PG_DAISY_PANEL == 2) return pgd::PanelConfig::st7789_240x135();
  return pgd::PanelConfig::ili9341_320x240(turned);
}

daisy::SpiHandle::Config::BaudPrescaler prescaler(uint8_t div) {
  using P = daisy::SpiHandle::Config::BaudPrescaler;
  return div >= 16 ? P::PS_16 : div >= 8 ? P::PS_8 : div >= 4 ? P::PS_4 : P::PS_2;
}

struct SpiBus final : pgd::SpiPanelBus {
  daisy::SpiHandle spi;
  daisy::GPIO dc, cs, bl;
  volatile bool busyFlag = false;
  bool ok = false;
  uint8_t divider = 2;
  static void done(void* ctx, daisy::SpiHandle::Result) { static_cast<SpiBus*>(ctx)->busyFlag = false; }
  // 25 MHz / div (2: 12.5 MHz, near the ILI9341's 10 MHz write-cycle rating, which
  // Adafruit's drivers exceed by far; slower over long breadboard wires: >display spi).
  bool initSpi(uint8_t div) {
    while (busyFlag) {
    }
    daisy::SpiHandle::Config c;
    c.periph = daisy::SpiHandle::Config::Peripheral::SPI_1;
    c.mode = daisy::SpiHandle::Config::Mode::MASTER;
    c.direction = daisy::SpiHandle::Config::Direction::TWO_LINES_TX_ONLY;
    c.nss = daisy::SpiHandle::Config::NSS::SOFT;
    c.baud_prescaler = prescaler(div);
    c.pin_config.sclk = seedPin(8);
    c.pin_config.mosi = seedPin(10);
    c.pin_config.miso = daisy::Pin();
    c.pin_config.nss = daisy::Pin();
    divider = div;
    ok = spi.Init(c) == daisy::SpiHandle::Result::OK;
    return ok;
  }
  bool init() {
    dc.Init(seedPin(9), daisy::GPIO::Mode::OUTPUT);
    cs.Init(seedPin(7), daisy::GPIO::Mode::OUTPUT);
    bl.Init(seedPin(27), daisy::GPIO::Mode::OUTPUT);
    cs.Write(false);   // the only device on the bus
    bl.Write(true);
    return initSpi(2);
  }
  void command(uint8_t cmd, const uint8_t* params, size_t n) override {
    while (busyFlag) {
    }
    dc.Write(false);
    spi.BlockingTransmit(&cmd, 1);
    dc.Write(true);
    if (n) spi.BlockingTransmit(const_cast<uint8_t*>(params), n);
  }
  void pixelsAsync(const uint8_t* data, size_t n) override {
    while (busyFlag) {
    }
    dc.Write(true);
    const uint32_t a = reinterpret_cast<uint32_t>(data) & ~31u;
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(a), static_cast<int32_t>(n + 64));
    busyFlag = true;
    if (spi.DmaTransmit(const_cast<uint8_t*>(data), n, nullptr, done, this) != daisy::SpiHandle::Result::OK)
      busyFlag = false;
  }
  bool busy() override { return busyFlag; }
  void delayMs(uint32_t ms) override { daisy::System::Delay(ms); }
};
SpiBus g_spi;
pgd::Panel g_panel;
// The strip buffers (30 KB) in D2 SRAM, next to the App: DMA1/2 (SPI1's) reach it,
// it is cached like AXI SRAM (pixelsAsync cleans the lines), and the AXI SRAM after
// the image is too small for them once the image is near its 480 KB.
__attribute__((section(".d2_bss"), aligned(32))) uint8_t g_strips[2 * pgd::Panel::kStripBytes];
bool g_panelOn = false;

// ---- I2C1: the BMI270 and the FT6206 ----------------------------------------------
// The timeout of one transaction (ms): the longest one is a 64-byte BMI270 chunk,
// ~1.7 ms at 400 kHz.
constexpr uint32_t kI2cTimeoutMs = 3;
struct I2c final : pgd::I2cBus {
  daisy::I2CHandle h;
  uint32_t errors = 0;
  bool init() {
    daisy::I2CHandle::Config c;
    c.periph = daisy::I2CHandle::Config::Peripheral::I2C_1;
    c.mode = daisy::I2CHandle::Config::Mode::I2C_MASTER;
    c.speed = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
    c.pin_config.scl = seedPin(11);
    c.pin_config.sda = seedPin(12);
    return h.Init(c) == daisy::I2CHandle::Result::OK;
  }
  bool write(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) override {
    const bool ok = h.WriteDataAtAddress(addr, reg, 1, const_cast<uint8_t*>(data), static_cast<uint16_t>(n),
                                         kI2cTimeoutMs) == daisy::I2CHandle::Result::OK;
    if (!ok) ++errors;
    return ok;
  }
  bool read(uint8_t addr, uint8_t reg, uint8_t* data, size_t n) override {
    const bool ok = h.ReadDataAtAddress(addr, reg, 1, data, static_cast<uint16_t>(n), kI2cTimeoutMs) ==
                    daisy::I2CHandle::Result::OK;
    if (!ok) ++errors;
    return ok;
  }
};
I2c g_i2c;
pgd::I2cGuard g_bus;   // both drivers go through it (a stuck bus never holds the loop)
uint32_t busMicros(void*) { return pgdev::clock::micros(); }
bool g_i2cOn = false;
pgd::Bmi270 g_imu;
// touch
pgd::Ft6206 g_touch;
pgd::TouchMap g_touchMap;
daisy::GPIO g_touchIrq;
pgd::Hal::Pointer g_ptrQ[16];
int g_ptrN = 0;
pg::tilt::Tracker g_tracker;
bool g_tiltOn = false, g_tiltFresh = false;
uint32_t g_lastImuMs = 0;
constexpr uint32_t kImuPollMs = 10;   // 100 Hz while held, as on the Cardputer
constexpr float kAccScale = 1.0f / 8192.0f, kGyrScale = 0.0174532925f / 32.768f;

// ---- G0 ------------------------------------------------------------------------------
daisy::GPIO g_g0;
pgd::Button g_g0Button;
pg::KeyEvent g_keyQ[16];
int g_keyN = 0;

// ---- microSD (FatFS) -----------------------------------------------------------------
daisy::SdmmcHandler g_sd;
__attribute__((section(".axi_bss"))) daisy::FatFSInterface g_fatfs;
__attribute__((section(".axi_bss"))) FIL g_files[2];
bool g_fileOpen[2] = {false, false};
bool g_sdMounted = false;
uint32_t g_sdTryMs = 0;
int g_sdErr = 0;

bool sdPath(const char* p, char* out, size_t cap) {
  if (!p || std::strncmp(p, "/sd", 3) != 0 || (p[3] != 0 && p[3] != '/')) return false;
  std::snprintf(out, cap, "%s%s", g_fatfs.GetSDPath(), p[3] ? p + 4 : "");
  return true;
}

void mkdirs(const char* fpath) {   // the parents of a file path ("0:/pg/x" -> "0:/pg")
  char d[64];
  std::snprintf(d, sizeof(d), "%s", fpath);
  for (char* s = d + 3; *s; ++s) {
    if (*s != '/') continue;
    *s = 0;
    f_mkdir(d);
    *s = '/';
  }
}

struct SdFs final : pg::FileSystem {
  bool mounted(const char* m) override { return g_sdMounted && m && std::strcmp(m, "/sd") == 0; }
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override {
    const int h = open(path, false);
    if (h < 0) return false;
    size_t n = 0;
    for (;;) {
      const int r = readSome(h, buf + n, cap - n);
      if (r <= 0) break;
      n += static_cast<size_t>(r);
    }
    close(h);
    if (len) *len = n;
    return true;
  }
  bool write(const char* path, const uint8_t* buf, size_t len) override {
    const int h = open(path, true);
    if (h < 0) return false;
    const bool ok = writeSome(h, buf, len);
    return close(h) && ok;
  }
  bool remove(const char* path) override {
    char p[64];
    return g_sdMounted && sdPath(path, p, sizeof(p)) && f_unlink(p) == FR_OK;
  }
  bool exists(const char* path) override {
    char p[64];
    FILINFO fi;
    return g_sdMounted && sdPath(path, p, sizeof(p)) && f_stat(p, &fi) == FR_OK;
  }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) override {
    char p[64];
    if (!g_sdMounted || !sdPath(dir, p, sizeof(p))) return -1;
    DIR d;
    if (f_opendir(&d, p) != FR_OK) return -1;
    int n = 0;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
      if (fi.fattrib & AM_DIR) continue;
      if (cb) cb(fi.fname, fi.fsize, user);
      ++n;
    }
    f_closedir(&d);
    return n;
  }
  int open(const char* path, bool forWrite) override {
    char p[64];
    if (!g_sdMounted || !sdPath(path, p, sizeof(p))) return -1;
    for (int i = 0; i < 2; ++i) {
      if (g_fileOpen[i]) continue;
      if (forWrite) mkdirs(p);
      if (f_open(&g_files[i], p, forWrite ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ) != FR_OK) return -1;
      g_fileOpen[i] = true;
      return i;
    }
    return -1;
  }
  int readSome(int h, uint8_t* buf, size_t cap) override {
    if (h < 0 || h > 1 || !g_fileOpen[h]) return -1;
    UINT got = 0;
    return f_read(&g_files[h], buf, static_cast<UINT>(cap), &got) == FR_OK ? static_cast<int>(got) : -1;
  }
  bool writeSome(int h, const uint8_t* buf, size_t len) override {
    if (h < 0 || h > 1 || !g_fileOpen[h]) return false;
    UINT put = 0;
    return f_write(&g_files[h], buf, static_cast<UINT>(len), &put) == FR_OK && put == len;
  }
  bool close(int h) override {
    if (h < 0 || h > 1 || !g_fileOpen[h]) return false;
    g_fileOpen[h] = false;
    return f_close(&g_files[h]) == FR_OK;
  }
  bool rename(const char* from, const char* to) override {
    char a[64], b[64];
    if (!g_sdMounted || !sdPath(from, a, sizeof(a)) || !sdPath(to, b, sizeof(b))) return false;
    f_unlink(b);   // FatFs does not replace: pg::io's power-loss recovery covers the gap (as on the Cardputer)
    return f_rename(a, b) == FR_OK;
  }
  long freeBytes(const char* m) override {
    if (!mounted(m)) return -1;
    DWORD clusters = 0;
    FATFS* fs = nullptr;
    if (f_getfree(g_fatfs.GetSDPath(), &clusters, &fs) != FR_OK || !fs) return -1;
    return static_cast<long>(clusters) * fs->csize * 512;
  }
};
SdFs g_sdFs;

void tryMountSd(uint32_t nowMs) {
  g_sdTryMs = nowMs | 1u;
  daisy::SdmmcHandler::Config sc;
  sc.Defaults();
  sc.speed = daisy::SdmmcHandler::Speed::STANDARD;   // 25 MHz: breadboard wiring
  if (g_sd.Init(sc) != daisy::SdmmcHandler::Result::OK) {
    g_sdErr = 1;
    return;
  }
  if (g_fatfs.Init(daisy::FatFSInterface::Config::MEDIA_SD) != daisy::FatFSInterface::Result::OK) {
    g_sdErr = 2;
    return;
  }
  const FRESULT r = f_mount(&g_fatfs.GetSDFileSystem(), g_fatfs.GetSDPath(), 1);
  g_sdErr = r;
  g_sdMounted = r == FR_OK;
}

}  // namespace

void begin(bool enabled) {
  g_on = enabled;
  if (!enabled) return;
  // G0
  g_g0.Init(seedPin(25), daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);   // pins.h "G0 BUTTON"
  // display
  if (g_spi.init()) {
    g_panel.init(panelConfig(false), &g_spi, g_strips);
    g_panel.begin();
    g_panelOn = true;
  }
  // I2C1: the IMU (its setup runs on in service()) and the touch controller
  g_tracker.setRate(1000.0f / kImuPollMs);
  g_touchIrq.Init(seedPin(pgd::pins::kTouchIrqSeed), daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
  const pgd::PanelConfig& pc = g_panel.config();
  g_touchMap.x0 = pc.x0;
  g_touchMap.y0 = pc.y0;
  g_touchMap.scale = pc.scale;
  const int o = pgd::TouchMap::orientationFor(pc.madctl);
  g_touchMap.orientation = static_cast<uint8_t>(o < 0 ? 3 : o);
  if (g_i2c.init()) {
    g_i2cOn = true;
    g_bus.init(&g_i2c, busMicros, nullptr);
#if PGD_HAVE_BMI270_CONFIG
    g_imu.begin(&g_bus, kBmiConfig, sizeof(kBmiConfig));
#else
    g_imu.begin(&g_bus, nullptr, 0);
#endif
    g_touch.begin(&g_bus, daisy::System::GetNow());
  }
  // SD: one attempt at boot (blocking, ~0.1-1 s with a card), retried every 10 s.
  tryMountSd(daisy::System::GetNow());
}

void service(uint32_t nowMs) {
  if (!g_on) return;
  pg::KeyEvent e;
  if (g_keyN < 16 && g_g0Button.poll(!g_g0.Read(), nowMs, &e)) g_keyQ[g_keyN++] = e;
  if (g_panelOn) g_panel.step();
  if (!g_imu.ready()) g_imu.step(nowMs);
  if (g_i2cOn && g_touch.step(nowMs)) {
    pgd::Ft6206::Event te;
    while (g_touch.pop(&te)) {
      if (g_ptrN >= 16) continue;
      int px, py, cx, cy;
      g_touchMap.mapToPanel(te.x, te.y, &px, &py);
      g_touchMap.toCanvas(px, py, &cx, &cy);
      g_ptrQ[g_ptrN++] = pgd::Hal::Pointer{static_cast<int16_t>(cx), static_cast<int16_t>(cy),
                                           static_cast<int16_t>(te.x), static_cast<int16_t>(te.y),
                                           te.kind != pgd::Ft6206::Event::Up, te.ms};
    }
  }
  if (g_tiltOn && g_imu.ready() && nowMs - g_lastImuMs >= kImuPollMs) {
    g_lastImuMs = nowMs;
    pgd::Bmi270::Raw r;
    if (g_imu.read(&r)) {
      const pg::tilt::Vec3 acc = {r.acc[0] * kAccScale, r.acc[1] * kAccScale, r.acc[2] * kAccScale};
      const pg::tilt::Vec3 gyr = {r.gyr[0] * kGyrScale, r.gyr[1] * kGyrScale, r.gyr[2] * kGyrScale};
      g_tracker.sample(acc, gyr);
      g_tiltFresh = true;
    }
  }
  if (!g_sdMounted && nowMs - g_sdTryMs > 10000) tryMountSd(nowMs);
}

bool pollKey(pg::KeyEvent* e) {
  if (!g_keyN) return false;
  *e = g_keyQ[0];
  for (int i = 1; i < g_keyN; ++i) g_keyQ[i - 1] = g_keyQ[i];
  --g_keyN;
  return true;
}

bool pollPointer(pgd::Hal::Pointer* p) {
  if (!g_ptrN) return false;
  *p = g_ptrQ[0];
  for (int i = 1; i < g_ptrN; ++i) g_ptrQ[i - 1] = g_ptrQ[i];
  --g_ptrN;
  return true;
}

bool touchInfo(pgd::Hal::TouchInfo* t) {
  if (!g_on) return false;
  t->state = g_i2cOn ? pgd::Ft6206::stateName(g_touch.state()) : "I2C NOT STARTED";
  t->present = g_touch.present();
  t->chipId = g_touch.chipId();
  t->firmware = g_touch.firmware();
  t->reads = g_touch.reads();
  t->errors = g_touch.errors();
  t->irq = g_touchIrq.Read() ? 1 : 0;
  t->orientation = g_touchMap.orientation;
  return true;
}

bool displayPresent() { return g_panelOn; }
void present(const pg::Framebuffer& fb, int y0, int y1) {
  if (g_panelOn) g_panel.present(fb, y0, y1);
}

void applySettings(const pgd::DaisySettings& s) {
  if (!g_on) return;
  bool again = false;
  if (g_panelOn && PG_DAISY_PANEL == 0) {
    const uint8_t m = s.displayTurned ? pgd::PanelConfig::kIliMadctlTurned : pgd::PanelConfig::kIliMadctl;
    if (m != g_panel.config().madctl) {
      g_panel.setMadctl(m);
      again = true;
    }
  }
  if (g_panelOn && s.spiDivider != g_spi.divider) {
    g_spi.initSpi(s.spiDivider);
    again = true;
  }
  if (again) g_panel.begin();   // blocking ~0.5 s, then the last frame again
  const int o = s.touchOrientation >= 0 ? s.touchOrientation : pgd::TouchMap::orientationFor(g_panel.config().madctl);
  g_touchMap.orientation = static_cast<uint8_t>(o < 0 ? 3 : o);
}

bool imuPresent() { return g_imu.ready(); }
void tiltPress(int rx, int ry) {
  g_tracker.press(rx, ry);
  g_tiltOn = true;
  g_tiltFresh = false;
}
void tiltRelease() {
  g_tracker.release();
  g_tiltOn = false;
}
bool tiltRead(float* x, float* y) {
  if (!g_tiltFresh) return false;
  g_tiltFresh = false;
  *x = g_tracker.x();
  *y = g_tracker.y();
  return true;
}

pg::FileSystem* sdCard() { return g_on ? &g_sdFs : nullptr; }

void reportLines(LineFn fn, void* user) {
  char l[240];
  if (!g_on) {
    fn(user, "[stage1] not started (a stage 0 build): no display, IMU, SD card or G0");
    return;
  }
  const pgd::PanelConfig& pc = g_panel.config();
  std::snprintf(l, sizeof(l),
                "[display] %s %ux%u, the UI at scale %u at (%u,%u), MADCTL 0x%02x, SPI %lu.%02lu MHz (25 / %u), "
                "frames %lu",
                g_panelOn ? pc.name() : "NOT STARTED", pc.width, pc.height, pc.scale, pc.x0, pc.y0, pc.madctl,
                static_cast<unsigned long>(25 / g_spi.divider), static_cast<unsigned long>(2500 / g_spi.divider % 100),
                g_spi.divider, static_cast<unsigned long>(g_panel.framesSent()));
  fn(user, l);
  std::snprintf(l, sizeof(l), "[imu] BMI270 %s @0x%02x, err 0x%02x, status 0x%02x, reads %lu%s",
                pgd::Bmi270::stateName(g_imu.state()), g_imu.address(), g_imu.errReg(), g_imu.internalStatus(),
                static_cast<unsigned long>(g_imu.reads()),
                PGD_HAVE_BMI270_CONFIG ? "" : " (built without Bosch's configuration file)");
  fn(user, l);
  std::snprintf(l, sizeof(l),
                "[i2c] I2C1 400 kHz %s: ok %lu, failed %lu (NAKs included), slow failures %lu (the bus %s), skipped "
                "%lu, longest %lu us",
                g_i2cOn ? "started" : "NOT STARTED", static_cast<unsigned long>(g_bus.ok()),
                static_cast<unsigned long>(g_bus.errors()), static_cast<unsigned long>(g_bus.stalls()),
                g_bus.stalled() ? "IS HELD: SDA/SCL stuck or no pull-ups?" : "free",
                static_cast<unsigned long>(g_bus.skipped()), static_cast<unsigned long>(g_bus.maxUs()));
  fn(user, l);
  std::snprintf(l, sizeof(l), "[sd] %s (FatFS result %d)", g_sdMounted ? "mounted" : "NOT MOUNTED", g_sdErr);
  fn(user, l);
  std::snprintf(l, sizeof(l), "[g0] %s", g_g0Button.down() ? "held" : "up");
  fn(user, l);
}

}  // namespace stage1
}  // namespace pgdev

#else  // stage 0: none of the drivers is linked (the 480 KB image has no room to spare)

#include <cstdio>

namespace pgdev {
namespace stage1 {
void begin(bool) {}
void service(uint32_t) {}
bool pollKey(pg::KeyEvent*) { return false; }
bool pollPointer(pgd::Hal::Pointer*) { return false; }
bool touchInfo(pgd::Hal::TouchInfo*) { return false; }
bool displayPresent() { return false; }
void present(const pg::Framebuffer&, int, int) {}
void applySettings(const pgd::DaisySettings&) {}
bool imuPresent() { return false; }
void tiltPress(int, int) {}
void tiltRelease() {}
bool tiltRead(float*, float*) { return false; }
pg::FileSystem* sdCard() { return nullptr; }
void reportLines(LineFn fn, void* user) { fn(user, "[stage1] not in this build (stage 0): no display, IMU, SD card or G0"); }
}  // namespace stage1
}  // namespace pgdev

#endif
