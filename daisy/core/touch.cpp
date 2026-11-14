#include "touch.h"

namespace pgd {

namespace {
constexpr uint8_t kRegFirst = 0x00, kRegThreshold = 0x80, kRegChipId = 0xA3, kRegFirmware = 0xA6, kRegVendor = 0xA8;
constexpr int kRegBytes = 15;   // 0x00..0x0E: mode, gesture, count, two points of 6 bytes
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

int TouchMap::orientationFor(uint8_t madctl) {
  switch (madctl & 0xE0) {   // MY MX MV (the BGR and other bits do not move pixels)
    case 0x20: return 3;     // rotation 1 (landscape): x = W' - 1 - ty, y = tx
    case 0xE0: return 5;     // rotation 3 (landscape, turned over): x = ty, y = H' - 1 - tx
    case 0x40: return 6;     // rotation 0 (portrait): x = 239 - tx, y = 319 - ty
    case 0x80: return 0;     // rotation 2 (portrait, turned over)
    default: return -1;
  }
}

void TouchMap::mapToPanel(int tx, int ty, int* px, int* py) const {
  if (tx < 0) tx = 0;
  if (ty < 0) ty = 0;
  if (tx >= nativeW) tx = nativeW - 1;
  if (ty >= nativeH) ty = nativeH - 1;
  const bool swap = orientation & 1;
  int a = swap ? ty : tx, b = swap ? tx : ty;
  const int aw = swap ? nativeH : nativeW, bh = swap ? nativeW : nativeH;
  if (orientation & 2) a = aw - 1 - a;
  if (orientation & 4) b = bh - 1 - b;
  *px = a;
  *py = b;
}

void TouchMap::toCanvas(int px, int py, int* cx, int* cy) const {
  const int s = scale ? scale : 1;
  *cx = floorDiv(px - x0, s);
  *cy = floorDiv(py - y0, s);
}

bool Ft6206::parse(const uint8_t* r, TouchFrame* out) {
  *out = TouchFrame{};
  const uint8_t n = r[0x02] & 0x0F;
  const bool ok = n <= 2 && (r[0x02] & 0xF0) == 0;
  out->count = ok ? n : 0;
  for (int i = 0; i < 2; ++i) {
    const uint8_t* p = r + 0x03 + i * 6;
    out->p[i].x = uint16_t(((p[0] & 0x0F) << 8) | p[1]);
    out->p[i].y = uint16_t(((p[2] & 0x0F) << 8) | p[3]);
    out->p[i].id = uint8_t(p[2] >> 4);
    out->p[i].event = uint8_t(p[0] >> 6);
  }
  return ok;
}

bool Ft6206::chipKnown(uint8_t id) { return id == 0x06 || id == 0x36 || id == 0x64; }

const char* Ft6206::stateName(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::Probe: return "probing";
    case State::Ready: return "present";
    case State::Absent: return "NOT FOUND at 0x38";
    case State::Lost: return "LOST (reads failed; probing again)";
  }
  return "?";
}

void Ft6206::begin(I2cBus* bus, uint32_t nowMs) {
  bus_ = bus;
  st_ = bus ? State::Probe : State::Off;
  next_ = nowMs;
  fails_ = 0;
  down_ = false;
  qn_ = 0;
}

void Ft6206::push(uint8_t kind, uint16_t x, uint16_t y, uint32_t ms) {
  if (qn_ >= kQueue) {
    ++dropped_;
    return;
  }
  q_[qn_++] = Event{kind, x, y, ms};
}

bool Ft6206::pop(Event* e) {
  if (!qn_) return false;
  *e = q_[0];
  for (int i = 1; i < qn_; ++i) q_[i - 1] = q_[i];
  --qn_;
  return true;
}

void Ft6206::probe(uint32_t nowMs) {
  uint8_t vend = 0, chip = 0, fw = 0;
  if (bus_->read(kAddr, kRegVendor, &vend, 1) && vend == kVendorId && bus_->read(kAddr, kRegChipId, &chip, 1) &&
      chipKnown(chip)) {
    bus_->read(kAddr, kRegFirmware, &fw, 1);
    const uint8_t th = kThreshold;
    bus_->write(kAddr, kRegThreshold, &th, 1);
    chip_ = chip;
    fw_ = fw;
    fails_ = 0;
    st_ = State::Ready;
    next_ = nowMs;
    return;
  }
  chip_ = chip;
  if (st_ != State::Lost) st_ = State::Absent;
  next_ = nowMs + kReprobeMs;
}

bool Ft6206::step(uint32_t nowMs) {
  if (!bus_ || st_ == State::Off) return false;
  if (int32_t(nowMs - next_) < 0) return false;
  if (st_ != State::Ready) {
    probe(nowMs);
    return true;
  }
  next_ = nowMs + kPollMs;
  uint8_t r[kRegBytes];
  if (!bus_->read(kAddr, kRegFirst, r, sizeof(r))) {
    ++errors_;
    if (++fails_ >= 3) {
      if (down_) push(Event::Up, lx_, ly_, nowMs);
      down_ = false;
      st_ = State::Lost;
      next_ = nowMs + kReprobeMs;
    }
    return true;
  }
  fails_ = 0;
  ++reads_;
  TouchFrame f;
  parse(r, &f);
  if (f.count > 0) {
    const uint16_t x = f.p[0].x, y = f.p[0].y;
    if (!down_) {
      down_ = true;
      ++touches_;
      push(Event::Down, x, y, nowMs);
    } else if (x != lx_ || y != ly_) {
      push(Event::Move, x, y, nowMs);
    }
    lx_ = x;
    ly_ = y;
  } else if (down_) {
    down_ = false;
    push(Event::Up, lx_, ly_, nowMs);
  }
  return true;
}

}  // namespace pgd
