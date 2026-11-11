// Platform layer: display. See display.h.
//
// Checked against M5GFX 0.2.36 (lgfx/v1/LGFXBase.hpp, panel/Panel_LCD.inl,
// platforms/esp32/Bus_SPI.inl):
//  * writePixelsDMA(const uint16_t*, len, swap=false) treats the data as
//    swap565_t, i.e. already in the panel's big-endian RGB565 order, so the
//    pixel copy is "no_convert" and Bus_SPI::writeBytes hands the buffer to
//    GDMA directly (buffer must be internal, DMA-capable RAM: .bss is).
//  * Bus_SPI::writeBytes waits for the previous transfer (SPI_USR) before it
//    starts the next one, so with two strip buffers the one being filled is
//    never the one on the bus.
//  * endWrite() waits for the last DMA to finish.
#include "display.h"

#include <M5Unified.h>
#include <esp_timer.h>

namespace plat {
namespace display {

namespace {

constexpr int kW = pg::kScreenW, kH = pg::kScreenH;
constexpr int kStripRows = 9;                    // 135 = 15 strips of 9 rows; 4320 bytes per strip
constexpr int kStripPx = kW * kStripRows;

uint16_t s_strip[2][kStripPx];                   // DMA source, internal RAM
uint16_t s_pal[256];                             // palette in panel byte order

bool     s_ok = false;
uint8_t  s_brightness = 128;
uint32_t s_frames = 0, s_fpsFrames = 0;
int64_t  s_fpsT0 = 0;
float    s_lastUs = 0, s_maxUs = 0, s_avgUs = 0, s_fps = 0;

inline uint16_t bswap(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

}  // namespace

bool begin() {
  s_ok = (M5.getDisplayCount() > 0) && (M5.Display.width() > 0);
  if (!s_ok) return false;
  // M5GFX's default for this board is already landscape 240x135 (rotation 1
  // in 0.2.x, renumbered to 0 in 0.3.0): never call setRotation with a literal.
  M5.Display.setBrightness(s_brightness);
  M5.Display.fillScreen(0);
  s_fpsT0 = esp_timer_get_time();
  return true;
}

void presentRows(const pg::Framebuffer& fb, int y0, int y1, StripHook hook, void* user) {
  if (!s_ok) return;
  if (y0 < 0) y0 = 0;
  if (y1 > kH) y1 = kH;
  if (y0 >= y1) return;
  const int64_t t0 = esp_timer_get_time();

  for (int i = 0; i < 256; ++i) s_pal[i] = bswap(fb.palette[i]);

  M5.Display.startWrite();
  M5.Display.setAddrWindow(0, y0, kW, y1 - y0);
  int buf = 0;
  for (int y = y0; y < y1; y += kStripRows) {
    const int rows = (y1 - y < kStripRows) ? (y1 - y) : kStripRows;
    const int n = rows * kW;
    const uint8_t* src = fb.px + y * kW;
    uint16_t* dst = s_strip[buf];
    for (int i = 0; i < n; i += 4) {             // kW is a multiple of 4
      dst[i]     = s_pal[src[i]];
      dst[i + 1] = s_pal[src[i + 1]];
      dst[i + 2] = s_pal[src[i + 2]];
      dst[i + 3] = s_pal[src[i + 3]];
    }
    if (hook) hook(dst, y, rows, user);
    M5.Display.writePixelsDMA(dst, n, false);    // returns once the transfer is started
    buf ^= 1;
  }
  M5.Display.endWrite();                         // waits for the last DMA

  const int64_t t1 = esp_timer_get_time();
  const float us = static_cast<float>(t1 - t0);
  s_lastUs = us;
  if (us > s_maxUs) s_maxUs = us;
  s_avgUs = (s_frames == 0) ? us : s_avgUs + (us - s_avgUs) * (1.0f / 16.0f);
  ++s_frames;
  ++s_fpsFrames;
  if (t1 - s_fpsT0 >= 1000000) {
    s_fps = static_cast<float>(s_fpsFrames) * 1.0e6f / static_cast<float>(t1 - s_fpsT0);
    s_fpsFrames = 0;
    s_fpsT0 = t1;
  }
}

void presentRows(const pg::Framebuffer& fb, int y0, int y1) { presentRows(fb, y0, y1, nullptr, nullptr); }
void present(const pg::Framebuffer& fb) { presentRows(fb, 0, kH, nullptr, nullptr); }

void directText(int x, int y, const char* text, uint16_t rgb565, uint8_t size) {
  if (!s_ok || !text) return;
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(rgb565, 0);
  M5.Display.drawString(text, x, y);
}

void setBrightness(uint8_t level) {
  s_brightness = level;
  if (s_ok) M5.Display.setBrightness(level);
}
uint8_t brightness() { return s_brightness; }

Stats stats() {
  Stats st;
  st.frames = s_frames;
  st.lastUs = s_lastUs;
  st.maxUs = s_maxUs;
  st.avgUs = s_avgUs;
  // fps decays to 0 if nobody presents for a while
  st.fps = (esp_timer_get_time() - s_fpsT0 > 2000000) ? 0.0f : s_fps;
  return st;
}

void resetStats() { s_maxUs = 0; s_avgUs = s_lastUs; }

}  // namespace display
}  // namespace plat
