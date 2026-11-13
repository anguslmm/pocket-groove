// BMI270 reader for the tilt control. See imu.h.
//
// Register values: the hardware spike's, device-tested (src/hwspike/imu.cpp has
// the sources for each), at the 100 Hz ODR the Rust BSP's tests pin:
//   ACC_CONF  0x40 = 0xA8   odr 100 Hz (0x08) | bwp normal (0x20) | filter_perf (0x80)
//   ACC_RANGE 0x41 = 0x01   +-4 g
//   GYR_CONF  0x42 = 0xE8   odr 100 Hz | bwp normal | noise_perf (0x40) | filter_perf
//   GYR_RANGE 0x43 = 0x09   +-1000 dps (bit 3: OIS range as the Rust BSP sets it)
//   PWR_CONF  0x7C = 0x00   advanced power save off
//   PWR_CTRL  0x7D = 0x0E   temperature + accelerometer + gyroscope on
// Data: 0x0C..0x17 = acc x y z, gyr x y z, little-endian int16 (12 bytes, one burst;
// the spike also read sensortime and the status bytes: 18 bytes, a third more bus time).
#include "imu.h"

#include <M5Unified.h>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace plat {
namespace imu {

namespace {

constexpr uint32_t kI2cHz = 400000;   // as the keyboard reader uses on the same bus
constexpr int kTaskPrio = 9;          // below the keyboard task (10), above the UI loop (1)
constexpr int kCore = 1;

uint8_t s_addr = 0;
uint8_t s_err = 0;
bool s_present = false;
TaskHandle_t s_task = nullptr;
volatile bool s_run = false;
Listener s_fn = nullptr;
void* s_user = nullptr;
volatile uint32_t s_reads = 0, s_errors = 0, s_busAvg = 0, s_busMax = 0, s_stack = 0;

bool wr(uint8_t reg, uint8_t v) {
  const bool ok = M5.In_I2C.writeRegister(s_addr, reg, &v, 1, kI2cHz);
  vTaskDelay(pdMS_TO_TICKS(2));   // the BMI270 wants time between writes (the Rust BSP waits 1 ms)
  return ok;
}

void task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // asleep until start()
    TickType_t last = xTaskGetTickCount();
    while (s_run) {
      uint8_t b[12];
      const int64_t t0 = esp_timer_get_time();
      const bool ok = M5.In_I2C.readRegister(s_addr, 0x0C, b, sizeof(b), kI2cHz);
      const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
      if (ok) {
        Raw r;
        for (int i = 0; i < 3; ++i) {
          r.acc[i] = static_cast<int16_t>(b[2 * i] | (b[2 * i + 1] << 8));
          r.gyr[i] = static_cast<int16_t>(b[6 + 2 * i] | (b[7 + 2 * i] << 8));
        }
        if (s_fn) s_fn(r, s_user);
        s_reads = s_reads + 1;
        s_busAvg = s_busAvg ? s_busAvg + ((static_cast<int32_t>(us) - static_cast<int32_t>(s_busAvg)) / 16) : us;
        if (us > s_busMax) s_busMax = us;
      } else {
        s_errors = s_errors + 1;
      }
      if ((s_reads & 63u) == 1u) s_stack = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
      vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / kPollHz));
    }
  }
}

}  // namespace

bool begin(Listener fn, void* user) {
  s_fn = fn;
  s_user = user;
  for (uint8_t a : {static_cast<uint8_t>(0x69), static_cast<uint8_t>(0x68)}) {
    uint8_t id = 0;
    if (M5.In_I2C.readRegister(a, 0x00, &id, 1, kI2cHz) && id == 0x24) {
      s_addr = a;
      s_present = true;
      break;
    }
  }
  if (!s_present) return false;
  bool ok = true;
  ok = wr(0x7C, 0x00) && ok;   // PWR_CONF
  ok = wr(0x41, 0x01) && ok;   // ACC_RANGE +-4 g
  ok = wr(0x40, 0xA8) && ok;   // ACC_CONF 100 Hz, performance
  ok = wr(0x43, 0x09) && ok;   // GYR_RANGE +-1000 dps
  ok = wr(0x42, 0xE8) && ok;   // GYR_CONF 100 Hz, performance
  ok = wr(0x7D, 0x0E) && ok;   // PWR_CTRL temp + acc + gyr
  uint8_t e = 0;
  M5.In_I2C.readRegister(s_addr, 0x02, &e, 1, kI2cHz);
  s_err = e;
  if (!ok) {
    s_present = false;
    return false;
  }
  if (!s_task) xTaskCreatePinnedToCore(task, "pg_imu", kTaskStack, nullptr, kTaskPrio, &s_task, kCore);
  if (!s_task) s_present = false;
  return s_present;
}

bool present() { return s_present; }

void start() {
  if (!s_present || s_run) return;
  s_run = true;
  xTaskNotifyGive(s_task);
}

void stop() { s_run = false; }

Stats stats() {
  Stats st;
  st.present = s_present;
  st.running = s_run;
  st.addr = s_addr;
  st.err = s_err;
  st.reads = s_reads;
  st.errors = s_errors;
  st.busAvgUs = s_busAvg;
  st.busMaxUs = s_busMax;
  st.stackFree = s_stack;
  return st;
}

}  // namespace imu
}  // namespace plat
