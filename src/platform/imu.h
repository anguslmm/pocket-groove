// Pocket Groove platform layer -- the BMI270 IMU as the tilt control's sensor
// (docs/UX.md 10.3 "Tilt"; docs/hardware-notes.md 11.4 "What a production
// version should do").
//
// The chip sits on the internal I2C bus with the keyboard (TCA8418, 0x34) and the
// codec (ES8311, 0x18); every transfer goes through M5.In_I2C, which locks per
// transaction, as the keyboard reader does (the spike measured no lost key
// events with the IMU polled at 200 Hz). M5.begin() (plat::sys::begin,
// cfg.internal_imu = true) has uploaded the chip's configuration file; begin()
// here sets the rate and ranges.
//
// Cost model: nothing runs while the gesture is not held. start() wakes a task
// on core 1 that reads the accelerometer and gyroscope (one 12-byte burst) every
// 1000 / kPollHz ms and hands each reading to the listener on that task; stop()
// puts it back to sleep after the read under way. The listener (the app's tilt
// tracker, src/app/tilt_input.cpp) runs the maths there.
//
// Memory: a few dozen bytes of static state; the task's stack (kTaskStack) and
// its control block come from the heap, once, at begin().
//
// No Arduino / M5 / ESP-IDF types in this header.
#pragma once
#include <cstdint>

namespace plat {
namespace imu {

constexpr uint32_t kOdrHz = 200;          // sensor output rate (BMI270 ODR code 0x09, as the spike ran it)
constexpr uint32_t kPollHz = 100;         // reads per second while held: the newest sample each time
constexpr float kAccLsbPerG = 8192.0f;    // +-4 g
constexpr float kGyrLsbPerDps = 32.768f;  // +-1000 dps
constexpr uint32_t kTaskStack = 3072;     // bytes, from the heap

struct Raw {
  int16_t acc[3];   // chip axes, kAccLsbPerG per g
  int16_t gyr[3];   // chip axes, kGyrLsbPerDps per degree/s
};

// Called on the IMU task for every reading. Keep it short (tens of microseconds).
using Listener = void (*)(const Raw& r, void* user);

// Finds the BMI270 (0x69, then 0x68; CHIP_ID 0x24), configures it and creates the
// task (asleep). False when the chip does not answer: start() then does nothing.
bool begin(Listener fn, void* user);
bool present();
void start();   // UI thread: read from now on (non-blocking)
void stop();    // UI thread: stop after the read under way (non-blocking)

struct Stats {
  bool present;
  bool running;
  uint8_t addr;        // 0x69 / 0x68, 0 = none
  uint8_t err;         // ERR_REG after configuration (0 = fine)
  uint32_t reads;      // readings delivered
  uint32_t errors;     // failed bus reads
  uint32_t busAvgUs;   // one 12-byte burst (busy-waited by M5's I2C layer), running average
  uint32_t busMaxUs;
  uint32_t stackFree;  // the task's unused stack, bytes (high-water mark)
};
Stats stats();

}  // namespace imu
}  // namespace plat
