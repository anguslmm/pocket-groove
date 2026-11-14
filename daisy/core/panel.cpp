#include "panel.h"

namespace pgd {

namespace {
constexpr uint8_t kSwReset = 0x01, kSlpOut = 0x11, kNorOn = 0x13, kInvOn = 0x21, kInvOff = 0x20, kDispOn = 0x29;
constexpr uint8_t kCaset = 0x2A, kRaset = 0x2B, kRamWr = 0x2C, kMadctl = 0x36, kColmod = 0x3A;
}  // namespace

PanelConfig PanelConfig::st7789_240x135() { return PanelConfig{}; }

PanelConfig PanelConfig::hx8357d_480x320() {
  PanelConfig c;
  c.controller = PanelController::HX8357D;
  c.width = 480;
  c.height = 320;
  c.ramX = 0;
  c.ramY = 0;
  c.madctl = 0x60;   // landscape (MV | MX); to be confirmed on the panel
  c.invert = false;
  c.scale = 2;
  c.x0 = 0;
  c.y0 = (320 - 2 * pg::kScreenH) / 2;   // 25
  return c;
}

PanelConfig PanelConfig::ili9341_320x240(bool turnedOver) {
  PanelConfig c;
  c.controller = PanelController::ILI9341;
  c.width = 320;
  c.height = 240;
  c.ramX = 0;
  c.ramY = 0;
  c.madctl = turnedOver ? kIliMadctlTurned : kIliMadctl;
  c.invert = false;
  c.scale = 1;
  c.x0 = (320 - pg::kScreenW) / 2;   // 40
  c.y0 = (240 - pg::kScreenH) / 2;   // 52 (52 above, 53 below)
  return c;
}

const char* PanelConfig::name() const {
  switch (controller) {
    case PanelController::ST7789: return "ST7789";
    case PanelController::HX8357D: return "HX8357D";
    case PanelController::ILI9341: return "ILI9341";
  }
  return "?";
}

namespace {
// The ILI9341's setup before sleep-out, as Adafruit_ILI9341 1.7.0 sends it
// (Adafruit_ILI9341.cpp initcmd[]; BSD / MIT licence, see docs/DAISY.md "Licences"):
// command, parameter count, parameters. The undocumented 0xEF and the power / VCOM /
// gamma values are the panel vendor's; MADCTL is ours (cfg.madctl), and sleep-out /
// display-on come from the common path below with the same 150 ms waits.
constexpr uint8_t kIliInit[] = {
    0xEF, 3, 0x03, 0x80, 0x02,
    0xCF, 3, 0x00, 0xC1, 0x30,                 // power control B
    0xED, 4, 0x64, 0x03, 0x12, 0x81,           // power on sequence
    0xE8, 3, 0x85, 0x00, 0x78,                 // driver timing A
    0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,     // power control A
    0xF7, 1, 0x20,                             // pump ratio
    0xEA, 2, 0x00, 0x00,                       // driver timing B
    0xC0, 1, 0x23,                             // PWCTR1: VRH
    0xC1, 1, 0x10,                             // PWCTR2: SAP, BT
    0xC5, 2, 0x3E, 0x28,                       // VMCTR1
    0xC7, 1, 0x86,                             // VMCTR2
    0x37, 1, 0x00,                             // VSCRSADD: no vertical scroll
    0xB1, 2, 0x00, 0x18,                       // FRMCTR1: 79 Hz
    0xB6, 3, 0x08, 0x82, 0x27,                 // DFUNCTR
    0xF2, 1, 0x00,                             // 3-gamma off
    0x26, 1, 0x01,                             // GAMMASET: curve 1
    0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,   // GMCTRP1
    0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,   // GMCTRN1
};
}  // namespace

void Panel::init(const PanelConfig& cfg, SpiPanelBus* bus, uint8_t* strips) {
  cfg_ = cfg;
  if (cfg_.scale < 1) cfg_.scale = 1;
  if (cfg_.scale > kMaxScale) cfg_.scale = kMaxScale;
  bus_ = bus;
  strip_[0] = strips;
  strip_[1] = strips ? strips + kStripBytes : nullptr;
  active_ = started_ = false;
  qy0_ = qy1_ = 0;
}

void Panel::begin() {
  if (!bus_) return;
  bus_->command(kSwReset, nullptr, 0);
  bus_->delayMs(150);
  const bool ili = cfg_.controller == PanelController::ILI9341;
  if (cfg_.controller == PanelController::HX8357D) {
    const uint8_t ext[3] = {0xFF, 0x83, 0x57};   // SETEXTC: unlock the extended commands
    bus_->command(0xB9, ext, 3);
    bus_->delayMs(5);
  }
  const uint8_t px = 0x55;   // 16 bits per pixel
  if (ili) {
    // Adafruit's order: the whole setup (MADCTL and the pixel format in it), then sleep out.
    for (size_t i = 0; i < sizeof(kIliInit);) {
      const uint8_t cmd = kIliInit[i], n = kIliInit[i + 1];
      bus_->command(cmd, kIliInit + i + 2, n);
      i += 2u + n;
    }
    bus_->command(kMadctl, &cfg_.madctl, 1);
    bus_->command(kColmod, &px, 1);
    bus_->command(kSlpOut, nullptr, 0);
    bus_->delayMs(150);
  } else {
    bus_->command(kSlpOut, nullptr, 0);
    bus_->delayMs(120);
    bus_->command(kColmod, &px, 1);
    bus_->command(kMadctl, &cfg_.madctl, 1);
  }
  bus_->command(cfg_.invert ? kInvOn : kInvOff, nullptr, 0);
  bus_->command(kNorOn, nullptr, 0);
  // Clear the whole panel (the scaled UI may not cover it), one black strip at a time.
  for (size_t i = 0; i < kStripBytes && strip_[0]; ++i) strip_[0][i] = 0;
  window(0, 0, cfg_.width, cfg_.height);
  const size_t total = size_t(cfg_.width) * cfg_.height * 2;
  for (size_t done = 0; done < total && strip_[0];) {
    const size_t n = total - done < kStripBytes ? total - done : kStripBytes;
    bus_->pixelsAsync(strip_[0], n);
    while (bus_->busy()) {
    }
    done += n;
  }
  bus_->command(kDispOn, nullptr, 0);
  bus_->delayMs(ili ? 150 : 20);
  active_ = started_ = false;
  if (fb_) present(*fb_, 0, pg::kScreenH);   // a re-begin: the picture again
}

void Panel::window(int px0, int py0, int pw, int ph) {
  const int xa = cfg_.ramX + px0, xb = xa + pw - 1, ya = cfg_.ramY + py0, yb = ya + ph - 1;
  const uint8_t c[4] = {uint8_t(xa >> 8), uint8_t(xa), uint8_t(xb >> 8), uint8_t(xb)};
  const uint8_t r[4] = {uint8_t(ya >> 8), uint8_t(ya), uint8_t(yb >> 8), uint8_t(yb)};
  bus_->command(kCaset, c, 4);
  bus_->command(kRaset, r, 4);
  bus_->command(kRamWr, nullptr, 0);
}

void Panel::present(const pg::Framebuffer& fb, int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > pg::kScreenH) y1 = pg::kScreenH;
  if (y0 >= y1 || !bus_ || !strip_[0]) return;
  fb_ = &fb;
  if (qy1_ > qy0_) {
    if (y0 < qy0_) qy0_ = y0;
    if (y1 > qy1_) qy1_ = y1;
  } else {
    qy0_ = y0;
    qy1_ = y1;
  }
  if (!active_) step();
}

