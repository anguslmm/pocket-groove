#include "gfx.h"

#include <cstdio>
#include <cstring>

#include "font5x7.h"

namespace smoke {

namespace {
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
}  // namespace

void Canvas::initPalette() {
  for (int i = 0; i < 256; ++i) fb_->palette[i] = 0;
  fb_->palette[kBlack]   = rgb(0, 0, 0);
  fb_->palette[kWhite]   = rgb(255, 255, 255);
  fb_->palette[kGrey]    = rgb(150, 150, 150);
  fb_->palette[kDark]    = rgb(50, 50, 60);
  fb_->palette[kGreen]   = rgb(40, 230, 60);
  fb_->palette[kRed]     = rgb(255, 50, 40);
  fb_->palette[kYellow]  = rgb(255, 220, 0);
  fb_->palette[kCyan]    = rgb(0, 220, 255);
  fb_->palette[kBlue]    = rgb(60, 110, 255);
  fb_->palette[kOrange]  = rgb(255, 140, 0);
  fb_->palette[kDkGreen] = rgb(0, 90, 20);
  fb_->palette[kDkRed]   = rgb(110, 0, 0);
  fb_->palette[kDkBlue]  = rgb(0, 30, 90);
}

void Canvas::clear(uint8_t c) { memset(fb_->px, c, sizeof(fb_->px)); }

void Canvas::fillRect(int x, int y, int w, int h, uint8_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > pg::kScreenW) w = pg::kScreenW - x;
  if (y + h > pg::kScreenH) h = pg::kScreenH - y;
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; ++j) memset(fb_->px + (y + j) * pg::kScreenW + x, c, static_cast<size_t>(w));
}

void Canvas::rect(int x, int y, int w, int h, uint8_t c) {
  fillRect(x, y, w, 1, c);
  fillRect(x, y + h - 1, w, 1, c);
  fillRect(x, y, 1, h, c);
  fillRect(x + w - 1, y, 1, h, c);
}

int Canvas::text(int x, int y, const char* s, uint8_t fg, int bg, int spacing) {
  if (!s) return x;
  for (; *s; ++s) {
    unsigned ch = static_cast<unsigned char>(*s);
    if (ch < kFontFirst || ch > kFontLast) ch = '?';
    const uint8_t* g = &kFont5x7[(ch - kFontFirst) * kFontW];
    if (bg >= 0) fillRect(x, y, kFontW + spacing, kCellH, static_cast<uint8_t>(bg));
    for (int cx = 0; cx < kFontW; ++cx) {
      const int px = x + cx;
      if (px < 0 || px >= pg::kScreenW) continue;
      uint8_t bits = g[cx];
      for (int cy = 0; cy < kFontH && bits; ++cy, bits >>= 1) {
        const int py = y + cy;
        if ((bits & 1) && py >= 0 && py < pg::kScreenH) fb_->px[py * pg::kScreenW + px] = fg;
      }
    }
    x += kFontW + spacing;
    if (x >= pg::kScreenW) break;
  }
  return x;
}

void Canvas::vline(int ln, int col, uint8_t fg, const char* fmt, va_list ap) {
  char buf[kCols + 8];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  buf[kCols - (col < kCols ? col : kCols)] = 0;   // clip to the screen
  text(col * kCellW, ln * kCellH + 1, buf, fg);
}

void Canvas::line(int ln, uint8_t fg, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(ln, 0, fg, fmt, ap);
  va_end(ap);
}

void Canvas::lineAt(int ln, int col, uint8_t fg, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(ln, col, fg, fmt, ap);
  va_end(ap);
}

void Canvas::title(const char* left, const char* right) {
  fillRect(0, 0, pg::kScreenW, kCellH, kDkBlue);
  text(1, 0, left, kWhite);
  if (right) {
    const int w = static_cast<int>(strlen(right)) * kCellW;
    text(pg::kScreenW - w, 0, right, kYellow);
  }
}

}  // namespace smoke
