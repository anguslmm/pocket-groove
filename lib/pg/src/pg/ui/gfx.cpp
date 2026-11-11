#include "gfx.h"

#include <cstring>
#include "font.h"

namespace pg {
namespace ui {

namespace {
// Exactly the mockups' palette (pgfb.build_palette()): base colours, then per
// track bright / mid = mix(bright, BG, .62) / dim (.36) / tint (.17).
const uint8_t kRgb[48][3] = {
    {0x08, 0x09, 0x0C}, {0x15, 0x18, 0x1F}, {0x23, 0x27, 0x31}, {0x3A, 0x41, 0x50},
    {0x7A, 0x83, 0x96}, {0xCF, 0xD6, 0xE2}, {0xFF, 0xFF, 0xFF}, {0xFF, 0xB4, 0x00},
    {0xFF, 0x38, 0x48}, {0x40, 0xE0, 0x70}, {0x30, 0xD0, 0xFF}, {0x5C, 0x40, 0x00},
    {0x60, 0x14, 0x1C}, {0x14, 0x50, 0x28}, {0x10, 0x4C, 0x60}, {0x00, 0x00, 0x00},
    {0xFF, 0x50, 0x3C}, {0xA1, 0x35, 0x2A}, {0x61, 0x23, 0x1D}, {0x32, 0x15, 0x14},  // KICK
    {0xFF, 0x8C, 0x20}, {0xA1, 0x5A, 0x18}, {0x61, 0x38, 0x13}, {0x32, 0x1F, 0x0F},  // SNAR
    {0xFF, 0xD4, 0x38}, {0xA1, 0x87, 0x27}, {0x61, 0x52, 0x1C}, {0x32, 0x2C, 0x13},  // CLAP
    {0x98, 0xE4, 0x30}, {0x61, 0x91, 0x22}, {0x3C, 0x58, 0x19}, {0x20, 0x2E, 0x12},  // CHH
    {0x30, 0xE4, 0xA8}, {0x21, 0x91, 0x6D}, {0x16, 0x58, 0x44}, {0x0F, 0x2E, 0x27},  // OHH
    {0x38, 0xB4, 0xFF}, {0x26, 0x73, 0xA3}, {0x19, 0x47, 0x63}, {0x10, 0x26, 0x35},  // PERC
    {0x94, 0x80, 0xFF}, {0x5F, 0x53, 0xA3}, {0x3A, 0x34, 0x63}, {0x20, 0x1D, 0x35},  // BASS
    {0xFF, 0x60, 0xC8}, {0xA1, 0x3F, 0x81}, {0x61, 0x28, 0x50}, {0x32, 0x18, 0x2C},  // LEAD
};
}  // namespace

void paletteRgb(int i, uint8_t* r, uint8_t* g, uint8_t* b) {
  if (i < 0 || i >= 48) { *r = *g = *b = 0; return; }
  *r = kRgb[i][0]; *g = kRgb[i][1]; *b = kRgb[i][2];
}

void initPalette(Framebuffer& fb) {
  for (int i = 0; i < 256; ++i) {
    if (i < 48) {
      fb.palette[i] = uint16_t(((kRgb[i][0] >> 3) << 11) | ((kRgb[i][1] >> 2) << 5) | (kRgb[i][2] >> 3));
    } else {
      fb.palette[i] = 0;
    }
  }
}

void Gfx::clear(uint8_t c) { std::memset(fb_.px, c, sizeof(fb_.px)); }

void Gfx::rect(int x, int y, int w, int h, uint8_t c) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > kScreenW ? kScreenW : x + w;
  int y1 = y + h > kScreenH ? kScreenH : y + h;
  if (x1 <= x0 || y1 <= y0) return;
  for (int yy = y0; yy < y1; ++yy) std::memset(&fb_.px[yy * kScreenW + x0], c, size_t(x1 - x0));
}

void Gfx::frame(int x, int y, int w, int h, uint8_t c) {
  rect(x, y, w, 1, c);
  rect(x, y + h - 1, w, 1, c);
  rect(x, y, 1, h, c);
  rect(x + w - 1, y, 1, h, c);
}

void Gfx::rrect(int x, int y, int w, int h, uint8_t c) {
  rect(x + 1, y, w - 2, h, c);
  rect(x, y + 1, w, h - 2, c);
}

void Gfx::rframe(int x, int y, int w, int h, uint8_t c) {
  rect(x + 1, y, w - 2, 1, c);
  rect(x + 1, y + h - 1, w - 2, 1, c);
  rect(x, y + 1, 1, h - 2, c);
  rect(x + w - 1, y + 1, 1, h - 2, c);
}

void Gfx::line(int x0, int y0, int x1, int y1, uint8_t c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (int guard = 0; guard < 1024; ++guard) {
    pixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

int Gfx::text(int x, int y, const char* s, uint8_t c, int scale) {
  for (; *s; ++s) {
    const uint8_t* cols = glyph(uint8_t(*s));
    if (x > -6 * scale && x < kScreenW && y > -8 * scale && y < kScreenH) {
      for (int cx = 0; cx < 5; ++cx) {
        uint8_t b = cols[cx];
        for (int cy = 0; b; ++cy, b >>= 1) {
          if (!(b & 1)) continue;
          if (scale == 1) pixel(x + cx, y + cy, c);
          else rect(x + cx * scale, y + cy * scale, scale, scale, c);
        }
      }
    }
    x += 6 * scale;
  }
  return x;
}

int Gfx::chip(int x, int y, const char* s, uint8_t col, uint8_t ink) {
  int n = 0;
  while (s[n]) ++n;
  const int w = n * 6 + 3;
  rrect(x, y, w, 9, col);
  text(x + 2, y + 1, s, ink);
  return x + w;
}

}  // namespace ui
}  // namespace pg
