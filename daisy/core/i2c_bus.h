// I2C1 on the Daisy (stage 1): one bus shared by the BMI270 (SparkFun Qwiic board on a
// lead, 0x68) and the 2090's FT6206 touch controller (0x38). Both drivers are
// main-loop state machines over I2cBus, one transaction at a time, so they never
// overlap. The device's transactions block (libDaisy's I2CHandle) with a short
// timeout; I2cGuard makes sure a broken bus cannot hold the main loop:
//   a missing device NAKs its address: the call fails at once (microseconds) and the
//   driver backs off on its own (the touch driver re-probes every 2 s);
//   a stuck bus (SDA held low, no pull-ups, a device hung mid-transfer) makes every
//   call run into its timeout (the STM32 HAL also waits up to 25 ms for the busy
//   flag): the first call that fails SLOWLY (>= kSlowUs) stalls the guard, and every
//   call for the next kHoldMs fails at once without touching the bus. Then it tries
//   again.
// Portable (host/daisy/test_daisy_touch.cpp: a slow simulated bus).
#pragma once
#include <cstddef>
#include <cstdint>

namespace pgd {

class I2cBus {
 public:
  virtual bool write(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) = 0;
  virtual bool read(uint8_t addr, uint8_t reg, uint8_t* data, size_t n) = 0;

 protected:
  ~I2cBus() = default;
};

class I2cGuard final : public I2cBus {
 public:
  static constexpr uint32_t kSlowUs = 2000;   // a failure this slow is a timeout, not a NAK
  static constexpr uint32_t kHoldMs = 1000;
  using Clock = uint32_t (*)(void* user);      // microseconds

  void init(I2cBus* raw, Clock us, void* user) {
    raw_ = raw;
    us_ = us;
    user_ = user;
  }
  bool write(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) override {
    if (!enter()) return false;
    const uint32_t t0 = us_(user_);
    return leave(raw_->write(addr, reg, data, n), t0);
  }
  bool read(uint8_t addr, uint8_t reg, uint8_t* data, size_t n) override {
    if (!enter()) return false;
    const uint32_t t0 = us_(user_);
    return leave(raw_->read(addr, reg, data, n), t0);
  }
  bool stalled() const { return stalled_; }
  uint32_t errors() const { return errors_; }     // failed transactions (NAKs included)
  uint32_t stalls() const { return stalls_; }     // slow failures (the bus held)
  uint32_t skipped() const { return skipped_; }   // calls refused while stalled
  uint32_t ok() const { return ok_; }
  uint32_t maxUs() const { return maxUs_; }       // the longest transaction

 private:
  bool enter() {
    if (!raw_) return false;
    if (stalled_) {
      if (us_(user_) - stallUs_ < kHoldMs * 1000u) {
        ++skipped_;
        return false;
      }
      stalled_ = false;
    }
    return true;
  }
  bool leave(bool good, uint32_t t0) {
    const uint32_t dt = us_(user_) - t0;
    if (dt > maxUs_) maxUs_ = dt;
    if (good) {
      ++ok_;
      return true;
    }
    ++errors_;
    if (dt >= kSlowUs) {
      stalled_ = true;
      stallUs_ = us_(user_);
      ++stalls_;
    }
    return false;
  }
  I2cBus* raw_ = nullptr;
  Clock us_ = nullptr;
  void* user_ = nullptr;
  bool stalled_ = false;
  uint32_t stallUs_ = 0;
  uint32_t errors_ = 0, stalls_ = 0, skipped_ = 0, ok_ = 0, maxUs_ = 0;
};

}  // namespace pgd
