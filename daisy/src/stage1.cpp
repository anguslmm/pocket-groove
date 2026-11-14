// See stage1.h. Pins: daisy/core/pins.h. Compiled in every build, started only with
// begin(true) (make STAGE=1); none of it has run on hardware.
//
// libDaisy 1289a00 facts relied on: SpiHandle::DmaTransmit does no cache maintenance
// (the strip buffers are cleaned before each transfer: they live in cached AXI SRAM;
// DMA1/2 reach AXI SRAM through the bus matrix); its end callback runs in the DMA
// interrupt. I2CHandle::ReadDataAtAddress / WriteDataAtAddress take 7-bit addresses
// and block (timeout in ms). FatFSInterface::Init(MEDIA_SD) links the SD driver and
// gives the path "0:/"; SdmmcHandler uses SDMMC1 on D1..D6, 4-bit.
#include "stage1.h"

#if PG_DAISY_STAGE >= 1

#include <cstdio>
#include <cstring>

#include "../core/bmi270.h"
#include "../core/button.h"
#include "../core/panel.h"
#include "../core/pins.h"
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
#ifndef PG_DAISY_PANEL_HX8357D
#define PG_DAISY_PANEL_HX8357D 1   // the likely first screen (3.5" 480x320); 0: ST7789 240x135
#endif

struct SpiBus final : pgd::SpiPanelBus {
  daisy::SpiHandle spi;
  daisy::GPIO dc, cs, bl;
  volatile bool busyFlag = false;
  bool ok = false;
  static void done(void* ctx, daisy::SpiHandle::Result) { static_cast<SpiBus*>(ctx)->busyFlag = false; }
  bool init() {
    daisy::SpiHandle::Config c;
    c.periph = daisy::SpiHandle::Config::Peripheral::SPI_1;
    c.mode = daisy::SpiHandle::Config::Mode::MASTER;
    c.direction = daisy::SpiHandle::Config::Direction::TWO_LINES_TX_ONLY;
    c.nss = daisy::SpiHandle::Config::NSS::SOFT;
    // The SPI1 kernel clock and so the bit rate this gives are not checked here: start
    // slow and raise it on the hardware (docs/DAISY.md "Display").
    c.baud_prescaler = daisy::SpiHandle::Config::BaudPrescaler::PS_8;
    c.pin_config.sclk = seedPin(8);
    c.pin_config.mosi = seedPin(10);
    c.pin_config.miso = daisy::Pin();
    c.pin_config.nss = daisy::Pin();
    ok = spi.Init(c) == daisy::SpiHandle::Result::OK;
    dc.Init(seedPin(9), daisy::GPIO::Mode::OUTPUT);
    cs.Init(seedPin(7), daisy::GPIO::Mode::OUTPUT);
    bl.Init(seedPin(27), daisy::GPIO::Mode::OUTPUT);
    cs.Write(false);   // the only device on the bus
    bl.Write(true);
    return ok;
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
__attribute__((section(".axi_bss"), aligned(32))) uint8_t g_strips[2 * pgd::Panel::kStripBytes];
bool g_panelOn = false;

// ---- I2C1: the BMI270 (and touch, later) --------------------------------------------
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
    const bool ok = h.WriteDataAtAddress(addr, reg, 1, const_cast<uint8_t*>(data), static_cast<uint16_t>(n), 10) ==
                    daisy::I2CHandle::Result::OK;
    if (!ok) ++errors;
    return ok;
  }
  bool read(uint8_t addr, uint8_t reg, uint8_t* data, size_t n) override {
    const bool ok = h.ReadDataAtAddress(addr, reg, 1, data, static_cast<uint16_t>(n), 10) == daisy::I2CHandle::Result::OK;
    if (!ok) ++errors;
    return ok;
  }
};
I2c g_i2c;
pgd::Bmi270 g_imu;
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
    g_panel.init(PG_DAISY_PANEL_HX8357D ? pgd::PanelConfig::hx8357d_480x320() : pgd::PanelConfig::st7789_240x135(),
                 &g_spi, g_strips);
    g_panel.begin();
    g_panelOn = true;
  }
  // IMU (the setup runs on in service())
  g_tracker.setRate(1000.0f / kImuPollMs);
  if (g_i2c.init()) {
#if PGD_HAVE_BMI270_CONFIG
    g_imu.begin(&g_i2c, kBmiConfig, sizeof(kBmiConfig));
#else
    g_imu.begin(&g_i2c, nullptr, 0);
#endif
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

bool displayPresent() { return g_panelOn; }
void present(const pg::Framebuffer& fb, int y0, int y1) {
  if (g_panelOn) g_panel.present(fb, y0, y1);
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

// Recording: line in through the codec's input is the first path (docs/DAISY.md); not
// built yet: the ring this would fill from audioInput() and a pg::sampler::MicInput over
// pg's Recorder (48 kHz in, recRatesFor) are the next step.
pg::sampler::MicInput* mic() { return nullptr; }
void audioInput(const float*, size_t) {}

pg::FileSystem* sdCard() { return g_on ? &g_sdFs : nullptr; }

void reportLines(LineFn fn, void* user) {
  char l[240];
  if (!g_on) {
    fn(user, "[stage1] not started (a stage 0 build): no display, IMU, SD card or G0");
    return;
  }
  std::snprintf(l, sizeof(l), "[display] %s, %ux%u at scale %u, frames %lu",
                g_panelOn ? (PG_DAISY_PANEL_HX8357D ? "HX8357D" : "ST7789") : "NOT STARTED", g_panel.config().width,
                g_panel.config().height, g_panel.config().scale, static_cast<unsigned long>(g_panel.framesSent()));
  fn(user, l);
  std::snprintf(l, sizeof(l), "[imu] BMI270 %s @0x%02x, err 0x%02x, status 0x%02x, reads %lu, i2c errors %lu%s",
                pgd::Bmi270::stateName(g_imu.state()), g_imu.address(), g_imu.errReg(), g_imu.internalStatus(),
                static_cast<unsigned long>(g_imu.reads()), static_cast<unsigned long>(g_i2c.errors),
                PGD_HAVE_BMI270_CONFIG ? "" : " (built without Bosch's configuration file)");
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
bool displayPresent() { return false; }
void present(const pg::Framebuffer&, int, int) {}
bool imuPresent() { return false; }
void tiltPress(int, int) {}
void tiltRelease() {}
bool tiltRead(float*, float*) { return false; }
pg::sampler::MicInput* mic() { return nullptr; }
void audioInput(const float*, size_t) {}
pg::FileSystem* sdCard() { return nullptr; }
void reportLines(LineFn fn, void* user) { fn(user, "[stage1] not in this build (stage 0): no display, IMU, SD card or G0"); }
}  // namespace stage1
}  // namespace pgdev

#endif
