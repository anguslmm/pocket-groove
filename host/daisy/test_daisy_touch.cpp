// Host test of the 2090's touch path (daisy/core/touch.h) and the shared I2C bus
// (daisy/core/i2c_bus.h), against a register model of the FT6206 and a BMI270 on the
// same simulated bus:
//   parse     the register bytes as Adafruit_FT6206::readData reads them: one and two
//             points, the event flag and touch id, a count over 2 (bus junk) as 0
//   driver    probe (vendor 0x11, chip 0x06 / 0x36 / 0x64; the threshold written),
//             down / move / up events at the poll rate, both devices on one bus
//   absent    nothing at 0x38: Absent at once (a NAK), probed again every 2 s, found
//             when it appears; a device that goes away mid-touch: Lost, with an up
//   mapping   the native portrait coordinates to the landscape panel for both
//             rotations (Adafruit's CapTouch_onoffbutton at rotation 1: x = 320 - ty,
//             y = tx) and into the UI canvas (offset 40 / 52, scale 1)
//   guard     a stuck bus (every call times out after 25 ms): the first slow failure
//             stalls the guard, later calls fail at once for a second, so the main
//             loop never waits more than one timeout per second; then it tries again
#include <cstring>
#include <vector>

#include "check.h"
#include "core/bmi270.h"
#include "core/i2c_bus.h"
#include "core/touch.h"

using namespace pgd;

