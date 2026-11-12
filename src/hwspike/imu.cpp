// BMI270 reader. See imu.h.
//
// Register values (BMI270, read from sources in this session):
//  * addresses / names: M5Unified 0.2.29 utility/imu/BMI270_Class.hpp
//    (ACC_X_LSB 0x0C, GYR_X_LSB 0x12, SENSORTIME 0x18, INT_STATUS_1 0x1D,
//    ACC_CONF 0x40, GYR_CONF 0x42, PWR_CONF 0x7C, PWR_CTRL 0x7D, CHIP_ID 0x00
//    = 0x24, ERR_REG 0x02); ACC_RANGE 0x41 / GYR_RANGE 0x43 from the Rust
//    BSP (tszarzynski/cardputer-adv @2de5578 src/bmi270.rs, device-tested).
//  * ACC_CONF = odr | bwp 2 (0x20) | filter_perf (0x80); GYR_CONF = odr |
//    0x20 | 0x80 | noise_perf (0x40): the Rust BSP's "performance" mode, whose
//    tests pin 100 Hz to 0xA8 / 0xE8 "as the datasheet's examples". ODR codes
//    0x08 = 100 Hz, 0x09 = 200, 0x0A = 400 (Rust BSP and espp bmi270_detail.hpp).
//  * ACC_RANGE 0x01 = +-4 g (espp: 0x00 2g, 0x01 4g, 0x02 8g, 0x03 16g).
//  * GYR_RANGE: low bits 0x00 2000, 0x01 1000, 0x02 500 dps (espp), bit 3 =
//    OIS range 2000 as the Rust BSP sets it: 0x09 = +-1000 dps.
//  * PWR_CTRL 0x0E = temperature + accel + gyro on (M5Unified's auxSetupMode
//    leaves this; Rust: acc 1<<2, gyr 1<<1). PWR_CONF 0x00 = advanced power
//    save off (M5Unified writes it in begin()).
//  * Sensortime LSB: 39.0625 us is the BMI270 datasheet figure as I remember
//    it, NOT read from a source here; the report therefore prints raw ticks per
//    sample as well, and the derived rate is labelled as using that value.
#include "imu.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace hws {
namespace imu {

namespace {

constexpr uint32_t kI2cHz = 400000;    // as the keyboard reader uses on the same bus
constexpr int kTaskPrio = 9;           // below the keyboard task (10), above the UI loop (1)
constexpr uint32_t kTaskStack = 4096;

uint8_t s_addr = 0;
bool s_present = false;
uint32_t s_rate = 200;
uint32_t s_poll = 200;
Regs s_regs = {};
TaskHandle_t s_task = nullptr;
esp_timer_handle_t s_timer = nullptr;
volatile bool s_running = false;
Listener volatile s_fn = nullptr;
void* volatile s_user = nullptr;

volatile uint32_t s_polls = 0, s_fresh = 0, s_err = 0, s_i2cMax = 0, s_stack = 0;
volatile float s_i2cAvg = 0, s_taskAvg = 0, s_lisAvg = 0;
volatile bool s_reset = false;

bool rd(uint8_t reg, uint8_t* buf, size_t n) { return M5.In_I2C.readRegister(s_addr, reg, buf, n, kI2cHz); }
bool wr(uint8_t reg, uint8_t v) {
  const bool ok = M5.In_I2C.writeRegister(s_addr, reg, &v, 1, kI2cHz);
  delayMicroseconds(1000);   // the Rust BSP waits between writes (BMI270 needs time in normal mode)
  return ok;
}

uint8_t odrCode(uint32_t hz) {
  if (hz >= 400) return 0x0A;
  if (hz >= 200) return 0x09;
  return 0x08;
}

void onTimer(void*) {
  if (s_task) xTaskNotifyGive(s_task);
}

void imuTask(void*) {
  const float cpu = static_cast<float>(esp_rom_get_cpu_ticks_per_us());
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!s_running) continue;
    if (s_reset) {
      s_reset = false;
      s_polls = 0; s_fresh = 0; s_err = 0; s_i2cMax = 0; s_i2cAvg = 0; s_taskAvg = 0; s_lisAvg = 0;
    }
    const int64_t t0 = esp_timer_get_time();
    uint8_t b[18];
    const bool ok = rd(0x0C, b, sizeof(b));
    const int64_t t1 = esp_timer_get_time();
    if (!ok) {
      s_err = s_err + 1;
      continue;
    }
    Sample s;
    for (int i = 0; i < 3; ++i) {
      s.acc[i] = static_cast<int16_t>(b[2 * i] | (b[2 * i + 1] << 8));
      s.gyr[i] = static_cast<int16_t>(b[6 + 2 * i] | (b[7 + 2 * i] << 8));
    }
    s.sensortime = static_cast<uint32_t>(b[12]) | (static_cast<uint32_t>(b[13]) << 8) |
                   (static_cast<uint32_t>(b[14]) << 16);
    s.intStatus1 = b[17];
    s.freshAcc = (b[17] & 0x80) != 0;
    s.tUs = static_cast<uint32_t>(t1);
    s.i2cUs = static_cast<uint32_t>(t1 - t0);
    if (s.i2cUs > s_i2cMax) s_i2cMax = s.i2cUs;
    const float ai = s_i2cAvg;
    s_i2cAvg = (ai == 0.0f) ? static_cast<float>(s.i2cUs) : ai + (static_cast<float>(s.i2cUs) - ai) * (1.0f / 64.0f);
    s_polls = s_polls + 1;
    if (s.freshAcc) s_fresh = s_fresh + 1;

