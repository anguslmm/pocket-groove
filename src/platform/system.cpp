// Platform layer: system services. See system.h.
#include "system.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <esp_chip_info.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>
#include <new>

#if PG_USB_AUDIO
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp32-hal-tinyusb.h>

#include "usb_cdc.h"
#endif

namespace plat {
namespace sys {

namespace {
BootInfo s_boot = {false, false, 0, "?"};
uint32_t s_serialMuteUntilMs = 0;   // after a stalled write, drop output until then

#if PG_USB_AUDIO
// Boot log: RTC slow memory is not part of the heap (and survives a
// software reset, which nothing here relies on: begin() empties it).
constexpr size_t kBootLogCap = 3072;
RTC_NOINIT_ATTR char s_bootLog[kBootLogCap];
size_t s_bootLogLen = 0;
bool s_bootLogOn = false;
constexpr uint32_t kMenuMagic = 0x504D454Eu;   // "PMEN"
RTC_NOINIT_ATTR uint32_t s_menuReq;
#endif

const char* boardName(m5::board_t b) {
  switch (b) {
    case m5::board_t::board_M5CardputerADV: return "M5CardputerADV";
    case m5::board_t::board_M5Cardputer:    return "M5Cardputer(orig)";
    case m5::board_t::board_unknown:        return "unknown";
    default:                                return "other-M5";
  }
}
}  // namespace

BootInfo begin(uint32_t serialBaud) {
#if PG_USB_AUDIO
  // The TinyUSB CDC port: plat::usbaudio::begin() starts it with the USB
  // stack. No TX ring (TinyUSB's own 64-byte FIFO); the timeout bounds a
  // stalled host as below. Output until then only goes to the boot log.
  (void)serialBaud;
  PgSerial.setTxTimeoutMs(10);
  s_bootLogLen = 0;
  s_bootLogOn = true;
#else
  // Serial is the native USB-Serial/JTAG CDC (ARDUINO_USB_MODE=1 + CDC_ON_BOOT=1).
  // A bigger TX ring lets a whole report page go out without waiting; a short
  // TX timeout bounds how long a stalled host can hold the UI loop (see
  // serialWrite for the rest of the protection).
  Serial.setTxBufferSize(4096);
  Serial.begin(serialBaud);
  Serial.setTxTimeoutMs(10);
#endif

  auto cfg = M5.config();
  cfg.serial_baudrate = 0;       // we own Serial
  cfg.clear_display   = true;
  cfg.output_power    = true;
  cfg.internal_imu    = true;    // probe only (answers the IMU address question); never read later
  cfg.internal_rtc    = false;
  cfg.internal_mic    = false;   // M5.Mic would reconfigure the codec against the DAC
  // internal_spk stays true ON PURPOSE: M5.begin() then only *stores* the
  // board's speaker pin config and ES8311 callback (M5Unified.inl, "Speaker.
  // setCallback / Speaker.config"); the I2S port is not touched until
  // M5.Speaker.begin() or a play*() call. plat::audio never makes those calls
  // in direct-I2S mode, so the port is free for our own driver, and the stored
  // config is exactly what the fallback backend needs.
  cfg.internal_spk    = true;
  cfg.external_imu    = false;
  cfg.external_rtc    = false;
  cfg.led_brightness  = 0;
  M5.begin(cfg);

  const m5::board_t b = M5.getBoard();
  s_boot.boardId        = static_cast<int>(b);
  s_boot.boardName      = boardName(b);
  s_boot.isCardputerAdv = (b == m5::board_t::board_M5CardputerADV);
  s_boot.displayOk      = (M5.getDisplayCount() > 0) && (M5.Display.width() > 0);
  return s_boot;
}

const BootInfo& bootInfo() { return s_boot; }

uint32_t millis()        { return ::millis(); }
uint64_t micros()        { return static_cast<uint64_t>(esp_timer_get_time()); }
void     delayMs(uint32_t ms) { ::delay(ms); }
uint32_t cycles()        { return esp_cpu_get_cycle_count(); }
uint32_t cyclesPerUs()   { return esp_rom_get_cpu_ticks_per_us(); }
int      currentCore()   { return xPortGetCoreID(); }

namespace {
struct TaskArg { void (*fn)(void*); void* user; };
void taskTrampoline(void* p) {
  TaskArg a = *static_cast<TaskArg*>(p);
  delete static_cast<TaskArg*>(p);
  a.fn(a.user);
  vTaskDelete(nullptr);
}
}  // namespace

bool runTask(void (*fn)(void*), void* user, const char* name, uint32_t stackBytes, int priority, int core) {
  if (!fn) return false;
  auto* a = new (std::nothrow) TaskArg{fn, user};
  if (!a) return false;
  if (core < 0 || core > 1) core = 1;
  if (xTaskCreatePinnedToCore(taskTrampoline, name ? name : "pg_job", stackBytes, a, priority, nullptr, core) != pdPASS) {
    delete a;
    return false;
  }
  return true;
}

#if PG_USB_AUDIO
void serialWrite(const char* data, size_t len) {
  if (!data || !len) return;
  if (s_bootLogOn && s_bootLogLen < kBootLogCap) {
    const size_t n = (len < kBootLogCap - s_bootLogLen) ? len : kBootLogCap - s_bootLogLen;
    memcpy(s_bootLog + s_bootLogLen, data, n);
    s_bootLogLen += n;
  }
  // DTR set = a terminal has the port open (false before the stack runs):
  // nothing else is worth the TX lock.
  if (!tud_cdc_n_connected(0)) return;
  const uint32_t now = ::millis();
  if (s_serialMuteUntilMs && static_cast<int32_t>(now - s_serialMuteUntilMs) < 0) return;
  s_serialMuteUntilMs = 0;
  // USBCDC::write gives up after the TX timeout when the host holds the port
  // open but does not read; the mute window then keeps a long report from
  // freezing the UI.
  const size_t done = PgSerial.write(reinterpret_cast<const uint8_t*>(data), len);
  if (done < len) s_serialMuteUntilMs = (::millis() + 1500) | 1u;
}
void serialWrite(const char* text) { if (text) serialWrite(text, strlen(text)); }
int  serialRead() { return PgSerial.available() > 0 ? PgSerial.read() : -1; }
bool serialHostListening() { return tud_cdc_n_connected(0); }

void bootLogEnd() { s_bootLogOn = false; }
void bootLogReplay() {
  const bool was = s_bootLogOn;
  s_bootLogOn = false;
  serialWrite("[boot] ---- boot log (printed before this terminal connected) ----\n");
  for (size_t o = 0; o < s_bootLogLen; o += 256) serialWrite(s_bootLog + o, (s_bootLogLen - o < 256) ? s_bootLogLen - o : 256);
  if (s_bootLogLen >= kBootLogCap) serialWrite("\n[boot] (boot log truncated at 3 KB)\n");
  serialWrite("[boot] ---- end of boot log ----\n");
  s_bootLogOn = was;
}

int crashSummary(char* buf, size_t cap) {
  if (!buf || cap < 2) return 0;
  buf[0] = 0;
  if (esp_core_dump_image_check() != ESP_OK) return 0;
  auto* sm = static_cast<esp_core_dump_summary_t*>(heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!sm) return 0;
  int n = 0;
  if (esp_core_dump_get_summary(sm) == ESP_OK) {
    char reason[48] = "";
    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) != ESP_OK) reason[0] = 0;
    sm->exc_task[sizeof(sm->exc_task) - 1] = 0;
    n = snprintf(buf, cap, "[crash] core dump in flash: task \"%s\", PC 0x%08lx, cause %lu, \"%s\"; backtrace%s:",
                 sm->exc_task, static_cast<unsigned long>(sm->exc_pc),
                 static_cast<unsigned long>(sm->ex_info.exc_cause), reason, sm->exc_bt_info.corrupted ? " (corrupt)" : "");
    const uint32_t depth = sm->exc_bt_info.depth < 16 ? sm->exc_bt_info.depth : 16;
    for (uint32_t i = 0; i < depth && n > 0 && static_cast<size_t>(n) < cap - 12; ++i)
      n += snprintf(buf + n, cap - static_cast<size_t>(n), " 0x%08lx", static_cast<unsigned long>(sm->exc_bt_info.bt[i]));
    if (n > 0 && static_cast<size_t>(n) < cap - 2) n += snprintf(buf + n, cap - static_cast<size_t>(n), "\n");
  }
  heap_caps_free(sm);
  return n > 0 ? n : 0;
}

