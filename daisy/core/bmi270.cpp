#include "bmi270.h"

#include <initializer_list>

namespace pgd {

namespace {
constexpr uint8_t kRegChipId = 0x00, kRegErr = 0x02, kRegData = 0x0C, kRegStatus = 0x21;
constexpr uint8_t kRegInitCtrl = 0x59, kRegInitAddr0 = 0x5B, kRegInitData = 0x5E;
constexpr uint8_t kRegAccConf = 0x40, kRegAccRange = 0x41, kRegGyrConf = 0x42, kRegGyrRange = 0x43;
constexpr uint8_t kRegPwrConf = 0x7C, kRegPwrCtrl = 0x7D, kRegCmd = 0x7E;
constexpr uint8_t kSoftReset = 0xB6;
}  // namespace

const char* Bmi270::stateName(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::Probe: return "probing";
    case State::Reset: return "resetting";
    case State::Upload: return "uploading its configuration";
    case State::Start: return "starting";
    case State::WaitInit: return "waiting for init";
    case State::Configure: return "configuring";
    case State::Ready: return "ready";
    case State::Absent: return "not found (no chip id 0x24 at 0x68 / 0x69)";
    case State::NoConfig: return "no configuration file (tools/daisy-setup.sh fetches Bosch's)";
    case State::Failed: return "FAILED (init status not ok, or bus errors)";
  }
  return "?";
}

void Bmi270::begin(I2cBus* bus, const uint8_t* config, size_t configBytes) {
  bus_ = bus;
  cfg_ = config;
  cfgN_ = configBytes;
  pos_ = 0;
  st_ = bus ? State::Probe : State::Off;
  timed_ = false;
}

bool Bmi270::step(uint32_t nowMs) {
  if (!bus_) return false;
  switch (st_) {
    case State::Probe: {
      for (uint8_t a : {uint8_t(0x68), uint8_t(0x69)}) {
        uint8_t id = 0;
        if (bus_->read(a, kRegChipId, &id, 1) && id == kChipId) {
          addr_ = a;
          if (!cfg_ || cfgN_ == 0 || (cfgN_ & 1)) {
            st_ = State::NoConfig;
            return false;
          }
          if (!wr(kRegCmd, kSoftReset)) break;
          t_ = nowMs;
          st_ = State::Reset;
          return true;
        }
      }
      if (st_ == State::Probe) st_ = State::Absent;
      return false;
    }
    case State::Reset:
      if (nowMs - t_ < 3) return true;   // 2 ms after a soft reset (datasheet; M5Unified waits 2 ms + a tick)
      if (!wr(kRegPwrConf, 0x00) || !wr(kRegInitCtrl, 0x00)) {
        st_ = State::Failed;
        return false;
      }
      pos_ = 0;
      st_ = State::Upload;
      return true;
    case State::Upload: {
      if (pos_ >= cfgN_) {
        st_ = State::Start;
        return true;
      }
      const size_t n = cfgN_ - pos_ < kChunk ? cfgN_ - pos_ : kChunk;
      // INIT_ADDR in 16-bit words: bits 0..3 in INIT_ADDR_0, the rest in INIT_ADDR_1.
      const uint8_t a[2] = {uint8_t((pos_ / 2) & 0x0F), uint8_t((pos_ / 2) >> 4)};
      if (!bus_->write(addr_, kRegInitAddr0, a, 2) || !bus_->write(addr_, kRegInitData, cfg_ + pos_, n)) {
        st_ = State::Failed;
        return false;
      }
      pos_ += n;
      return true;
    }
    case State::Start:
      if (!wr(kRegInitCtrl, 0x01)) {
        st_ = State::Failed;
        return false;
      }
      t_ = nowMs;
      st_ = State::WaitInit;
      return true;
    case State::WaitInit: {
      if (!bus_->read(addr_, kRegStatus, &status_, 1)) {
        st_ = State::Failed;
        return false;
      }
      if ((status_ & 0x0F) == 0x01) {
        st_ = State::Configure;
        return true;
      }
      if (nowMs - t_ > kInitTimeoutMs) {
        st_ = State::Failed;
        return false;
      }
      return true;
    }
    case State::Configure: {
      // The Cardputer's values (src/platform/imu.cpp): 200 Hz, +-4 g, +-1000 dps.
      const bool ok = wr(kRegPwrConf, 0x00) && wr(kRegAccRange, 0x01) && wr(kRegAccConf, 0xA9) &&
                      wr(kRegGyrRange, 0x09) && wr(kRegGyrConf, 0xE9) && wr(kRegPwrCtrl, 0x0E);
      bus_->read(addr_, kRegErr, &err_, 1);
      st_ = ok && err_ == 0 ? State::Ready : State::Failed;
      return false;
    }
    default:
      return false;
  }
}

bool Bmi270::read(Raw* out) {
  if (st_ != State::Ready) return false;
  uint8_t b[12];
  if (!bus_->read(addr_, kRegData, b, sizeof(b))) {
    ++errors_;
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    out->acc[i] = int16_t(b[2 * i] | (b[2 * i + 1] << 8));
    out->gyr[i] = int16_t(b[6 + 2 * i] | (b[7 + 2 * i] << 8));
  }
  ++reads_;
  return true;
}

}  // namespace pgd
