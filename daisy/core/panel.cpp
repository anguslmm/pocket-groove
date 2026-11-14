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
  if (cfg_.controller == PanelController::HX8357D) {
    const uint8_t ext[3] = {0xFF, 0x83, 0x57};   // SETEXTC: unlock the extended commands
    bus_->command(0xB9, ext, 3);
    bus_->delayMs(5);
  }
  bus_->command(kSlpOut, nullptr, 0);
  bus_->delayMs(120);
  const uint8_t px = 0x55;   // 16 bits per pixel
  bus_->command(kColmod, &px, 1);
  bus_->command(kMadctl, &cfg_.madctl, 1);
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
  bus_->delayMs(20);
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
