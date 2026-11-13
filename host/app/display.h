// The window: the 240x135 pg::Framebuffer, integer-scaled with nearest
// neighbour, with the desktop strip (strip.h, kStripH device pixels) under it
// at the same factor; the whole 240 x (135 + kStripH) canvas is centred on
// black when the window's shape differs. While the Stems list is open, its
// own 240x135 framebuffer is shown where the device screen is. With the macro
// lab (--lab, lab.h) a panel kLabW device pixels wide and the full canvas
// height sits to the right of the screen and strip.
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
constexpr int kLabW = 184;   // the macro lab panel (lab.h), when on

class Display {
 public:
  // lab: also show the macro lab panel (the canvas grows by kLabW).
  bool open(int scale, char* err, int errCap, bool lab = false);
  void close();
  // Converts rows y0..y1 (inclusive) of fb to the texture: what changed.
  void upload(const pg::Framebuffer& fb, int y0, int y1);
  void uploadStrip(const pg::Framebuffer& fb);     // its top kStripH rows
  void uploadOverlay(const pg::Framebuffer& fb);   // the Stems list
  // The lab panel: kLabW x kCanvasH palette indices, row-major, and the palette.
  void uploadLab(const uint8_t* px, const uint16_t* palette);
  int canvasW() const { return canvasW_; }
  void present(bool overlay);
  void setTitle(const char* title);
  uint32_t windowId() const;
  int pixelScale() const { return lastScale_; }   // integer factor of the last present
  // Window coordinates (points, as in mouse events) <-> canvas device pixels.
  bool toCanvas(int wx, int wy, int* cx, int* cy) const;
  void fromCanvas(int cx, int cy, int* wx, int* wy) const;
  // As toCanvas, but also outside the canvas (a drag that leaves it).
  void toCanvasAny(int wx, int wy, int* cx, int* cy) const;

 private:
  void geometry(int* s, int* ox, int* oy, int* outW, int* outH) const;
  SDL_Window* win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture* tex_ = nullptr;
  SDL_Texture* strip_ = nullptr;
  SDL_Texture* overlay_ = nullptr;
  SDL_Texture* lab_ = nullptr;
  int canvasW_ = kCanvasW;
  uint32_t argb_[pg::kScreenW * pg::kScreenH] = {};
  uint32_t tmp_[pg::kScreenW * pg::kScreenH] = {};
  uint32_t labPx_[kLabW * kCanvasH] = {};
  int lastScale_ = 0;
};

// RGB565 -> 8-bit channels the way the screenshots (host/common/ui_harness.h)
// and the mockups expand them, so the window shows the same colours.
inline uint32_t rgb565ToArgb(uint16_t v) {
  const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

}  // namespace desk