    const uint32_t c0 = esp_cpu_get_cycle_count();
    Listener fn = s_fn;
    if (fn) fn(s, s_user);
    const uint32_t lc = esp_cpu_get_cycle_count() - c0;
    const float al = s_lisAvg;
    s_lisAvg = (al == 0.0f) ? static_cast<float>(lc) : al + (static_cast<float>(lc) - al) * (1.0f / 64.0f);
    const float tu = static_cast<float>(t1 - t0) + static_cast<float>(lc) / cpu;
    const float at = s_taskAvg;
    s_taskAvg = (at == 0.0f) ? tu : at + (tu - at) * (1.0f / 64.0f);
    if ((s_polls & 0xFF) == 0) s_stack = uxTaskGetStackHighWaterMark(nullptr);
  }
}

}  // namespace

bool begin() {
  for (uint8_t a : {static_cast<uint8_t>(0x69), static_cast<uint8_t>(0x68)}) {
    uint8_t id = 0;
    if (M5.In_I2C.readRegister(a, 0x00, &id, 1, kI2cHz) && id == 0x24) {
      s_addr = a;
      s_present = true;
      break;
    }
  }
  s_regs.addr = s_addr;
  if (!s_task) xTaskCreatePinnedToCore(imuTask, "hws_imu", kTaskStack, nullptr, kTaskPrio, &s_task, 1);
  if (!s_timer) {
    esp_timer_create_args_t a = {};
    a.callback = onTimer;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "hws_imu";
    esp_timer_create(&a, &s_timer);
  }
  return s_present;
}

bool present() { return s_present; }

bool configure(uint32_t hz, bool poll2x) {
  if (!s_present) return false;
  const bool was = s_running;
  setRunning(false);
  s_rate = (hz >= 400) ? 400 : (hz >= 200 ? 200 : 100);
  s_poll = poll2x ? 2 * s_rate : s_rate;
  const uint8_t odr = odrCode(s_rate);
  bool ok = true;
  ok = wr(0x7C, 0x00) && ok;                                          // PWR_CONF: advanced power save off
  ok = wr(0x41, 0x01) && ok;                                          // ACC_RANGE +-4 g
  ok = wr(0x40, static_cast<uint8_t>(odr | 0x20 | 0x80)) && ok;       // ACC_CONF
  ok = wr(0x43, 0x09) && ok;                                          // GYR_RANGE +-1000 dps (OIS bit as Rust BSP)
  ok = wr(0x42, static_cast<uint8_t>(odr | 0x20 | 0x80 | 0x40)) && ok; // GYR_CONF
  ok = wr(0x7D, 0x0E) && ok;                                          // PWR_CTRL: temp + acc + gyr
  delay(50);                                                          // gyro start-up
  uint8_t v = 0;
  s_regs.addr = s_addr;
  rd(0x00, &v, 1); s_regs.chipId = v;
  rd(0x02, &v, 1); s_regs.err = v;
  rd(0x40, &v, 1); s_regs.accConf = v;
  rd(0x41, &v, 1); s_regs.accRange = v;
  rd(0x42, &v, 1); s_regs.gyrConf = v;
  rd(0x43, &v, 1); s_regs.gyrRange = v;
  rd(0x7C, &v, 1); s_regs.pwrConf = v;
  rd(0x7D, &v, 1); s_regs.pwrCtrl = v;
  if (was) setRunning(true);
  return ok;
}

uint32_t rateHz() { return s_rate; }
uint32_t pollHz() { return s_poll; }

void setRunning(bool on) {
  if (!s_timer || !s_present) {
    s_running = false;
    return;
  }
  if (on == s_running) return;
  if (on) {
    s_running = true;
    esp_timer_start_periodic(s_timer, 1000000ull / s_poll);
  } else {
    esp_timer_stop(s_timer);
    s_running = false;
    delay(5);   // let a poll in progress finish
  }
}

bool isRunning() { return s_running; }
Regs regs() { return s_regs; }
float accLsbPerG() { return 32768.0f / 4.0f; }
float gyrLsbPerDps() { return 32768.0f / 1000.0f; }

float readOneByteUs() {
  if (!s_present) return 0;
  uint8_t v = 0;
  const int64_t t0 = esp_timer_get_time();
  M5.In_I2C.readRegister(s_addr, 0x03, &v, 1, kI2cHz);
  return static_cast<float>(esp_timer_get_time() - t0);
}

void setListener(Listener fn, void* user) {
  s_fn = nullptr;
  s_user = user;
  s_fn = fn;
}

Stats stats() {
  Stats st;
  st.polls = s_polls;
  st.fresh = s_fresh;
  st.i2cErrors = s_err;
  st.i2cAvgUs = s_i2cAvg;
  st.i2cMaxUs = static_cast<float>(s_i2cMax);
  st.taskAvgUs = s_taskAvg;
  st.listenerAvgCyc = s_lisAvg;
  st.stackFree = s_stack;
  return st;
}

void resetStats() { s_reset = true; }

}  // namespace imu
}  // namespace hws
