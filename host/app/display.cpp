#include "display.h"

#include <SDL.h>

#include <cstdio>

namespace desk {

bool Display::open(int scale, char* err, int errCap) {
  // Nearest-neighbour for the texture (the factor is an integer anyway).
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  win_ = SDL_CreateWindow("Pocket Groove", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, pg::kScreenW * scale,
                          pg::kScreenH * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!win_) {
    std::snprintf(err, size_t(errCap), "window: %s", SDL_GetError());
    return false;
  }
  SDL_SetWindowMinimumSize(win_, pg::kScreenW, pg::kScreenH);
  ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, 0);   // e.g. the dummy video driver: software
  if (!ren_) {
    std::snprintf(err, size_t(errCap), "renderer: %s", SDL_GetError());
    return false;
  }
  tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pg::kScreenW, pg::kScreenH);
  if (!tex_) {
    std::snprintf(err, size_t(errCap), "texture: %s", SDL_GetError());
    return false;
  }
#if SDL_VERSION_ATLEAST(2, 0, 12)
  SDL_SetTextureScaleMode(tex_, SDL_ScaleModeNearest);
#endif
  // A binary started from Terminal is not a bundled app: make sure the window
  // comes to the front and takes keyboard focus.
  SDL_RaiseWindow(win_);
  return true;
}

void Display::close() {
  if (tex_) SDL_DestroyTexture(tex_);
  if (ren_) SDL_DestroyRenderer(ren_);
  if (win_) SDL_DestroyWindow(win_);
  tex_ = nullptr;
  ren_ = nullptr;
  win_ = nullptr;
}

void Display::upload(const pg::Framebuffer& fb, int y0, int y1) {
  if (!tex_) return;
  if (y0 < 0) y0 = 0;
  if (y1 > pg::kScreenH - 1) y1 = pg::kScreenH - 1;
  if (y1 < y0) return;
  uint32_t lut[256];
  for (int i = 0; i < 256; ++i) lut[i] = rgb565ToArgb(fb.palette[i]);
  for (int y = y0; y <= y1; ++y) {
    const uint8_t* s = fb.px + y * pg::kScreenW;
    uint32_t* d = argb_ + y * pg::kScreenW;
    for (int x = 0; x < pg::kScreenW; ++x) d[x] = lut[s[x]];
  }
  SDL_Rect r{0, y0, pg::kScreenW, y1 - y0 + 1};
  SDL_UpdateTexture(tex_, &r, argb_ + y0 * pg::kScreenW, pg::kScreenW * 4);
}

void Display::present() {
  if (!ren_) return;
  int w = 0, h = 0;
  if (SDL_GetRendererOutputSize(ren_, &w, &h) != 0 || w <= 0 || h <= 0) SDL_GetWindowSize(win_, &w, &h);
  int s = w / pg::kScreenW < h / pg::kScreenH ? w / pg::kScreenW : h / pg::kScreenH;
  if (s < 1) s = 1;
  lastScale_ = s;
  const SDL_Rect dst{(w - pg::kScreenW * s) / 2, (h - pg::kScreenH * s) / 2, pg::kScreenW * s, pg::kScreenH * s};
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
  SDL_RenderClear(ren_);
  SDL_RenderCopy(ren_, tex_, nullptr, &dst);
  SDL_RenderPresent(ren_);
}

void Display::setTitle(const char* title) {
  if (win_) SDL_SetWindowTitle(win_, title);
}

uint32_t Display::windowId() const { return win_ ? SDL_GetWindowID(win_) : 0; }

}  // namespace desk
