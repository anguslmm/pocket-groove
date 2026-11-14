// Host test of the Daisy's BMI270 driver (daisy/core/bmi270.h) against a register
// model: the chip only reports "init ok" if the whole configuration file arrived at
// the right INIT_ADDR offsets after a soft reset with power save off, and then
// returns the sensor data it holds.
#include <cstring>
#include <vector>

#include "check.h"
#include "core/bmi270.h"

using namespace pgd;

namespace {

struct Model final : I2cBus {
  uint8_t addr = 0x68;
  uint8_t reg[256] = {};
  std::vector<uint8_t> cfgRam = std::vector<uint8_t>(8192, 0);
  const std::vector<uint8_t>* expect = nullptr;
  bool resetSeen = false;
  int writes = 0, maxWrite = 0;
  bool write(uint8_t a, uint8_t r, const uint8_t* d, size_t n) override {
    if (a != addr) return false;
    ++writes;
    if (int(n) > maxWrite) maxWrite = int(n);
    if (r == 0x7E && n == 1 && d[0] == 0xB6) {
      resetSeen = true;
      std::memset(reg, 0, sizeof(reg));
      reg[0x00] = 0x24;
      reg[0x7C] = 0x03;   // power save on after reset
      return true;
    }
    if (r == 0x5E) {   // INIT_DATA at INIT_ADDR (words)
      const size_t at = size_t((reg[0x5B] & 0x0F) | (reg[0x5C] << 4)) * 2;
      for (size_t i = 0; i < n && at + i < cfgRam.size(); ++i) cfgRam[at + i] = d[i];
      return true;
    }
    for (size_t i = 0; i < n; ++i) reg[uint8_t(r + i)] = d[i];
    if (r == 0x59 && d[0] == 0x01)
      reg[0x21] = (resetSeen && reg[0x7C] == 0 && expect && cfgRam == *expect) ? 0x01 : 0x02;
    return true;
  }
  bool read(uint8_t a, uint8_t r, uint8_t* d, size_t n) override {
    if (a != addr) return false;
    for (size_t i = 0; i < n; ++i) d[i] = reg[uint8_t(r + i)];
    return true;
  }
};

std::vector<uint8_t> blob() {
  std::vector<uint8_t> b(8192);
  for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(i * 7 + (i >> 9));
  return b;
}

void testInitAndRead() {
  Model m;
  m.reg[0x00] = 0x24;
  m.addr = 0x69;   // the jumper's address: found too
  const std::vector<uint8_t> cfg = blob();
  m.expect = &cfg;
  Bmi270 b;
  b.begin(&m, cfg.data(), cfg.size());
  uint32_t t = 0;
  int steps = 0;
  while (b.step(t) && steps < 1000) {
    ++t;
    ++steps;
  }
  CHECK(b.ready());
  CHECK_EQ(b.address(), 0x69);
  CHECK(steps > int(8192 / Bmi270::kChunk));   // spread over steps
  CHECK(m.maxWrite <= int(Bmi270::kChunk));
  CHECK_EQ(m.reg[0x40], 0xA9);
  CHECK_EQ(m.reg[0x7D], 0x0E);
  const int16_t acc[3] = {8192, -100, 3}, gyr[3] = {-32, 0, 32767};
  for (int i = 0; i < 3; ++i) {
    m.reg[0x0C + 2 * i] = uint8_t(acc[i]);
    m.reg[0x0D + 2 * i] = uint8_t(acc[i] >> 8);
    m.reg[0x12 + 2 * i] = uint8_t(gyr[i]);
    m.reg[0x13 + 2 * i] = uint8_t(gyr[i] >> 8);
  }
  Bmi270::Raw r;
  CHECK(b.read(&r));
  CHECK(r.acc[0] == 8192 && r.acc[1] == -100 && r.gyr[0] == -32 && r.gyr[2] == 32767);
}

void testFailures() {
  {
    Model m;   // no chip
    Bmi270 b;
    b.begin(&m, nullptr, 0);
    m.addr = 0x10;
    b.step(0);
    CHECK(b.state() == Bmi270::State::Absent);
  }
  {
    Model m;
    m.reg[0x00] = 0x24;
    Bmi270 b;
    b.begin(&m, nullptr, 0);   // no configuration file
    b.step(0);
    CHECK(b.state() == Bmi270::State::NoConfig);
  }
  {
    Model m;
    m.reg[0x00] = 0x24;
    const std::vector<uint8_t> cfg = blob();
    std::vector<uint8_t> other = cfg;
    other[4000] ^= 1;   // the chip "expects" something else: init status not ok
    m.expect = &other;
    Bmi270 b;
    b.begin(&m, cfg.data(), cfg.size());
    uint32_t t = 0;
    while (b.step(t) && t < 2000) t += 1;
    CHECK(b.state() == Bmi270::State::Failed);
    CHECK_EQ(b.internalStatus(), 0x02);
    Bmi270::Raw r;
    CHECK(!b.read(&r));
  }
}

}  // namespace

int main() {
  testInitAndRead();
  testFailures();
  return pgdt::finish("test_daisy_bmi270");
}
