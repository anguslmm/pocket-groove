// Palette (docs/UX.md section 12) and drawing primitives equivalent to the
// mockups' tools/ux_mock/pgfb.py. Everything clips to the 240x135 framebuffer.
#pragma once
#include <cstdint>
#include "../platform.h"

namespace pg {
namespace ui {

enum Colour : uint8_t {
  BG = 0, PANEL, RAISED, LINE, DIM, TEXT, WHITE, AMBER, RED, GREEN, CYAN,
  AMBER_D, RED_D, GREEN_D, CYAN_D, BLACK,
  TRACK0 = 16,   // 16 + 4*track + shade
};
enum Shade : uint8_t { kBright = 0, kMid = 1, kDim = 2, kTint = 3 };
constexpr uint8_t tc(int track, int shade = 0) { return uint8_t(TRACK0 + 4 * track + shade); }

// Writes the 48 used RGB565 entries (and black for 48..255) into fb.palette.
void initPalette(Framebuffer& fb);
// RGB888 of a palette entry as the mockups define it (before RGB565 quantisation).
void paletteRgb(int index, uint8_t* r, uint8_t* g, uint8_t* b);

// Pixel width of a string in FONT_S at `scale` (last column has no gap).
inline int tw(const char* s, int scale = 1) {
  int n = 0;
  while (s[n]) ++n;
  return n ? n * 6 * scale - scale : 0;
}

class Gfx {
 public:
  explicit Gfx(Framebuffer& fb) : fb_(fb) {}
  void clear(uint8_t c);
  void pixel(int x, int y, uint8_t c) {
    if (unsigned(x) < unsigned(kScreenW) && unsigned(y) < unsigned(kScreenH)) fb_.px[y * kScreenW + x] = c;
  }
  void rect(int x, int y, int w, int h, uint8_t c);
  void frame(int x, int y, int w, int h, uint8_t c);
  void rrect(int x, int y, int w, int h, uint8_t c);    // filled, corner pixels knocked out
  void rframe(int x, int y, int w, int h, uint8_t c);
  void line(int x0, int y0, int x1, int y1, uint8_t c);
  // Draws s with FONT_S at scale 1 (S), 2 (L) or 3 (XL); returns the end x.
  int text(int x, int y, const char* s, uint8_t c, int scale = 1);
  void textC(int cx, int y, const char* s, uint8_t c, int scale = 1) { text(cx - tw(s, scale) / 2, y, s, c, scale); }
  void textR(int xr, int y, const char* s, uint8_t c, int scale = 1) { text(xr - tw(s, scale) + 1, y, s, c, scale); }
  // Rounded label 9 px tall, text inset 2 px. Returns x after the chip.
  int chip(int x, int y, const char* s, uint8_t col, uint8_t ink = BG);

 private:
  Framebuffer& fb_;
};

}  // namespace ui
}  // namespace pg