void Panel::fill(int buf, int y, int rows) {
  const int s = cfg_.scale;
  uint8_t* d = strip_[buf];
  for (int r = 0; r < rows; ++r) {
    const uint8_t* src = fb_->px + (y + r) * pg::kScreenW;
    for (int rep = 0; rep < s; ++rep) {
      for (int x = 0; x < pg::kScreenW; ++x) {
        const uint16_t v = pal_[src[x]];
        for (int k = 0; k < s; ++k) {
          *d++ = uint8_t(v);         // pal_ is byte-swapped already: low byte first = the colour's high byte
          *d++ = uint8_t(v >> 8);
        }
      }
    }
  }
}

bool Panel::step() {
  if (!bus_) return false;
  if (!active_) {
    if (qy1_ <= qy0_ || !fb_) return false;
    for (int i = 0; i < 256; ++i) pal_[i] = uint16_t((fb_->palette[i] << 8) | (fb_->palette[i] >> 8));
    y_ = qy0_;
    y1_ = qy1_;
    qy0_ = qy1_ = 0;
    const int s = cfg_.scale;
    window(cfg_.x0, cfg_.y0 + y_ * s, pg::kScreenW * s, (y1_ - y_) * s);
    active_ = true;
    started_ = false;
    buf_ = 0;
  }
  // Fill the buffer that is not on the bus, then send it once the bus is free.
  if (!started_ && y_ < y1_) {
    const int rows = y1_ - y_ < kStripRows ? y1_ - y_ : kStripRows;
    fill(buf_, y_, rows);
    started_ = true;   // "filled, waiting for the bus"
  }
  if (bus_->busy()) return true;
  if (started_) {
    const int rows = y1_ - y_ < kStripRows ? y1_ - y_ : kStripRows;
    const int s = cfg_.scale;
    bus_->pixelsAsync(strip_[buf_], size_t(pg::kScreenW) * s * rows * s * 2);
    y_ += rows;
    buf_ ^= 1;
    started_ = false;
    if (y_ < y1_) {   // fill the next one while this one goes out
      const int next = y1_ - y_ < kStripRows ? y1_ - y_ : kStripRows;
      fill(buf_, y_, next);
      started_ = true;
    }
    return true;
  }
  if (y_ >= y1_) {
    active_ = false;
    ++frames_;
    if (qy1_ > qy0_) return step();
  }
  return active_;
}

}  // namespace pgd
