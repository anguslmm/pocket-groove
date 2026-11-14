// Host test of the Daisy's display path (daisy/core/panel.h) against a simulated
// MIPI-DCS panel that executes CASET / RASET / RAMWR and the pixel bytes as a
// controller does. The picture it ends up with must be the Cardputer path's
// (src/platform/display.cpp: palette[index] byte-swapped into the strip, sent as
// is), pixel for pixel, at scale 1 on the 240x135 ST7789 and at scale 2 on a 480x320
// HX8357D; partial updates (dirty rows) only touch their rows.
#include <algorithm>
#include <cstring>
#include <vector>

#include "check.h"
#include "core/panel.h"
#include "pg/ui/gfx.h"

using namespace pgd;

namespace {

struct SimPanel final : SpiPanelBus {
  int ramW, ramH;
  std::vector<uint16_t> ram;
  int xa = 0, xb = 0, ya = 0, yb = 0, cx = 0, cy = 0;
  bool writing = false, odd = false;
  uint8_t hi = 0;
  int busyPolls = 0;
  std::vector<uint8_t> cmds;
  uint32_t pixelBytes = 0;
  SimPanel(int w, int h) : ramW(w), ramH(h), ram(size_t(w) * h, 0xDEAD) {}
  void command(uint8_t c, const uint8_t* p, size_t n) override {
    cmds.push_back(c);
    writing = false;
    if (c == 0x2A && n == 4) { xa = p[0] << 8 | p[1]; xb = p[2] << 8 | p[3]; }
    if (c == 0x2B && n == 4) { ya = p[0] << 8 | p[1]; yb = p[2] << 8 | p[3]; }
    if (c == 0x2C) { writing = true; cx = xa; cy = ya; odd = false; }
  }
  void pixelsAsync(const uint8_t* d, size_t n) override {
    CHECK(writing);
    pixelBytes += uint32_t(n);
    for (size_t i = 0; i < n; ++i) {
      if (!odd) { hi = d[i]; odd = true; continue; }
      odd = false;
      if (cx < ramW && cy < ramH) ram[size_t(cy) * ramW + cx] = uint16_t(hi << 8 | d[i]);
      if (++cx > xb) { cx = xa; ++cy; }
    }
    busyPolls = 2;   // the "DMA" takes two polls
  }
  bool busy() override { return busyPolls-- > 0; }
  void delayMs(uint32_t) override {}
};

// The Cardputer path's pixel: what M5GFX's writePixelsDMA puts on the bus for index i.
uint16_t cardputerPixel(const pg::Framebuffer& fb, int x, int y) {
  const uint16_t c = fb.palette[fb.px[y * pg::kScreenW + x]];
  const uint16_t swapped = uint16_t((c << 8) | (c >> 8));   // s_pal[i] = bswap(palette[i])
  uint8_t bytes[2];
  std::memcpy(bytes, &swapped, 2);                          // little-endian memory, sent in order
  return uint16_t(bytes[0] << 8 | bytes[1]);                // the panel's view: first byte high
}

void runFrame(Panel& p, SimPanel& sim) {
  for (int i = 0; i < 100000 && p.step(); ++i) {
  }
  (void)sim;
}

std::vector<uint8_t> g_strips(2 * Panel::kStripBytes);

void testScale1() {
  static pg::Framebuffer fb;
  pg::ui::initPalette(fb);
  for (int i = 0; i < pg::kScreenW * pg::kScreenH; ++i) fb.px[i] = uint8_t((i * 13) ^ (i >> 7));
  const PanelConfig cfg = PanelConfig::st7789_240x135();
  SimPanel sim(320, 240);   // the ST7789's RAM
  Panel p;
  p.init(cfg, &sim, g_strips.data());
  p.begin();
  CHECK(sim.cmds.size() >= 8 && sim.cmds[0] == 0x01 && sim.cmds.back() == 0x29);   // reset first, on last
  p.present(fb, 0, pg::kScreenH);
  runFrame(p, sim);
  int bad = 0;
  for (int y = 0; y < pg::kScreenH; ++y)
    for (int x = 0; x < pg::kScreenW; ++x)
      bad += sim.ram[size_t(cfg.ramY + y) * 320 + cfg.ramX + x] != cardputerPixel(fb, x, y);
  CHECK_EQ(bad, 0);
  CHECK_EQ(p.framesSent(), 1u);
  // Dirty rows only: change rows 50..59, present 50..60; the rest stays as it was.
  for (int x = 0; x < pg::kScreenW; ++x)
    for (int y = 0; y < pg::kScreenH; ++y) fb.px[y * pg::kScreenW + x] = uint8_t(y < 50 || y >= 60 ? fb.px[y * pg::kScreenW + x] : 7);
  const uint32_t before = sim.pixelBytes;
  p.present(fb, 50, 60);
  runFrame(p, sim);
  CHECK_EQ(sim.pixelBytes - before, uint32_t(pg::kScreenW * 10 * 2));
  bad = 0;
  for (int y = 0; y < pg::kScreenH; ++y)
    for (int x = 0; x < pg::kScreenW; ++x)
      bad += sim.ram[size_t(cfg.ramY + y) * 320 + cfg.ramX + x] != cardputerPixel(fb, x, y);
  CHECK_EQ(bad, 0);
  // Two presents while busy merge into one window.
  p.present(fb, 0, 3);
  p.present(fb, 100, 103);
  runFrame(p, sim);
  CHECK_EQ(p.framesSent(), 4u);   // the first started at once, the second (merged) after it
}

void testScale2() {
  static pg::Framebuffer fb;
  pg::ui::initPalette(fb);
  for (int i = 0; i < pg::kScreenW * pg::kScreenH; ++i) fb.px[i] = uint8_t(i * 31 + 5);
  const PanelConfig cfg = PanelConfig::hx8357d_480x320();
  SimPanel sim(480, 320);
  Panel p;
  p.init(cfg, &sim, g_strips.data());
  p.begin();
  CHECK(std::find(sim.cmds.begin(), sim.cmds.end(), uint8_t(0xB9)) != sim.cmds.end());   // HX8357D unlock
  p.present(fb, 0, pg::kScreenH);
  runFrame(p, sim);
  int bad = 0, black = 0;
  for (int y = 0; y < 320; ++y)
    for (int x = 0; x < 480; ++x) {
      const uint16_t got = sim.ram[size_t(y) * 480 + x];
      const int uy = y - cfg.y0;
      if (uy < 0 || uy >= 2 * pg::kScreenH) {
        black += got == 0;   // the 25 spare rows above and below: cleared by begin()
        continue;
      }
      bad += got != cardputerPixel(fb, x / 2, uy / 2);
    }
  CHECK_EQ(bad, 0);
  CHECK_EQ(black, 480 * 50);
}

// The Adafruit 2090 (ILI9341 320x240, the stage 1 default): Adafruit_ILI9341's setup
// before sleep-out, MADCTL 0x28 (landscape, BGR), the UI at scale 1 centred at
// (40, 52), the 80 x 240 + 240 x 105 border black, dirty rows only; turned over, a new
// begin() sends 0xE8 and the picture again without a present().
void testIli9341() {
  static pg::Framebuffer fb;
  pg::ui::initPalette(fb);
  for (int i = 0; i < pg::kScreenW * pg::kScreenH; ++i) fb.px[i] = uint8_t((i * 7) ^ (i >> 5));
  const PanelConfig cfg = PanelConfig::ili9341_320x240();
  CHECK(cfg.controller == PanelController::ILI9341 && cfg.width == 320 && cfg.height == 240 && cfg.scale == 1);
  CHECK(cfg.x0 == 40 && cfg.y0 == 52 && cfg.madctl == 0x28 && !cfg.invert);
  CHECK(std::strcmp(cfg.name(), "ILI9341") == 0);
  SimPanel sim(320, 240);
  struct Rec final : SpiPanelBus {   // records the parameters of each command too
    SimPanel* s;
    std::vector<std::vector<uint8_t>> params;
    explicit Rec(SimPanel* p) : s(p) {}
    void command(uint8_t c, const uint8_t* p, size_t n) override {
      params.emplace_back(p, p + n);
      s->command(c, p, n);
    }
    void pixelsAsync(const uint8_t* d, size_t n) override { s->pixelsAsync(d, n); }
    bool busy() override { return s->busy(); }
    void delayMs(uint32_t ms) override { s->delayMs(ms); }
  } rec(&sim);
  Panel p;
  p.init(cfg, &rec, g_strips.data());
  p.begin();
  // Adafruit's order: reset first, its table, sleep out, display on last
  CHECK(sim.cmds.size() > 20 && sim.cmds[0] == 0x01 && sim.cmds.back() == 0x29);
  auto idx = [&](uint8_t c) {
    for (size_t i = 0; i < sim.cmds.size(); ++i)
      if (sim.cmds[i] == c) return int(i);
    return -1;
  };
  CHECK(idx(0xEF) > 0 && idx(0xCF) > idx(0xEF) && idx(0xE0) > idx(0xCF) && idx(0xE1) > idx(0xE0));
  CHECK(idx(0x11) > idx(0xE1));   // sleep out after the table
  const std::vector<uint8_t>& pwr = rec.params[size_t(idx(0xC0))];
  CHECK(pwr.size() == 1 && pwr[0] == 0x23);
  const std::vector<uint8_t>& gam = rec.params[size_t(idx(0xE0))];
  CHECK(gam.size() == 15 && gam[0] == 0x0F && gam[14] == 0x00);
  const std::vector<uint8_t>& mad = rec.params[size_t(idx(0x36))];
  CHECK(mad.size() == 1 && mad[0] == 0x28);
  const std::vector<uint8_t>& col = rec.params[size_t(idx(0x3A))];
  CHECK(col.size() == 1 && col[0] == 0x55);
  CHECK(idx(0x21) < 0);           // no inversion
  CHECK(std::all_of(sim.ram.begin(), sim.ram.end(), [](uint16_t v) { return v == 0; }));   // cleared once, all of it
  p.present(fb, 0, pg::kScreenH);
  runFrame(p, sim);
  int bad = 0, black = 0;
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 320; ++x) {
      const uint16_t got = sim.ram[size_t(y) * 320 + x];
      const int ux = x - 40, uy = y - 52;
      if (ux < 0 || ux >= pg::kScreenW || uy < 0 || uy >= pg::kScreenH) {
        black += got == 0;
        continue;
      }
      bad += got != cardputerPixel(fb, ux, uy);
    }
  CHECK_EQ(bad, 0);
  CHECK_EQ(black, 320 * 240 - pg::kScreenW * pg::kScreenH);
  // dirty rows only: 10 rows of the UI = 240 x 10 pixels on the bus
  for (int x = 0; x < pg::kScreenW; ++x)
    for (int y = 70; y < 80; ++y) fb.px[y * pg::kScreenW + x] = 3;
  const uint32_t before = sim.pixelBytes;
  p.present(fb, 70, 80);
  runFrame(p, sim);
  CHECK_EQ(sim.pixelBytes - before, uint32_t(pg::kScreenW * 10 * 2));
  CHECK(sim.ram[size_t(52 + 75) * 320 + 40 + 100] == cardputerPixel(fb, 100, 75));
  // turned over: a new begin() with 0xE8 clears and sends the last frame by itself
  sim.cmds.clear();
  rec.params.clear();
  p.setMadctl(PanelConfig::kIliMadctlTurned);
  p.begin();
  runFrame(p, sim);
  CHECK(rec.params[size_t(idx(0x36))][0] == 0xE8);
  CHECK(sim.ram[size_t(52 + 75) * 320 + 40 + 100] == cardputerPixel(fb, 100, 75));
  CHECK(sim.ram[0] == 0);
  CHECK(PanelConfig::ili9341_320x240(true).madctl == 0xE8);
}

}  // namespace

int main() {
  testScale1();
  testScale2();
  testIli9341();
  return pgdt::finish("test_daisy_panel");
}
