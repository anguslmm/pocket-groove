// The audio callback's load, from cycle counts (the Daisy: the Cortex-M7's DWT cycle
// counter, 480 MHz). The callback calls begin() first and end() last; the period is
// the block's duration in cycles. Kept:
//   now        moving average of the load (time constant ~64 blocks), percent
//   window     peak load, longest block (us), blocks and late blocks since takeWindow()
//   boot       longest block since boot, late blocks since boot
// A block is LATE when its own processing took longer than the block period (the
// DMA has then played part of the buffer before it was written: an underrun), or
// when it started more than 1.5 periods after the previous one (the interrupt was
// held off: another interrupt at the same priority, or interrupts masked).
// Writer: the audio interrupt. Reader: the main loop (32-bit fields, read whole; a
// report that is one block stale is fine). Portable (host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstdint>

namespace pgd {

class LoadMeter {
 public:
  void configure(uint32_t cpuHz, uint32_t periodCycles) {
    cpuHz_ = cpuHz ? cpuHz : 1;
    period_ = periodCycles ? periodCycles : 1;
  }
  uint32_t periodCycles() const { return period_; }

  // Audio interrupt.
  void begin(uint32_t cycles) {
    if (started_) {
      const uint32_t gap = cycles - lastBegin_;
      if (gap > period_ + period_ / 2) {
        ++lateGaps_;
        ++windowLate_;
      }
    }
    started_ = true;
    lastBegin_ = cycles;
  }
  void end(uint32_t cycles) {
    const uint32_t used = cycles - lastBegin_;
    // permille of the period, moving average (1/64)
    const uint32_t pm = static_cast<uint32_t>((static_cast<uint64_t>(used) * 1000u) / period_);
    avgPm16_ = blocks_ ? avgPm16_ + ((static_cast<int32_t>(pm << 4) - static_cast<int32_t>(avgPm16_)) >> 6)
                       : (pm << 4);
    if (pm > windowPeakPm_) windowPeakPm_ = pm;
    if (used > windowMaxCycles_) windowMaxCycles_ = used;
    if (used > bootMaxCycles_) bootMaxCycles_ = used;
    if (used > period_) {
      ++overruns_;
      ++windowLate_;
    }
    ++blocks_;
    ++windowBlocks_;
  }

  // Main loop.
  struct Window {
    uint32_t peakPermille, maxUs, blocks, late;
  };
  Window takeWindow() {
    Window w{windowPeakPm_, toUs(windowMaxCycles_), windowBlocks_, windowLate_};
    windowPeakPm_ = 0;
    windowMaxCycles_ = 0;
    windowBlocks_ = 0;
    windowLate_ = 0;
    return w;
  }
  uint32_t nowPermille() const { return avgPm16_ >> 4; }
  uint32_t blocks() const { return blocks_; }
  uint32_t overruns() const { return overruns_; }    // processing longer than the period
  uint32_t lateStarts() const { return lateGaps_; }  // started > 1.5 periods after the previous
  uint32_t bootMaxUs() const { return toUs(bootMaxCycles_); }
  uint32_t periodUs() const { return toUs(period_); }
  uint32_t toUs(uint32_t cycles) const {
    return static_cast<uint32_t>((static_cast<uint64_t>(cycles) * 1000000u) / cpuHz_);
  }

 private:
  uint32_t cpuHz_ = 1, period_ = 1;
  bool started_ = false;
  uint32_t lastBegin_ = 0;
  uint32_t avgPm16_ = 0;
  uint32_t blocks_ = 0, overruns_ = 0, lateGaps_ = 0, bootMaxCycles_ = 0;
  uint32_t windowPeakPm_ = 0, windowMaxCycles_ = 0, windowBlocks_ = 0, windowLate_ = 0;
};

}  // namespace pgd