void restartIntoBootMenu() {
  s_menuReq = kMenuMagic;
  ::delay(50);
  esp_restart();
}
bool bootMenuRequested() {
  const bool r = (s_menuReq == kMenuMagic);
  s_menuReq = 0;
  return r;
}
#else
void serialWrite(const char* data, size_t len) {
  if (!data || !len) return;
  const uint32_t now = ::millis();
  if (s_serialMuteUntilMs && static_cast<int32_t>(now - s_serialMuteUntilMs) < 0) return;
  s_serialMuteUntilMs = 0;
  // HWCDC::write returns short when the host is plugged in but not reading
  // (after ~20 x TX timeout). Without the mute window a 100-line report would
  // then freeze the UI for seconds.
  const size_t done = Serial.write(reinterpret_cast<const uint8_t*>(data), len);
  if (done < len) s_serialMuteUntilMs = (::millis() + 1500) | 1u;
}
void serialWrite(const char* text) { if (text) serialWrite(text, strlen(text)); }
int  serialRead() { return Serial.available() > 0 ? Serial.read() : -1; }
bool serialHostListening() { return HWCDC::isConnected(); }
#endif

void chipInfo(ChipInfo* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  strlcpy(out->model, ESP.getChipModel(), sizeof(out->model));
  out->revMajor   = static_cast<uint8_t>(ci.revision / 100);
  out->revMinor   = static_cast<uint8_t>(ci.revision % 100);
  out->cores      = ci.cores;
  out->cpuMhz     = getCpuFrequencyMhz();
  out->flashBytes = ESP.getFlashChipSize();
  out->psramFound = psramFound();
  out->psramBytes = out->psramFound ? ESP.getPsramSize() : 0;
  strlcpy(out->idfVersion, esp_get_idf_version(), sizeof(out->idfVersion));
  strlcpy(out->coreVersion, ESP.getCoreVersion(), sizeof(out->coreVersion));
  const char* rr = "?";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   rr = "POWERON";   break;
    case ESP_RST_EXT:       rr = "EXT_PIN";   break;
    case ESP_RST_SW:        rr = "SOFTWARE";  break;
    case ESP_RST_PANIC:     rr = "PANIC";     break;
    case ESP_RST_INT_WDT:   rr = "INT_WDT";   break;
    case ESP_RST_TASK_WDT:  rr = "TASK_WDT";  break;
    case ESP_RST_WDT:       rr = "OTHER_WDT"; break;
    case ESP_RST_DEEPSLEEP: rr = "DEEPSLEEP"; break;
    case ESP_RST_BROWNOUT:  rr = "BROWNOUT";  break;
    case ESP_RST_USB:       rr = "USB";       break;
    case ESP_RST_JTAG:      rr = "JTAG";      break;
    case ESP_RST_PWR_GLITCH: rr = "PWR_GLITCH"; break;
    case ESP_RST_CPU_LOCKUP: rr = "CPU_LOCKUP"; break;
    default:                rr = "UNKNOWN";   break;
  }
  // + the chip's raw reset cause (ROM code, e.g. 1 power-on, 21 USB-UART chip
  // reset, 15 brownout, 16 RTC watchdog) so an unexpected mapping shows up.
  snprintf(out->resetReason, sizeof(out->resetReason), "%s/%d", rr, static_cast<int>(esp_rom_get_reset_reason(0)));
}

