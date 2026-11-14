// The BMI270 for tilt on the Daisy (stage 1: a SparkFun BMI270 Qwiic board on an I2C
// lead, address 0x68, or 0x69 with its jumper; docs/DAISY.md). A main-loop state
// machine over an I2C bus: probe the chip id, soft reset, upload Bosch's 8 KB
// configuration file in chunks (one chunk per step, so the upload never holds the
// loop for long), start it and wait for INTERNAL_STATUS = "init ok", then set the
// same sensor configuration the Cardputer uses (src/platform/imu.cpp: 200 Hz, +-4 g,
// +-1000 dps, performance mode). Then read() gives accelerometer + gyroscope (12
// bytes at 0x0C) whenever the caller polls (100 Hz while G0 is held, as there).
//
// The configuration file is Bosch Sensortec's (BMI270_SensorAPI bmi270.c
// bmi270_config_file[], BSD-3-Clause, the same 8192 bytes M5Unified uploads on the
// Cardputer); tools/daisy-setup.sh extracts it at a pinned commit into
// daisy/vendor/ (not in git). Without it, the driver stops at NoConfig and tilt is
// off. Register addresses: the BMI270 datasheet names as M5Unified's BMI270_Class.hpp
// and the Cardputer driver use them.
// Portable (host/daisy/test_daisy_bmi270.cpp, against a register model).
#pragma once
#include <cstddef>
#include <cstdint>

#include "i2c_bus.h"

namespace pgd {

class Bmi270 {
 public:
  enum class State : uint8_t { Off, Probe, Reset, Upload, Start, WaitInit, Configure, Ready, Absent, NoConfig, Failed };
  static constexpr uint8_t kChipId = 0x24;
  static constexpr size_t kChunk = 64;          // config bytes per I2C write
  static constexpr uint32_t kInitTimeoutMs = 50;
  struct Raw {
    int16_t acc[3], gyr[3];
  };

  void begin(I2cBus* bus, const uint8_t* config, size_t configBytes);
  // Main loop. Returns true while the setup is still under way.
  bool step(uint32_t nowMs);
  bool ready() const { return st_ == State::Ready; }
  State state() const { return st_; }
  static const char* stateName(State s);
  uint8_t address() const { return addr_; }
  uint8_t errReg() const { return err_; }
  uint8_t internalStatus() const { return status_; }
  bool read(Raw* out);
  uint32_t reads() const { return reads_; }
  uint32_t errors() const { return errors_; }

 private:
  bool wr(uint8_t reg, uint8_t v) { return bus_->write(addr_, reg, &v, 1); }
  I2cBus* bus_ = nullptr;
  const uint8_t* cfg_ = nullptr;
  size_t cfgN_ = 0, pos_ = 0;
  State st_ = State::Off;
  uint8_t addr_ = 0x68, err_ = 0, status_ = 0;
  uint32_t t_ = 0, reads_ = 0, errors_ = 0;
  bool timed_ = false;
};

}  // namespace pgd
