// Host tests for the Daisy firmware's portable pieces (daisy/core): the byte ring,
// the main-loop scheduler, the key script, the serial demux, the load meter, the LED
// patterns, the screen mirror's encoding, the pots' pickup, and the QSPI layout /
// pin map / linker script agreeing with each other.
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "core/fb_stream.h"
#include "core/key_script.h"
#include "core/layout.h"
#include "core/led.h"
#include "core/load_meter.h"
#include "core/pins.h"
#include "core/pots.h"
#include "core/ring.h"
#include "core/scheduler.h"
#include "core/serial_link.h"
#include "pg/io/xfer.h"
#include "pg/ui/keys.h"

using namespace pgd;

namespace {

void testRing() {
  uint8_t mem[16];
  ByteRing r(mem, 20);   // rounded down to 16
  CHECK_EQ(r.capacity(), 16u);
  const uint8_t a[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  CHECK(r.push(a, 10));
  CHECK(!r.push(a, 7));          // all or nothing
  CHECK_EQ(r.pushSome(a, 10), 6u);
  CHECK_EQ(r.size(), 16u);
  uint8_t out[32];
  CHECK_EQ(r.pop(out, 12), 12u);
  CHECK_EQ(out[0], 1);
  CHECK_EQ(out[11], 2);   // 1..10 then 1..6: index 11 is the second 2
  CHECK(r.push(a, 10));   // wraps
  CHECK_EQ(r.pop(out, 32), 14u);
  CHECK_EQ(out[3], 6);
  CHECK_EQ(out[4], 1);
  CHECK_EQ(r.popByte(), -1);
}

struct Clock {
  uint32_t ms = 1000, us = 1000000;
  static uint32_t msF(void* u) { return static_cast<Clock*>(u)->ms; }
  static uint32_t usF(void* u) {
    Clock* c = static_cast<Clock*>(u);
    c->us += 7;   // every read advances: tasks appear to take time
    return c->us;
  }
};

void testScheduler() {
  Scheduler s;
  int a = 0, b = 0, c = 0;
  s.add("every", 0, [](void* u, uint32_t) { ++*static_cast<int*>(u); }, &a);
  s.add("10ms", 10, [](void* u, uint32_t) { ++*static_cast<int*>(u); }, &b);
  const int ic = s.add("33ms", 33, [](void* u, uint32_t) { ++*static_cast<int*>(u); }, &c);
  Clock clk;
  for (int i = 0; i < 100; ++i) {   // 100 ms in 1 ms passes
    s.runOnce(Clock::msF, Clock::usF, &clk);
    clk.ms += 1;
  }
  CHECK_EQ(a, 100);
  CHECK_EQ(b, 10);      // at 0, 10, ..., 90
  CHECK_EQ(c, 4);       // at 0, 33, 66, 99
  // A stall of 500 ms: each periodic task runs once, not 50 times to catch up.
  clk.ms += 500;
  const int b0 = b;
  s.runOnce(Clock::msF, Clock::usF, &clk);
  s.runOnce(Clock::msF, Clock::usF, &clk);
  CHECK_EQ(b - b0, 1);
  // poke runs a task on the next pass.
  const int c0 = c;
  s.poke(ic);
  clk.ms += 1;
  s.runOnce(Clock::msF, Clock::usF, &clk);
  CHECK_EQ(c - c0, 1);
  CHECK(s.stats(0).runs >= 100u);
  CHECK(s.stats(0).maxUs > 0u);
  s.takeWindow();
  CHECK_EQ(s.stats(0).windowRuns, 0u);
  CHECK_EQ(s.count(), 3);
}

std::vector<pg::KeyEvent> g_ev;
void collect(void*, const pg::KeyEvent& e) { g_ev.push_back(e); }

void testKeyScript() {
  KeyScript k;
  g_ev.clear();
  const int q = pg::ui::keyFromName("q"), spc = pg::ui::keyFromName("spc"), sft = pg::ui::keyFromName("sft");
  CHECK(q >= 0 && spc >= 0 && sft >= 0);
  CHECK(k.parse("down sft") == KeyScript::Parse::Queued);
  CHECK(k.parse("tap q 100") == KeyScript::Parse::Queued);
  CHECK(k.parse("up sft") == KeyScript::Parse::Queued);
  CHECK(k.parse("wait 50") == KeyScript::Parse::Queued);
  CHECK(k.parse("tap spc") == KeyScript::Parse::Queued);
  CHECK(k.parse("  # a comment") == KeyScript::Parse::Skipped);
  CHECK(k.parse("battery 50") == KeyScript::Parse::Skipped);   // ui_shot's setup lines
  CHECK(k.parse("tap nokey") == KeyScript::Parse::UnknownKey);
  CHECK(k.parse("wait x") == KeyScript::Parse::BadNumber);
  CHECK(k.parse("shot here") == KeyScript::Parse::Other);
  CHECK(std::strcmp(k.verb(), "shot") == 0 && std::strcmp(k.arg(), "here") == 0);
  CHECK(k.parse("down G0") == KeyScript::Parse::Queued);
  // t = 0: sft down, q down, then the tap's 100 ms hold.
  k.step(0, collect, nullptr);
  CHECK_EQ(g_ev.size(), 2u);
  CHECK(g_ev[0].key == sft && g_ev[0].down);
  CHECK(g_ev[1].key == q && g_ev[1].down);
  k.step(99, collect, nullptr);
  CHECK_EQ(g_ev.size(), 2u);
  k.step(100, collect, nullptr);   // q up, sft up, then wait 50
  CHECK_EQ(g_ev.size(), 4u);
  CHECK(g_ev[2].key == q && !g_ev[2].down && g_ev[2].timeUs == 100000u);
  k.step(149, collect, nullptr);
  CHECK_EQ(g_ev.size(), 4u);
  k.step(150, collect, nullptr);   // spc down, its 40 ms
  CHECK_EQ(g_ev.size(), 5u);
  k.step(190, collect, nullptr);   // spc up, G0 down
  CHECK_EQ(g_ev.size(), 7u);
  CHECK(g_ev[6].key == pg::kKeyG0 && g_ev[6].down);
  CHECK(k.idle());
  CHECK(k.held() == (1ull << pg::kKeyG0));
  k.releaseAll(200, collect, nullptr);
  CHECK_EQ(g_ev.size(), 8u);
  CHECK(!g_ev[7].down && k.held() == 0);
  // Full queue: the command is dropped and counted.
  KeyScript f;
  int queued = 0;
  for (int i = 0; i < 200; ++i) queued += f.parse("wait 1") == KeyScript::Parse::Queued ? 1 : 0;
  CHECK_EQ(queued, KeyScript::kQueue);
  CHECK_EQ(f.dropped(), 200u - KeyScript::kQueue);
  CHECK(f.parse("tap q") == KeyScript::Parse::Full);
}

void testSerialLink() {
  pg::io::Xfer x;
  x.init(nullptr, "t");
  SerialLink s;
  std::string letters, lines;
  int xferBytes = 0;
  auto feed = [&](const std::string& str) {
    for (char ch : str) {
      switch (s.feed(static_cast<uint8_t>(ch), 0, x)) {
        case SerialLink::Kind::Letter: letters += s.letter(); break;
        case SerialLink::Kind::ScriptLine: lines += std::string(s.line()) + "|"; break;
        case SerialLink::Kind::Xfer: ++xferBytes; break;
        default: break;
      }
    }
  };
  feed("r>tap q 20\r\nh\n~PGX 1 HELLO 00000000\n>down sft\n");
  CHECK(letters == "rh");
  CHECK(lines == "tap q 20|down sft|");
  CHECK(x.lineReady());   // the protocol line went to Xfer
  CHECK(xferBytes == int(std::strlen("~PGX 1 HELLO 00000000\n")));
  // A script line with a '~' inside stays a script line.
  lines.clear();
  feed(">tap ~\n");
  CHECK(lines == "tap ~|");
  // Too long: dropped whole.
  lines.clear();
  feed(">" + std::string(200, 'a') + "\nr");
  CHECK(lines.empty());
  CHECK_EQ(s.overlongLines(), 1u);
  CHECK(letters == "rhr");
}

void testLoadMeter() {
  LoadMeter m;
  const uint32_t hz = 480000000u, period = hz / 500u;   // 2 ms blocks
  m.configure(hz, period);
  uint32_t t = 0;
  for (int i = 0; i < 1000; ++i) {
    m.begin(t);
    m.end(t + period / 4);   // 25 %
    t += period;
  }
  CHECK(m.nowPermille() >= 245 && m.nowPermille() <= 255);
  // One block over its period, one late start.
  m.begin(t);
  m.end(t + period + 10);
  t += period * 2;   // a missed slot: 2 periods since the last start
  m.begin(t);
  m.end(t + 10);
  const LoadMeter::Window w = m.takeWindow();
  CHECK_EQ(m.overruns(), 1u);
  CHECK_EQ(m.lateStarts(), 1u);
  CHECK_EQ(w.late, 2u);
  CHECK(w.peakPermille >= 1000u);
  CHECK_EQ(w.blocks, 1002u);
  CHECK_EQ(m.periodUs(), 2000u);
  CHECK(w.maxUs >= 2000u);
  CHECK_EQ(m.takeWindow().blocks, 0u);
}

void testLed() {
  int on = 0;
  for (uint32_t t = 0; t < 1000; ++t) on += ledOn(LedMode::Running, t) ? 1 : 0;
  CHECK_EQ(on, 120);   // two 60 ms flashes per second
  on = 0;
  for (uint32_t t = 0; t < 1000; ++t) on += ledOn(LedMode::NoAudio, t) ? 1 : 0;
  CHECK_EQ(on, 500);
  CHECK(ledOn(LedMode::Booting, 12345));
  CHECK(ledModeName(LedMode::RomMissing)[0] != '?');
}

struct Lines {
  std::vector<std::string> v;
  static void sink(void* u, const char* l, size_t n) { static_cast<Lines*>(u)->v.emplace_back(l, n); }
};

void testFbStream() {
  static pg::Framebuffer fb;
  for (int i = 0; i < pg::kScreenW * pg::kScreenH; ++i) fb.px[i] = static_cast<uint8_t>(i * 7 + i / 240);
  for (int i = 0; i < 256; ++i) fb.palette[i] = static_cast<uint16_t>(i * 257 + 3);
  FbStream s;
  CHECK(s.start(&fb, 10, 13, "my shot"));
  CHECK(!s.start(&fb, 0, 1, "busy"));
  Lines out;
  // A small room first: nothing fits but the begin line.
  size_t sent = s.pump(60, Lines::sink, &out);
  CHECK(sent > 0 && out.v.size() == 1);
  while (s.active()) s.pump(800, Lines::sink, &out);
  CHECK_EQ(out.v.size(), 6u);   // B, P, 3 rows, E
  CHECK(out.v[0].rfind("~PGF B 1 240 135 10 13 my_shot\n", 0) == 0);
  // The palette and a row decode to what was sent.
  uint8_t pal[600], row[300];
  std::string p = out.v[1].substr(7);
  p.pop_back();
  CHECK_EQ(pg::io::base64Decode(p.c_str(), pal, sizeof(pal)), 512);
  CHECK(pal[2 * 5] == uint8_t(fb.palette[5]) && pal[2 * 5 + 1] == uint8_t(fb.palette[5] >> 8));
  std::string r = out.v[3];
  CHECK(r.rfind("~PGF R 11 ", 0) == 0);
  r = r.substr(10);
  r.pop_back();
  CHECK_EQ(pg::io::base64Decode(r.c_str(), row, sizeof(row)), pg::kScreenW);
  CHECK(std::memcmp(row, fb.px + 11 * pg::kScreenW, pg::kScreenW) == 0);
  CHECK(out.v[5] == "~PGF E 1\n");
  CHECK_EQ(s.framesSent(), 1u);
  for (const std::string& l : out.v) CHECK(l.size() <= 720);
}

struct Macros {
  int v[4] = {100, 100, 100, 100};
  int sets = 0;
  bool instrument = true;
  static int cur(void* u, int m) {
    Macros* a = static_cast<Macros*>(u);
    return a->instrument ? a->v[m] : -1;
  }
  static void set(void* u, int m, int val) {
    Macros* a = static_cast<Macros*>(u);
    a->v[m] = val;
    ++a->sets;
  }
};

void testPots() {
  PotMacros p;
  Macros m;
  float raw[4] = {0.0f, 0.5f, 1.0f, -1.0f};   // pot 4 absent
  // Pickup: pot 1 at 0 and pot 3 at 255 are far from 100: nothing moves.
  CHECK_EQ(p.update(raw, 6, Macros::cur, Macros::set, &m), 0);
  // Pot 2 at 0.5 (128) is far from 100 too; sweep pot 1 up past 100: it catches.
  for (int i = 0; i <= 50; ++i) {
    raw[0] = float(i) / 100.0f;
    p.update(raw, 6, Macros::cur, Macros::set, &m);
  }
  CHECK(p.engaged(0));
  CHECK(m.v[0] >= 125 && m.v[0] <= 129);
  CHECK_EQ(m.v[2], 100);   // pot 3 never crossed
  CHECK(!p.engaged(2));
  // Crossing from above: pot 3 comes down past 100.
  for (int i = 100; i >= 30; --i) {
    raw[2] = float(i) / 100.0f;
    p.update(raw, 6, Macros::cur, Macros::set, &m);
  }
  CHECK(p.engaged(2));
  CHECK(m.v[2] >= 75 && m.v[2] <= 78);
  // ADC noise of one step: no stream of changes.
  const int s0 = m.sets;
  raw[2] = 0.30f + 0.5f / 255.0f;
  p.update(raw, 6, Macros::cur, Macros::set, &m);
  CHECK_EQ(m.sets, s0);
  // The value moved by something else (a key): pickup again.
  m.v[0] = 20;
  raw[0] = 0.51f;
  p.update(raw, 6, Macros::cur, Macros::set, &m);
  CHECK(!p.engaged(0));
  CHECK_EQ(m.v[0], 20);
  // A new track: everything picks up afresh.
  p.update(raw, 7, Macros::cur, Macros::set, &m);
  CHECK(!p.engaged(1));   // pot 2 (128) is not at its macro (100) on the new track
  CHECK(p.engaged(2));    // pot 3 happens to sit on its value: caught at once
  // No instrument: nothing.
  m.instrument = false;
  CHECK_EQ(p.update(raw, 7, Macros::cur, Macros::set, &m), 0);
  // Off.
  m.instrument = true;
  p.setEnabled(false);
  raw[1] = 100.0f / 255.0f;
  CHECK_EQ(p.update(raw, 7, Macros::cur, Macros::set, &m), 0);
}

std::string readFile(const char* path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void testLayoutAndPins() {
  using namespace layout;
  CHECK_EQ(kRomAddr, 0x900B8000u);
  CHECK(kAppAddr + kAppMax <= kRomAddr);
  CHECK(kRomAddr + kRomMax <= kFsAddr);
  CHECK(kFsAddr + 2 * kFsBankBytes <= kPoolAddr);
  CHECK(kPoolAddr + kPoolBytes <= kQspiBase + kQspiBytes);
  CHECK(kFsSlotBytes >= pg::io::Xfer::kMaxPutBytes);   // the largest project fits a /int slot
  // The linker script says the same.
  const std::string lds = readFile("daisy/pg_daisy.lds");
  CHECK(!lds.empty());
  CHECK(lds.find("QSPI_ROM    (RX)  : ORIGIN = 0x900B8000, LENGTH = 288K") != std::string::npos);
  CHECK(lds.find("_pg_image_max = 0x78000;") != std::string::npos);
  CHECK(288u * 1024u == kRomMax);
  // Pins: no D number twice, analog functions on ADC pins, every pin on the Seed3's 31.
  for (int i = 0; i < pins::kMapCount; ++i) {
    const pins::PinUse& a = pins::kMap[i];
    CHECK(a.seed <= 30);
    CHECK(!a.adc || pins::kSeed[a.seed].adc);
    for (int j = i + 1; j < pins::kMapCount; ++j) CHECK(pins::kMap[j].seed != a.seed);
  }
  // libDaisy's peripheral routing for what the map relies on (daisy_seed.h, the
  // Rev4 pinout CSV): SDMMC1 on D1..D6, SPI1 SCK/MOSI on D8/D10, I2C1 on D11/D12,
  // I2C4 on D13/D14 (PB6/PB7).
  CHECK(pins::kSeed[4].port == 'C' && pins::kSeed[4].pin == 8);    // SDMMC1_D0
  CHECK(pins::kSeed[6].port == 'C' && pins::kSeed[6].pin == 12);   // SDMMC1_CK
  CHECK(pins::kSeed[8].port == 'G' && pins::kSeed[8].pin == 11);   // SPI1_SCK
  CHECK(pins::kSeed[10].port == 'B' && pins::kSeed[10].pin == 5);  // SPI1_MOSI
  CHECK(pins::kSeed[11].port == 'B' && pins::kSeed[11].pin == 8);  // I2C1_SCL
  CHECK(pins::kSeed[13].port == 'B' && pins::kSeed[13].pin == 6);  // I2C4_SCL
  for (uint8_t d : pins::kPotSeed) CHECK(pins::kSeed[d].adc);
  // The header pins are the Seed's 1..15 and 22..37 (16..21 and 38..40 are analog / power).
  for (const pins::SeedPin& s : pins::kSeed) CHECK((s.header >= 1 && s.header <= 15) || (s.header >= 22 && s.header <= 37));
}

}  // namespace

int main() {
  testRing();
  testScheduler();
  testKeyScript();
  testSerialLink();
  testLoadMeter();
  testLed();
  testFbStream();
  testPots();
  testLayoutAndPins();
  return pgdt::finish("test_daisy_core");
}
