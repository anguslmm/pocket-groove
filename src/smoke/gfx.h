// Smoke test: minimal drawing into a pg::Framebuffer (8-bit indexed) with the
// embedded 5x7 font. Text cells are 6x8 px: 40 columns x 16 lines on 240x135.
// Portable: no hardware includes.
#pragma once
#include <cstdarg>
#include <cstdint>

#include "pg/platform.h"

namespace smoke {

enum Color : uint8_t {
  kBlack = 0, kWhite, kGrey, kDark, kGreen, kRed, kYellow, kCyan, kBlue, kOrange, kDkGreen, kDkRed, kDkBlue,
  kNumColors
};

constexpr int kCellW = 6, kCellH = 8;
constexpr int kCols = pg::kScreenW / kCellW;   // 40
constexpr int kLines = pg::kScreenH / kCellH;  // 16

class Canvas {
 public:
  explicit Canvas(pg::Framebuffer* fb) : fb_(fb) {}
  void initPalette();
  void clear(uint8_t c);
  void fillRect(int x, int y, int w, int h, uint8_t c);
  void rect(int x, int y, int w, int h, uint8_t c);
  // Draws text at pixel position; returns the x after the last glyph.
  // spacing = blank columns between glyphs (1 normally, 0 to squeeze).
  int text(int x, int y, const char* s, uint8_t fg, int bg = -1, int spacing = 1);
  // Line-based helpers (line 0..15, column 0..39).
  void line(int ln, uint8_t fg, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
  void lineAt(int ln, int col, uint8_t fg, const char* fmt, ...) __attribute__((format(printf, 5, 6)));
  void title(const char* left, const char* right);
  pg::Framebuffer* fb() { return fb_; }

 private:
  void vline(int ln, int col, uint8_t fg, const char* fmt, va_list ap);
  pg::Framebuffer* fb_;
};

}  // namespace smoke