HeapInfo heap() {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  HeapInfo h;
  h.freeBytes    = heap_caps_get_free_size(caps);
  h.largestBlock = heap_caps_get_largest_free_block(caps);
  h.minEverFree  = heap_caps_get_minimum_free_size(caps);
  h.totalBytes   = heap_caps_get_total_size(caps);
  return h;
}
extern "C" {
extern int _data_start, _bss_end;   // ESP-IDF linker script (sections.ld): start of .dram0.data, end of .bss
}
uint32_t staticDramBytes() {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_bss_end) - reinterpret_cast<uintptr_t>(&_data_start));
}
uint32_t stackFreeMinSelf() { return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)); }

bool resetWasAbnormal() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:   // double exception
      return true;
    // Not abnormal: POWERON (power switch / USB plug), SW (esp_restart), EXT,
    // and USB -- the reset esptool triggers through the USB-Serial/JTAG port
    // after flashing (S3 ROM: "rst:0x15 USB_UART_CHIP_RESET") or that a
    // terminal causes by toggling DTR/RTS. esptool's RTC-watchdog reset is
    // used only for USB-OTG (TinyUSB) ports, which this firmware does not use.
    default:
      return false;
  }
}
void restart() { esp_restart(); }

void* allocInternal(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
void  freeInternal(void* p)       { heap_caps_free(p); }

Battery battery() {
  // Cardputer-Adv: plain ADC read on GPIO10 through a 2:1 divider, no I2C.
  Battery b;
  b.percent    = M5.Power.getBatteryLevel();
  b.millivolts = M5.Power.getBatteryVoltage();
  if (b.percent < 0 || b.percent > 100) b.percent = -1;
  if (b.millivolts <= 0) b.millivolts = -1;
  return b;
}

bool i2cProbe(uint8_t addr) { return M5.In_I2C.scanID(addr, 100000); }

int i2cScan(uint8_t* found, int cap) {
  int n = 0;
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    if (M5.In_I2C.scanID(a, 100000)) {
      if (found && n < cap) found[n] = a;
      ++n;
    }
  }
  return n;
}

bool i2cRead8(uint8_t addr, uint8_t reg, uint8_t* value, uint32_t hz) {
  uint8_t v = 0;
  const bool ok = M5.In_I2C.readRegister(addr, reg, &v, 1, hz);
  if (ok && value) *value = v;
  return ok;
}
bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t value, uint32_t hz) {
  return M5.In_I2C.writeRegister(addr, reg, &value, 1, hz);
}

const char* imuName() {
  switch (M5.Imu.getType()) {
    case m5::imu_none:    return "none";
    case m5::imu_bmi270:  return "BMI270";
    case m5::imu_unknown: return "unknown";
    default:              return "other";
  }
}

}  // namespace sys
}  // namespace plat
