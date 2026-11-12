// The window: the 240x135 pg::Framebuffer, integer-scaled with nearest
// neighbour, centred on black when the window's shape is not 16:9.
// HiDPI: the window is created in points (scale x 240 by scale x 135) with
// SDL_WINDOW_ALLOW_HIGHDPI, and the integer factor is chosen from the
// renderer's output size in PIXELS, so a Retina screen gets a sharp 8x.
#pragma once
#include <cstdint>

#include "pg/platform.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace desk {

class Display {
 public:
  bool open(int scale, char* err, int errCap);
  void close();
  // Converts rows y0..y1 (inclusive) of fb to the texture: what changed.
  void upload(const pg::Framebuffer& fb, int y0, int y1);
  void present();
  void setTitle(const char* title);
  uint32_t windowId() const;
  int pixelScale() const { return lastScale_; }   // integer factor of the last present

 private:
  SDL_Window* win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture* tex_ = nullptr;
  uint32_t argb_[pg::kScreenW * pg::kScreenH] = {};
  int lastScale_ = 0;
};

// RGB565 -> 8-bit channels the way the screenshots (host/common/ui_harness.h)
// and the mockups expand them, so the window shows the same colours.
inline uint32_t rgb565ToArgb(uint16_t v) {
  const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

}  // namespace desk
