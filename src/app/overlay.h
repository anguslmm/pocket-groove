// Pocket Groove firmware -- overlays drawn by the firmware on top of the UI:
// the diagnostics strip, the codec volume bar and a one-line alert (no audio /
// CPU overload). They are composited into the RGB565 strips on their way to
// the panel (plat::display::StripHook), so the UI's Framebuffer is never
// touched and the UI never has to redraw what an overlay covered.
// UI thread only. Portable: no hardware includes.
#pragma once
#include <cstdint>

namespace app {

constexpr int kOvCols = 40;   // 6 px cells: 240 px
// Text lines per box: the USB audio build's diagnostics strip has a fourth
// (USB) line.
#if PG_USB_AUDIO
constexpr int kOvLines = 4;
#else
constexpr int kOvLines = 3;
#endif

struct OverlayBox {
  bool     on = false;
  int16_t  x = 0, y = 0, w = 0, h = 0;
  uint16_t bg = 0, border = 0;          // RGB565 (normal byte order); border == bg: none
  uint8_t  lines = 0;                   // 0..kOvLines text lines, 8 px apart from y + 2
  char     text[kOvLines][kOvCols + 1] = {};
#if PG_USB_AUDIO
  uint16_t fg[kOvLines] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
#else
  uint16_t fg[kOvLines] = {0xFFFF, 0xFFFF, 0xFFFF};
#endif
  int8_t   barPct = -1;                 // -1: no bar
  int16_t  barY = 0, barH = 0;          // bar spans x + 4 .. x + w - 4
  uint16_t barFg = 0xFFFF, barBg = 0;
};

enum OverlayId : uint8_t { kOvDiag = 0, kOvVolume, kOvAlert, kNumOverlays };

class Overlays {
 public:
  OverlayBox& box(OverlayId id) { return box_[id]; }
  // Call after editing boxes: compares with what was last pushed and returns
  // the row range [*y0, *y1) that must be re-sent to the panel (empty: false).
  bool takeDirty(int* y0, int* y1);
  // plat::display::StripHook
  static void hook(uint16_t* px, int y, int rows, void* self);

 private:
  void render(uint16_t* px, int y, int rows) const;
  uint32_t signature() const;
  OverlayBox box_[kNumOverlays];
  uint32_t lastSig_ = 0;
  int lastY0_ = 0, lastY1_ = 0;         // rows covered by what was last pushed
};

// RGB565 colours used by the overlays.
constexpr uint16_t kRgbBlack = 0x0000, kRgbWhite = 0xFFFF, kRgbGrey = 0x8410, kRgbDark = 0x18E3, kRgbPanel = 0x2124,
                   kRgbGreen = 0x07E0, kRgbRed = 0xF800, kRgbDkRed = 0x7800, kRgbAmber = 0xFD20, kRgbCyan = 0x07FF;

}  // namespace app
