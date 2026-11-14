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

}  // namespace

int main() {
  testScale1();
  testScale2();
  return pgdt::finish("test_daisy_panel");
}
