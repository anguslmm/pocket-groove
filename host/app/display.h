// The window: the 240x135 pg::Framebuffer, integer-scaled with nearest
// neighbour, with the desktop strip (strip.h, kStripH device pixels) under it
// at the same factor; the whole 240 x (135 + kStripH) canvas is centred on
// black when the window's shape differs. While the Stems list is open, its
// own 240x135 framebuffer is shown where the device screen is.
// HiDPI: the window is created in points (scale x the canvas) with
// SDL_WINDOW_ALLOW_HIGHDPI, and the integer factor is chosen from the
// renderer's output size in PIXELS, so a Retina screen gets a sharp 8x.
#pragma once
#include <cstdint>

#include "pg/platform.h"
#include "strip.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace desk {

constexpr int kCanvasW = pg::kScreenW, kCanvasH = pg::kScreenH + kStripH;

class Display {
 public:
  bool open(int scale, char* err, int errCap);
  void close();
  // Converts rows y0..y1 (inclusive) of fb to the texture: what changed.
  void upload(const pg::Framebuffer& fb, int y0, int y1);
  void uploadStrip(const pg::Framebuffer& fb);     // its top kStripH rows
  void uploadOverlay(const pg::Framebuffer& fb);   // the Stems list
  void present(bool overlay);
  void setTitle(const char* title);
  uint32_t windowId() const;
  int pixelScale() const { return lastScale_; }   // integer factor of the last present
  // Window coordinates (points, as in mouse events) <-> canvas device pixels.
  bool toCanvas(int wx, int wy, int* cx, int* cy) const;
  void fromCanvas(int cx, int cy, int* wx, int* wy) const;

 private:
  void geometry(int* s, int* ox, int* oy, int* outW, int* outH) const;
  SDL_Window* win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture* tex_ = nullptr;
  SDL_Texture* strip_ = nullptr;
  SDL_Texture* overlay_ = nullptr;
  uint32_t argb_[pg::kScreenW * pg::kScreenH] = {};
  uint32_t tmp_[pg::kScreenW * pg::kScreenH] = {};
  int lastScale_ = 0;
};

// RGB565 -> 8-bit channels the way the screenshots (host/common/ui_harness.h)
// and the mockups expand them, so the window shows the same colours.
inline uint32_t rgb565ToArgb(uint16_t v) {
  const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

}  // namespace desk
