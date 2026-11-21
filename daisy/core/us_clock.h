// A microsecond clock from a free-running 32-bit hardware counter that ticks faster
// than 1 MHz, correct across the counter's wrap.
//
// Why: libDaisy's System::GetUs() is TIM2's 32-bit count divided by its ticks per
// microsecond (src/per/tim.cpp: GetTick() / (GetFreq() / 1000000); TIM2 runs at
// 2 x PCLK1 = 240 MHz at 480 MHz boost). So it does not wrap at 2^32 us (71 minutes)
// but at 2^32 / 240 us = 17.9 s, jumping from 17,895,697 back to 0: a difference
// taken across that jump is about 4.28 billion microseconds, as the owner's second
// report showed (every task's "max 4277071601us"). This class keeps its own count:
// each update adds the ticks since the previous one (modulo 2^32, so the counter's
// wrap is just a step like any other), whole microseconds go to the count, the rest
// is carried. The count then wraps at 2^32 us like any 32-bit clock, and unsigned
// differences between two readings are right as long as they are less than 71
// minutes apart. Requirement: update() at least once per counter period (17.9 s);
// the firmware calls it from the main loop and from every audio block.
//
// Not reentrant: the device calls it with interrupts masked (a few cycles). Portable
// (host/daisy/test_daisy_clock.cpp).
#pragma once
#include <cstdint>

namespace pgd {

class UsClock {
 public:
  void start(uint32_t raw, uint32_t ticksPerUs, uint32_t startUs = 0) {
    last_ = raw;
    perUs_ = ticksPerUs ? ticksPerUs : 1;
    rem_ = 0;
    us_ = startUs;
  }
  uint32_t update(uint32_t raw) {
    const uint32_t d = raw - last_;   // modulo 2^32: the counter's wrap is an ordinary step
    last_ = raw;
    us_ += d / perUs_;
    rem_ += d % perUs_;
    if (rem_ >= perUs_) {
      rem_ -= perUs_;
      ++us_;
    }
    return us_;
  }
  uint32_t now() const { return us_; }

 private:
  uint32_t last_ = 0, perUs_ = 1, rem_ = 0, us_ = 0;
};

}  // namespace pgd
