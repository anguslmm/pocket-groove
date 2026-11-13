#include "display.h"

#include <SDL.h>

#include <cstdio>

namespace desk {

bool Display::open(int scale, char* err, int errCap, bool lab) {
  canvasW_ = kCanvasW + (lab ? kLabW : 0);
  // Nearest-neighbour for the texture (the factor is an integer anyway).
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  win_ = SDL_CreateWindow("Pocket Groove", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, canvasW_ * scale,
                          kCanvasH * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!win_) {
    std::snprintf(err, size_t(errCap), "window: %s", SDL_GetError());
    return false;
  }
  SDL_SetWindowMinimumSize(win_, canvasW_, kCanvasH);
  ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, 0);   // e.g. the dummy video driver: software
  if (!ren_) {
    std::snprintf(err, size_t(errCap), "renderer: %s", SDL_GetError());
    return false;
  }
  tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pg::kScreenW, pg::kScreenH);
  strip_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kCanvasW, kStripH);
  overlay_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pg::kScreenW,
                               pg::kScreenH);
  if (lab) lab_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kLabW, kCanvasH);
  if (!tex_ || !strip_ || !overlay_ || (lab && !lab_)) {
    std::snprintf(err, size_t(errCap), "texture: %s", SDL_GetError());
    return false;
  }
#if SDL_VERSION_ATLEAST(2, 0, 12)
  SDL_SetTextureScaleMode(tex_, SDL_ScaleModeNearest);
  SDL_SetTextureScaleMode(strip_, SDL_ScaleModeNearest);
  SDL_SetTextureScaleMode(overlay_, SDL_ScaleModeNearest);
  if (lab_) SDL_SetTextureScaleMode(lab_, SDL_ScaleModeNearest);
#endif
  // A binary started from Terminal is not a bundled app: make sure the window
  // comes to the front and takes keyboard focus.
  SDL_RaiseWindow(win_);
  return true;
}

void Display::close() {
  for (SDL_Texture** t : {&tex_, &strip_, &overlay_, &lab_}) {
    if (*t) SDL_DestroyTexture(*t);
    *t = nullptr;
  }
  if (ren_) SDL_DestroyRenderer(ren_);
  if (win_) SDL_DestroyWindow(win_);
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

namespace {
void convert(const pg::Framebuffer& fb, int rows, uint32_t* out) {
  uint32_t lut[256];
  for (int i = 0; i < 256; ++i) lut[i] = rgb565ToArgb(fb.palette[i]);
  for (int i = 0; i < pg::kScreenW * rows; ++i) out[i] = lut[fb.px[i]];
}
}  // namespace

void Display::uploadStrip(const pg::Framebuffer& fb) {
  if (!strip_) return;
  convert(fb, kStripH, tmp_);
  SDL_UpdateTexture(strip_, nullptr, tmp_, pg::kScreenW * 4);
}

void Display::uploadOverlay(const pg::Framebuffer& fb) {
  if (!overlay_) return;
  convert(fb, pg::kScreenH, tmp_);
  SDL_UpdateTexture(overlay_, nullptr, tmp_, pg::kScreenW * 4);
}

void Display::uploadLab(const uint8_t* px, const uint16_t* palette) {
  if (!lab_) return;
  uint32_t lut[256];
  for (int i = 0; i < 256; ++i) lut[i] = rgb565ToArgb(palette[i]);
  for (int i = 0; i < kLabW * kCanvasH; ++i) labPx_[i] = lut[px[i]];
  SDL_UpdateTexture(lab_, nullptr, labPx_, kLabW * 4);
}

void Display::geometry(int* s, int* ox, int* oy, int* outW, int* outH) const {
  int w = 0, h = 0;
  if (!ren_ || SDL_GetRendererOutputSize(ren_, &w, &h) != 0 || w <= 0 || h <= 0) {
    if (win_) SDL_GetWindowSize(win_, &w, &h);
  }
  int f = w / canvasW_ < h / kCanvasH ? w / canvasW_ : h / kCanvasH;
  if (f < 1) f = 1;
  *s = f;
  *ox = (w - canvasW_ * f) / 2;
  *oy = (h - kCanvasH * f) / 2;
  *outW = w;
  *outH = h;
}

void Display::present(bool overlay) {
  if (!ren_) return;
  int s, ox, oy, w, h;
  geometry(&s, &ox, &oy, &w, &h);
  lastScale_ = s;
  const SDL_Rect dev{ox, oy, pg::kScreenW * s, pg::kScreenH * s};
  const SDL_Rect str{ox, oy + pg::kScreenH * s, kCanvasW * s, kStripH * s};
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
  SDL_RenderClear(ren_);
  SDL_RenderCopy(ren_, overlay ? overlay_ : tex_, nullptr, &dev);
  SDL_RenderCopy(ren_, strip_, nullptr, &str);
  if (lab_) {
    const SDL_Rect lr{ox + kCanvasW * s, oy, kLabW * s, kCanvasH * s};
    SDL_RenderCopy(ren_, lab_, nullptr, &lr);
  }
  SDL_RenderPresent(ren_);
}

bool Display::toCanvas(int wx, int wy, int* cx, int* cy) const {
  int s, ox, oy, w, h;
  geometry(&s, &ox, &oy, &w, &h);
  int ww = w, wh = h;
  if (win_) SDL_GetWindowSize(win_, &ww, &wh);
  // points -> pixels (Retina: 2 pixels per point)
  const int px = ww > 0 ? wx * w / ww : wx, py = wh > 0 ? wy * h / wh : wy;
  if (px < ox || py < oy) return false;
  *cx = (px - ox) / s;
  *cy = (py - oy) / s;
  return *cx < canvasW_ && *cy < kCanvasH;
}

void Display::toCanvasAny(int wx, int wy, int* cx, int* cy) const {
  int s, ox, oy, w, h;
  geometry(&s, &ox, &oy, &w, &h);
  int ww = w, wh = h;
  if (win_) SDL_GetWindowSize(win_, &ww, &wh);
  const int px = ww > 0 ? wx * w / ww : wx, py = wh > 0 ? wy * h / wh : wy;
  *cx = (px - ox) >= 0 ? (px - ox) / s : -1 - (ox - px) / s;
  *cy = (py - oy) >= 0 ? (py - oy) / s : -1 - (oy - py) / s;
}

void Display::fromCanvas(int cx, int cy, int* wx, int* wy) const {
  int s, ox, oy, w, h;
  geometry(&s, &ox, &oy, &w, &h);
  int ww = w, wh = h;
  if (win_) SDL_GetWindowSize(win_, &ww, &wh);
  const int px = ox + cx * s + s / 2, py = oy + cy * s + s / 2;
  *wx = w > 0 ? px * ww / w : px;
  *wy = h > 0 ? py * wh / h : py;
}

void Display::setTitle(const char* title) {
  if (win_) SDL_SetWindowTitle(win_, title);
}

uint32_t Display::windowId() const { return win_ ? SDL_GetWindowID(win_) : 0; }

}  // namespace desk
