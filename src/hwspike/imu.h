// Pocket Groove hardware spike -- BMI270 reader on the internal I2C bus
// (shared with the TCA8418 keyboard 0x34 and the ES8311 codec 0x18).
//
// M5.begin() (plat::sys::begin, cfg.internal_imu = true) already uploads the
// BMI270's 8 KB configuration file (M5Unified 0.2.29 BMI270_Class::begin).
// This module then sets ODR / ranges / filter mode itself and polls the data
// registers from a task on core 1, driven by an esp_timer at the poll rate:
// one 18-byte burst from 0x0C = acc xyz, gyr xyz, sensortime (3 bytes),
// EVENT, INT_STATUS_0, INT_STATUS_1 (bit 7 accel data ready, bit 6 gyro, as
// M5Unified's getImuRawData reads them).
//
// No Arduino / ESP types in this header.
#pragma once
#include <cstdint>

namespace hws {
namespace imu {

struct Sample {
  int16_t  acc[3];
  int16_t  gyr[3];
  uint32_t sensortime;   // 24-bit chip counter
  uint8_t  intStatus1;
  bool     freshAcc;     // INT_STATUS_1 bit 7: new accelerometer data since the last read
  uint32_t tUs;          // esp_timer time of the read (end of the I2C transaction)
  uint32_t i2cUs;        // duration of the burst read
};

struct Regs {
  uint8_t addr, chipId, err, accConf, accRange, gyrConf, gyrRange, pwrConf, pwrCtrl;
};

struct Stats {
  uint32_t polls, fresh, i2cErrors;
  float    i2cAvgUs, i2cMaxUs;
  float    taskAvgUs;      // whole poll incl. listener (core 1 busy time; M5GFX's I2C polls the bus)
  float    listenerAvgCyc; // the listener alone (the maths)
  uint32_t stackFree;
};

// Finds the BMI270 (0x69, then 0x68; CHIP_ID 0x24). Starts the poll task (stopped).
bool begin();
bool present();
// ODR 100/200/400 Hz; polls at the same rate, or twice as often with
// poll2x (fewer lost samples, twice the bus traffic). Accel +-4 g, gyro
// +-1000 dps, performance filter mode (register values from the
// device-tested Rust BSP).
bool configure(uint32_t rateHz, bool poll2x);
uint32_t rateHz();
uint32_t pollHz();
void setRunning(bool on);
bool isRunning();
Regs regs();                        // read back at the last configure()
float accLsbPerG();
float gyrLsbPerDps();
float readOneByteUs();              // a 1-byte register read, timed (UI thread)

// Called on the IMU task after every poll (fresh or not). Keep it short.
using Listener = void (*)(const Sample& s, void* user);
void setListener(Listener fn, void* user);

Stats stats();
void resetStats();

}  // namespace imu
}  // namespace hws
