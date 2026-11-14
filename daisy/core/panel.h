// An SPI display for the Daisy (stage 1; the panel is not chosen yet: docs/DAISY.md
// "Display"). The UI draws into its 240x135 8-bit framebuffer (pg/platform.h); this
// sends the rows the UI reports dirty to a MIPI-DCS panel (ST7789, HX8357D: both use
// CASET 0x2A / RASET 0x2B / RAMWR 0x2C and RGB565 big-endian over 4-wire SPI), scaled
// by an integer factor and placed at an offset, so the same code drives the
// Cardputer's 1.14" 240x135 at scale 1 or a 3.5" 480x320 at scale 2 (480x270, 25
// rows spare above and below). The panel's size, controller, offsets and scale are
// configuration (PanelConfig), not constants: nothing else in the target knows them.
//
// The expander is the Cardputer's (src/platform/display.cpp): each strip of rows is
// palette-expanded into a buffer in the panel's byte order (palette[index] with the
// bytes swapped) while the previous strip is on the bus (DMA), so there is no 16-bit
// copy of the screen. Here it is a main-loop state machine: step() fills the next
// strip when its buffer is free and starts its transfer, and returns at once while
// a transfer runs.
// Portable over SpiPanelBus (host/daisy/test_daisy_panel.cpp: against a simulated
// panel that executes the commands, the result equals the Cardputer path's pixels).
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/platform.h"

namespace pgd {

// The SPI link to the panel. Commands and their parameters are short and blocking;
// pixel data goes out asynchronously (DMA) from a buffer that stays untouched until
// busy() is false again.
class SpiPanelBus {
 public:
  virtual void command(uint8_t cmd, const uint8_t* params, size_t n) = 0;
  virtual void pixelsAsync(const uint8_t* data, size_t n) = 0;   // after a RAMWR command
  virtual bool busy() = 0;
  virtual void delayMs(uint32_t ms) = 0;

 protected:
  ~SpiPanelBus() = default;
};

enum class PanelController : uint8_t { ST7789, HX8357D };

struct PanelConfig {
  PanelController controller = PanelController::ST7789;
  uint16_t width = 240, height = 135;   // the panel in its landscape orientation
  uint16_t ramX = 40, ramY = 53;        // the visible area's origin in the controller's RAM
  uint8_t madctl = 0x70;                // memory access control (orientation, RGB order)
  bool invert = true;                   // INVON (the Cardputer's IPS panel needs it)
  uint8_t scale = 1;                    // integer scale of the UI's 240x135
  uint16_t x0 = 0, y0 = 0;              // where the scaled UI sits on the panel

  // The Cardputer's panel (ST7789V2 1.14" 240x135, as M5GFX sets it up: offsets 40/53
  // in landscape; the Adafruit 6113 is the same panel, its offsets to be confirmed).
  static PanelConfig st7789_240x135();
  // Adafruit 5846 class (HX8357D 3.5" 480x320): the UI at scale 2, centred.
  static PanelConfig hx8357d_480x320();
};

class Panel {
 public:
  static constexpr int kStripRows = 8;   // UI rows per strip (scaled: rows * scale panel rows)
  static constexpr int kMaxScale = 2;
  static constexpr size_t kStripBytes = size_t(pg::kScreenW) * kMaxScale * kStripRows * kMaxScale * 2;

  // strips: 2 * kStripBytes of DMA-able memory (the Daisy: not cached, D2).
  void init(const PanelConfig& cfg, SpiPanelBus* bus, uint8_t* strips);
  // Blocking: software reset (the 6113 and 5846 have no reset pin wired), sleep out,
  // pixel format, orientation, inversion, clear to black, display on (~300 ms).
  void begin();
  const PanelConfig& config() const { return cfg_; }

  // Queue UI rows [y0, y1) of fb (merged with rows already queued).
  void present(const pg::Framebuffer& fb, int y0, int y1);
  // Main loop: moves the transfer on. Returns true while busy.
  bool step();
  bool busy() const { return active_; }
  uint32_t framesSent() const { return frames_; }

 private:
  void window(int px0, int py0, int pw, int ph);
  void fill(int buf, int y, int rows);

  PanelConfig cfg_;
  SpiPanelBus* bus_ = nullptr;
  uint8_t* strip_[2] = {nullptr, nullptr};
  const pg::Framebuffer* fb_ = nullptr;
  uint16_t pal_[256] = {};   // panel byte order
  int qy0_ = 0, qy1_ = 0;     // queued rows
  int y_ = 0, y1_ = 0, buf_ = 0;
  bool active_ = false, started_ = false;
  uint32_t frames_ = 0;
};

}  // namespace pgd
