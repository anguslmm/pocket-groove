// Pocket Groove platform layer -- LCD (ST7789V2, 240x135 landscape, SPI3 at
// 40 MHz, backlight PWM on GPIO38).
//
// present() expands a pg::Framebuffer (8-bit indices + RGB565 palette) into
// two small strip buffers in the panel's byte order and streams them with DMA:
// while strip n is on the bus, strip n+1 is being expanded. No full-screen
// 16-bit sprite (that would cost 64.8 KB of a heap that has no PSRAM behind it).
//
// UI thread only. No Arduino / M5 / ESP-IDF types in this header.
#pragma once
#include <cstdint>

#include "pg/platform.h"

namespace plat {
namespace display {

struct Stats {
  uint32_t frames;        // present() calls since begin
  float    lastUs;        // last present(): expand + transfer, start to DMA done
  float    maxUs;
  float    avgUs;         // moving average (time constant ~16 frames)
  float    fps;           // frames presented in the last full second
};

// Requires plat::sys::begin() (which brings up the panel). Returns false if no
// panel was detected; present() is then a no-op.
bool  begin();
void  present(const pg::Framebuffer& fb);
// Only the rows [y0, y1) -- for partial updates.
void  presentRows(const pg::Framebuffer& fb, int y0, int y1);
// Same, with a compositing hook: after each strip of rows [y, y + rows) has been
// expanded to RGB565 (panel byte order, i.e. byte-swapped, kScreenW px per row)
// and before it is sent, hook(px, y, rows, user) may draw over it. This is how
// firmware overlays sit on top of the UI without touching the Framebuffer.
using StripHook = void (*)(uint16_t* px, int y, int rows, void* user);
void  presentRows(const pg::Framebuffer& fb, int y0, int y1, StripHook hook, void* user);
// Backlight. M5GFX 0.2.36 drives GPIO38 with LEDC at 256 Hz / 9 bit for this
// board (M5GFX.cpp _set_pwm_backlight(GPIO_NUM_38, 7, 256, false, 16); on Arduino
// core 3.x the channel number is ignored and ledcAttach() takes the first free
// channel). 256 Hz edges coupled into the headphone output as an audible buzz
// at dimmed levels, so begin() moves that LEDC timer to kBacklightPwmHz, keeping
// M5GFX's 9-bit duty scale. Level 255 is duty 512/512: the pin is held high, DC,
// no PWM edges at all; 0 is held low. Anything in between switches at
// kBacklightPwmHz, above the audio band.
constexpr uint8_t  kBrightnessFull  = 255;      // DC: what the instrument uses while sound can be heard
constexpr uint32_t kBacklightPwmHz  = 39062;    // 40 MHz XTAL / 512 / 2: integer divider, no fractional jitter
struct Backlight {
  bool     reconfigured;   // the LEDC timer now runs at pwmHz
  uint32_t stockHz;        // what M5GFX had set up (read back from the LEDC timer), 0 = unknown
  uint32_t pwmHz;          // read back after the change (0 = change failed: still stockHz)
  uint8_t  bits;           // duty resolution (9, M5GFX's)
  uint32_t duty;           // current raw duty, 0..dutyFull
  uint32_t dutyFull;       // 1 << bits = always on
  bool     dc;             // duty is 0 or full: no switching edges
};
Backlight backlight();

// Unbuffered text straight to the panel with the library's built-in font, for
// boot and fatal-error messages before/without a Framebuffer. Not for the UI.
void  directText(int x, int y, const char* text, uint16_t rgb565, uint8_t size = 1);
void  setBrightness(uint8_t level);       // 0..255 (255 = DC, see Backlight)
uint8_t brightness();
Stats stats();
void  resetStats();

}  // namespace display
}  // namespace plat
