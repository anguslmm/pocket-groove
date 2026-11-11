#include "overlay.h"

#include <cstring>

#include "pg/config.h"
#include "pg/io/project_io.h"
#include "pg/ui/font.h"

namespace app {

namespace {
inline uint16_t bswap(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }
}  // namespace

uint32_t Overlays::signature() const {
  uint32_t crc = 0;
  for (const OverlayBox& b : box_) {
    const uint8_t on = b.on ? 1 : 0;
    crc = pg::io::crc32(crc, &on, 1);
    if (b.on) crc = pg::io::crc32(crc, reinterpret_cast<const uint8_t*>(&b), sizeof(b));
  }
  return crc;
}

bool Overlays::takeDirty(int* y0, int* y1) {
  const uint32_t sig = signature();
  if (sig == lastSig_) return false;
  lastSig_ = sig;
  int a = pg::kScreenH, z = 0;
  for (const OverlayBox& b : box_) {
    if (!b.on) continue;
    if (b.y < a) a = b.y;
    if (b.y + b.h > z) z = b.y + b.h;
  }
  // the union with what was shown before, so a box that went away is erased
  int ra = a, rz = z;
  if (lastY1_ > lastY0_) {
    if (lastY0_ < ra) ra = lastY0_;
    if (lastY1_ > rz) rz = lastY1_;
  }
  lastY0_ = (z > a) ? a : 0;
  lastY1_ = (z > a) ? z : 0;
  if (ra < 0) ra = 0;
  if (rz > pg::kScreenH) rz = pg::kScreenH;
  if (rz <= ra) return false;
  *y0 = ra;
  *y1 = rz;
  return true;
}

void Overlays::hook(uint16_t* px, int y, int rows, void* self) {
  static_cast<const Overlays*>(self)->render(px, y, rows);
}

void Overlays::render(uint16_t* px, int y0, int rows) const {
  constexpr int W = pg::kScreenW;
  for (const OverlayBox& b : box_) {
    if (!b.on || b.w <= 0 || b.h <= 0) continue;
    const int top = b.y > y0 ? b.y : y0;
    const int bot = (b.y + b.h < y0 + rows) ? b.y + b.h : y0 + rows;
    if (top >= bot) continue;
    const int xa = b.x < 0 ? 0 : b.x;
    const int xz = (b.x + b.w > W) ? W : b.x + b.w;
    const uint16_t bg = bswap(b.bg), border = bswap(b.border);
    const int lines = b.lines > 3 ? 3 : b.lines;   // text[] / fg[] hold 3
    int len[3];
    for (int i = 0; i < 3; ++i) len[i] = static_cast<int>(strnlen(b.text[i], kOvCols));
    for (int y = top; y < bot; ++y) {
      uint16_t* row = px + (y - y0) * W;
      const int ry = y - b.y;
      const bool edgeRow = (ry == 0 || ry == b.h - 1);
      // text line and glyph row for this pixel row
      const int tl = (ry - 2) / 8, gy = (ry - 2) % 8;
      const bool textRow = ry >= 2 && tl < lines && gy < 7;
      const uint16_t fg = textRow ? bswap(b.fg[tl]) : 0;
      const bool barRow = b.barPct >= 0 && ry >= b.barY && ry < b.barY + b.barH;
      const int barX0 = b.x + 4, barW = b.w - 8;
      const int barX1 = barX0 + (barW * (b.barPct < 0 ? 0 : b.barPct > 100 ? 100 : b.barPct)) / 100;
      const uint16_t barFg = bswap(b.barFg), barBg = bswap(b.barBg);
      for (int x = xa; x < xz; ++x) {
        const int rx = x - b.x;
        uint16_t c = bg;
        if (b.border != b.bg && (edgeRow || rx == 0 || rx == b.w - 1)) {
          c = border;
        } else if (textRow && rx >= 2) {
          const int ci = (rx - 2) / 6, gx = (rx - 2) % 6;
          if (ci < len[tl] && gx < 5) {
            const uint8_t* g = pg::ui::glyph(static_cast<uint8_t>(b.text[tl][ci]));
            if ((g[gx] >> gy) & 1) c = fg;
          }
        } else if (barRow && x >= barX0 && x < barX0 + barW) {
          c = (x < barX1) ? barFg : barBg;
        }
        row[x] = c;
      }
    }
  }
}

}  // namespace app
