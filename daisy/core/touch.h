// The capacitive touch of the Adafruit 2090 (stage 1): a FocalTech FT6206 on I2C1 at
// 0x38, sharing the bus with the BMI270 (i2c_bus.h). A main-loop state machine:
//   Probe   vendor id (0xA8) must read 0x11 and the chip id (0xA3) 0x06 (FT6206),
//           0x36 (FT6236) or 0x64 (FT6236U / FT6336U), as Adafruit_FT6206 checks;
//           then the touch threshold (0x80) is set to 40, the value Adafruit's
//           examples for this board use. No answer: Absent, probed again every 2 s
//           (a NAK costs microseconds).
//   Ready   every kPollMs the registers 0x00..0x0E are read in one transaction
//           (15 bytes, ~0.4 ms at 400 kHz) and parsed (parse(): TD_STATUS, then per
//           point XH (event flag in bits 7:6, X bits 11:8), XL, YH (touch id in
//           bits 7:4, Y bits 11:8), YL, as Adafruit_FT6206::readData does); the
//           first point becomes down / move / up events in the panel's own
//           (portrait, native) coordinates. Three failed reads in a row: Lost
//           (an up event if a touch was down), probed again every 2 s.
// The IRQ pin (low while touched) is not needed: polling works without it, and the
// report shows its level so the wiring can be checked.
//
// Coordinates: the FT6206 reports in the panel's native portrait frame (x 0..239,
// y 0..319). TouchMap turns that into the landscape panel the UI is shown on
// (one of the eight orientations, from the display's rotation) and then into the
// UI canvas (undoing the centring offset and the scale): see mapToPanel / toCanvas.
// Portable (host/daisy/test_daisy_touch.cpp: a register model of the chip).
#pragma once
#include <cstdint>

#include "i2c_bus.h"

namespace pgd {

struct TouchPoint {
  uint16_t x, y;   // native panel coordinates
  uint8_t id;      // the chip's touch id (0..1)
  uint8_t event;   // 0 press down, 1 lift up, 2 contact, 3 none (the chip's flag)
};
struct TouchFrame {
  uint8_t count;   // 0..2 (anything else read as 0, as Adafruit's driver does)
  TouchPoint p[2];
};

// orientation 0..7: bit 0 swap x and y, bit 1 mirror the (swapped) x, bit 2 mirror y.
struct TouchMap {
  uint8_t orientation = 3;            // the 2090 at MADCTL 0x28 (Adafruit's rotation 1)
  uint16_t nativeW = 240, nativeH = 320;
  uint16_t x0 = 40, y0 = 52;          // the UI canvas on the panel
  uint8_t scale = 1;
  // The orientation that matches an ILI9341 MADCTL (rotation 1 = 0x28, 3 = 0xE8;
  // portrait 0x48 / 0x88), from Adafruit's 2090 examples (CapTouch_onoffbutton at
  // rotation 1: x = 320 - ty, y = tx); -1 for a value not known.
  static int orientationFor(uint8_t madctl);
  void mapToPanel(int tx, int ty, int* px, int* py) const;
  // Canvas coordinates; may be outside 0..239 x 0..134 (a touch on the border).
  void toCanvas(int px, int py, int* cx, int* cy) const;
};

class Ft6206 {
 public:
  static constexpr uint8_t kAddr = 0x38;
  static constexpr uint8_t kVendorId = 0x11;
  static constexpr uint8_t kThreshold = 40;
  static constexpr uint32_t kPollMs = 16;        // ~60 Hz
  static constexpr uint32_t kReprobeMs = 2000;
  static constexpr int kQueue = 16;
  enum class State : uint8_t { Off, Probe, Ready, Absent, Lost };
  struct Event {
    enum Kind : uint8_t { Down, Move, Up };
    uint8_t kind;
    uint16_t x, y;   // native
    uint32_t ms;
  };

  // regs: the 15 bytes from register 0x00. false: malformed (count read as 0).
  static bool parse(const uint8_t* regs, TouchFrame* out);
  static bool chipKnown(uint8_t chipId);
  static const char* stateName(State s);

  void begin(I2cBus* bus, uint32_t nowMs);
  // Main loop, every pass: probes or polls when it is time. true: the bus was used.
  bool step(uint32_t nowMs);
  bool pop(Event* e);

  State state() const { return st_; }
  bool present() const { return st_ == State::Ready; }
  uint8_t chipId() const { return chip_; }
  uint8_t firmware() const { return fw_; }
  bool down() const { return down_; }
  uint16_t lastX() const { return lx_; }
  uint16_t lastY() const { return ly_; }
  uint32_t touches() const { return touches_; }   // down events since boot
  uint32_t reads() const { return reads_; }
  uint32_t errors() const { return errors_; }
  uint32_t dropped() const { return dropped_; }

 private:
  void push(uint8_t kind, uint16_t x, uint16_t y, uint32_t ms);
  void probe(uint32_t nowMs);
  I2cBus* bus_ = nullptr;
  State st_ = State::Off;
  uint32_t next_ = 0;
  uint8_t chip_ = 0, fw_ = 0, fails_ = 0;
  bool down_ = false;
  uint16_t lx_ = 0, ly_ = 0;
  uint32_t touches_ = 0, reads_ = 0, errors_ = 0, dropped_ = 0;
  Event q_[kQueue];
  int qn_ = 0;
};

}  // namespace pgd