namespace {

uint32_t g_us = 0;   // the simulated clock
uint32_t clockUs(void*) { return g_us; }

struct SimBus final : I2cBus {
  bool touchThere = true, imuThere = true, stuck = false;
  uint8_t ft[256] = {};
  uint8_t bmiId = 0x24;
  int calls = 0, touchReads = 0;
  SimBus() {
    ft[0xA8] = 0x11;
    ft[0xA3] = 0x06;
    ft[0xA6] = 0x03;
  }
  bool write(uint8_t addr, uint8_t reg, const uint8_t* d, size_t n) override {
    ++calls;
    if (stuck) {
      g_us += 25000;
      return false;
    }
    g_us += 50;
    if (addr == 0x38 && touchThere) {
      for (size_t i = 0; i < n; ++i) ft[(reg + i) & 0xFF] = d[i];
      return true;
    }
    return addr == 0x68 && imuThere;
  }
  bool read(uint8_t addr, uint8_t reg, uint8_t* d, size_t n) override {
    ++calls;
    if (stuck) {
      g_us += 25000;
      return false;
    }
    g_us += 50 + uint32_t(n) * 25;
    if (addr == 0x38 && touchThere) {
      for (size_t i = 0; i < n; ++i) d[i] = ft[(reg + i) & 0xFF];
      if (reg == 0x00) ++touchReads;
      return true;
    }
    if (addr == 0x68 && imuThere) {
      std::memset(d, 0, n);
      if (reg == 0x00) d[0] = bmiId;
      return true;
    }
    return false;   // NAK
  }
  void touch(int n, uint16_t x, uint16_t y, uint8_t ev = 2) {
    ft[0x02] = uint8_t(n);
    ft[0x03] = uint8_t((ev << 6) | ((x >> 8) & 0x0F));
    ft[0x04] = uint8_t(x);
    ft[0x05] = uint8_t((0 << 4) | ((y >> 8) & 0x0F));
    ft[0x06] = uint8_t(y);
  }
};

void testParse() {
  uint8_t r[15] = {};
  r[0x02] = 1;
  r[0x03] = 0x80 | 0x01;   // contact, x high nibble 1
  r[0x04] = 0x2C;          // x = 0x12C = 300? (beyond the panel: parse does not clamp)
  r[0x05] = 0x10 | 0x00;   // id 1
  r[0x06] = 0xC8;          // y = 200
  r[0x09] = 0x40 | 0x00;   // the second point: lift up
  r[0x0A] = 10;
  r[0x0B] = 0x00 | 0x01;
  r[0x0C] = 0x02;          // y = 258
  TouchFrame f;
  CHECK(Ft6206::parse(r, &f));
  CHECK_EQ(f.count, 1);
  CHECK_EQ(f.p[0].x, 300);
  CHECK_EQ(f.p[0].y, 200);
  CHECK_EQ(f.p[0].id, 1);
  CHECK_EQ(f.p[0].event, 2);
  CHECK_EQ(f.p[1].x, 10);
  CHECK_EQ(f.p[1].y, 258);
  CHECK_EQ(f.p[1].event, 1);
  r[0x02] = 2;
  CHECK(Ft6206::parse(r, &f) && f.count == 2);
  r[0x02] = 0xFF;   // what an unpowered bus reads
  CHECK(!Ft6206::parse(r, &f));
  CHECK_EQ(f.count, 0);
  r[0x02] = 3;
  Ft6206::parse(r, &f);
  CHECK_EQ(f.count, 0);
  CHECK(Ft6206::chipKnown(0x06) && Ft6206::chipKnown(0x36) && Ft6206::chipKnown(0x64) && !Ft6206::chipKnown(0x00));
}

void testDriverAndSharedBus() {
  g_us = 0;
  SimBus bus;
  I2cGuard guard;
  guard.init(&bus, clockUs, nullptr);
  Ft6206 t;
  Bmi270 imu;
  imu.begin(&guard, nullptr, 0);   // no configuration file: it stops at NoConfig after its probe
  t.begin(&guard, 0);
  uint32_t ms = 0;
  for (; ms < 100; ++ms) {
    imu.step(ms);
    t.step(ms);
  }
  CHECK(t.present());
  CHECK_EQ(t.chipId(), 0x06);
  CHECK_EQ(t.firmware(), 0x03);
  CHECK_EQ(bus.ft[0x80], Ft6206::kThreshold);
  CHECK(imu.state() == Bmi270::State::NoConfig);   // the IMU answered on the same bus
  const int reads0 = bus.touchReads;
  CHECK(reads0 >= 5 && reads0 <= 7);   // ~60 Hz over the 100 ms
  Ft6206::Event e;
  CHECK(!t.pop(&e));
  bus.touch(1, 100, 200);
  for (int i = 0; i < 20; ++i) t.step(ms++);
  CHECK(t.pop(&e) && e.kind == Ft6206::Event::Down && e.x == 100 && e.y == 200);
  CHECK(!t.pop(&e));   // held still: no moves
  bus.touch(1, 110, 205);
  for (int i = 0; i < 20; ++i) t.step(ms++);
  CHECK(t.pop(&e) && e.kind == Ft6206::Event::Move && e.x == 110 && e.y == 205);
  bus.touch(0, 0, 0);
  for (int i = 0; i < 20; ++i) t.step(ms++);
  CHECK(t.pop(&e) && e.kind == Ft6206::Event::Up && e.x == 110 && e.y == 205);
  CHECK_EQ(t.touches(), 1u);
  CHECK_EQ(t.errors(), 0u);
  // the device goes away mid-touch: three failed reads, Lost, an up for the touch
  bus.touch(1, 50, 60);
  for (int i = 0; i < 20; ++i) t.step(ms++);
  CHECK(t.pop(&e) && e.kind == Ft6206::Event::Down);
  bus.touchThere = false;
  for (int i = 0; i < 80; ++i) t.step(ms++);
  CHECK(t.state() == Ft6206::State::Lost);
  CHECK(t.pop(&e) && e.kind == Ft6206::Event::Up);
  // back: found again at the next probe (2 s)
  bus.touchThere = true;
  bus.touch(0, 0, 0);
  for (int i = 0; i < 2100; ++i) t.step(ms++);
  CHECK(t.present());
}

void testAbsent() {
  g_us = 0;
  SimBus bus;
  bus.touchThere = false;
  I2cGuard guard;
  guard.init(&bus, clockUs, nullptr);
  Ft6206 t;
  t.begin(&guard, 0);
  const uint32_t us0 = g_us;
  t.step(0);
  CHECK(t.state() == Ft6206::State::Absent);
  CHECK(g_us - us0 < 1000);   // a NAK costs microseconds
  const int c0 = bus.calls;
  for (uint32_t ms = 1; ms < 1999; ++ms) t.step(ms);
  CHECK_EQ(bus.calls, c0);    // no bus traffic until the next probe
  bus.touchThere = true;
  for (uint32_t ms = 1999; ms < 2100; ++ms) t.step(ms);
  CHECK(t.present());
  CHECK(!guard.stalled());
  // a wrong chip at 0x38 is not taken for the touch controller
  SimBus other;
  other.ft[0xA8] = 0x00;
  I2cGuard g2;
  g2.init(&other, clockUs, nullptr);
  Ft6206 t2;
  t2.begin(&g2, 0);
  t2.step(0);
  CHECK(t2.state() == Ft6206::State::Absent);
}

void testMapping() {
  TouchMap m;   // the 2090 at MADCTL 0x28: orientation 3
  CHECK_EQ(TouchMap::orientationFor(0x28), 3);
  CHECK_EQ(TouchMap::orientationFor(0xE8), 5);
  CHECK_EQ(TouchMap::orientationFor(0x48), 6);
  CHECK_EQ(TouchMap::orientationFor(0x88), 0);
  int px, py, cx, cy;
  // Adafruit's rotation-1 formula: x = 320 - ty, y = tx (here 319 - ty: pixel indices)
  for (int tx : {0, 37, 120, 239})
    for (int ty : {0, 99, 160, 319}) {
      m.mapToPanel(tx, ty, &px, &py);
      CHECK(px == 319 - ty && py == tx);
    }
  m.mapToPanel(120, 160, &px, &py);   // the panel's centre ...
  m.toCanvas(px, py, &cx, &cy);       // ... is the canvas' centre
  CHECK(cx == 119 && cy == 68);
  m.mapToPanel(239, 0, &px, &py);     // the panel's bottom right corner: outside the canvas
  m.toCanvas(px, py, &cx, &cy);
  CHECK(cx == 279 && cy == 187);
  m.toCanvas(0, 0, &cx, &cy);         // top left of the panel: negative canvas coordinates
  CHECK(cx == -40 && cy == -52);
  m.toCanvas(40, 52, &cx, &cy);
  CHECK(cx == 0 && cy == 0);
  // turned over (0xE8): the same touch lands 180 degrees round
  m.orientation = uint8_t(TouchMap::orientationFor(0xE8));
  m.mapToPanel(10, 20, &px, &py);
  CHECK(px == 20 && py == 239 - 10);
  // out-of-range raw values are clamped to the panel
  m.orientation = 3;
  m.mapToPanel(400, 500, &px, &py);
  CHECK(px == 0 && py == 239);
  // a scaled panel (480x320 at 2): canvas = (panel - offset) / 2
  TouchMap big;
  big.x0 = 0;
  big.y0 = 25;
  big.scale = 2;
  big.toCanvas(479, 294, &cx, &cy);
  CHECK(cx == 239 && cy == 134);
}

void testGuard() {
  g_us = 0;
  SimBus bus;
  bus.stuck = true;
  I2cGuard guard;
  guard.init(&bus, clockUs, nullptr);
  Ft6206 t;
  Bmi270 imu;
  t.begin(&guard, 0);
  imu.begin(&guard, nullptr, 0);
  // one second of main loop passes (1 ms each): only the first call waits for the timeout
  uint32_t worst = 0;
  for (uint32_t ms = 0; ms < 990; ++ms) {
    const uint32_t before = g_us;
    t.step(ms);
    if (imu.state() == Bmi270::State::Probe) imu.step(ms);
    const uint32_t dt = g_us - before;
    worst = dt > worst ? dt : worst;
    g_us = before + (dt > 1000 ? dt : 1000);   // the pass takes at least 1 ms
  }
  CHECK(guard.stalled());
  CHECK_EQ(guard.stalls(), 1u);
  CHECK_EQ(bus.calls, 1);   // everything after the first call refused without touching the bus
  CHECK(guard.skipped() > 0);
  CHECK(worst <= 25000u);
  CHECK(t.state() == Ft6206::State::Absent);
  CHECK(imu.state() == Bmi270::State::Absent);
  // the bus comes free: after the hold, the next probe goes through
  bus.stuck = false;
  g_us += 1000000;
  for (uint32_t ms = 2000; ms < 2100; ++ms) t.step(ms);
  CHECK(!guard.stalled());
  CHECK(t.present());
}

}  // namespace

int main() {
  testParse();
  testDriverAndSharedBus();
  testAbsent();
  testMapping();
  testGuard();
  return pgdt::finish("test_daisy_touch");
}
