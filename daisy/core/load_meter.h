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
//
// THE WARM-UP. The first kWarmupBlocks blocks after start() run cold: the caches
// are empty, and the first one also does the session's one-time work in the
// engine (the boot-time project load is sanitized in the audio thread, the FX
// buffers are cleared, every instrument is evaluated, the transport starts). The
// first Seed3 measured 3,424 us for that block (171 % of the period) against 814 us
// at most in 12 minutes after it. As on the Cardputer (src/platform/audio_out.cpp
// kWarmupBlocks), the warm-up is left out of the average, the peaks and the longest
// block; its own longest block is kept apart (warmupMaxUs) and the late counters
// say how many of theirs fell in it.
//
// EVENTS. Every underrun and every late start gets a record (uptime, block number,
// duration, gap since the previous block, and the context the caller passes in:
// what the main loop was in, flags such as a QSPI operation or the transport
// starting). The first kKeepFirst are kept for good, then the latest kKeepLast in a
// ring; the longest block after the warm-up keeps its record too. So one report
// says when, and probably why.
//
// Writer: the audio interrupt. Reader: the main loop (32-bit fields read whole; a
// record is copied while the interrupt may write the next one, which a report
// tolerates: the sequence number shows it). Portable (host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstdint>

namespace pgd {

class LoadMeter {
 public:
  static constexpr uint32_t kWarmupBlocks = 32;   // 64 ms at 96 frames / 48 kHz
  static constexpr int kKeepFirst = 4, kKeepLast = 4;

  // What was going on (filled by the caller, cheap reads only).
  enum Flag : uint16_t {
    kFlagWarmup = 1u << 0,        // the block was in the warm-up
    kFlagFirstBlock = 1u << 1,    // the very first block after start()
    kFlagPlayStart = 1u << 2,     // the transport started in this block (or the one before)
    kFlagRefresh = 1u << 3,       // the engine re-evaluated instruments (RefreshInstruments)
    kFlagPlaying = 1u << 4,       // the transport was playing
    kFlagQspi = 1u << 5,          // a QSPI erase or write was under way in the main loop
    kFlagUsbDown = 1u << 6,       // USB not configured (enumerating, or no host)
    kFlagMacroMove = 1u << 7,     // a pot moved a macro within the last 20 ms
    kFlagKey = 1u << 8,           // a key event within the last 20 ms
    kFlagSerialRx = 1u << 9,      // USB bytes arrived since the previous block
  };
  struct Context {
    int8_t task = -1;       // the main loop's task running when the block began (-1: none, between tasks)
    uint16_t flags = 0;
  };
  enum Kind : uint8_t { kUnderrun = 1, kLateStart = 2 };
  struct Event {
    uint32_t seq;           // 1, 2, ...: its number among all events
    uint32_t ms;            // uptime when the block ended
    uint32_t block;         // its number since start() (0 = the first)
    uint32_t us;            // how long it took
    uint32_t gapUs;         // since the previous block's start
    uint8_t kind;           // Kind bits
    Context ctx;
  };

  void configure(uint32_t cpuHz, uint32_t periodCycles) {
    cpuHz_ = cpuHz ? cpuHz : 1;
    period_ = periodCycles ? periodCycles : 1;
  }
  uint32_t periodCycles() const { return period_; }
  // A new start (boot, or after the audio restarted): everything from zero.
  void start() {
    const uint32_t hz = cpuHz_, p = period_;
    *this = LoadMeter{};
    configure(hz, p);
  }

  // Audio interrupt.
  void begin(uint32_t cycles) {
    gap_ = 0;
    if (started_) {
      gap_ = cycles - lastBegin_;
      if (gap_ > period_ + period_ / 2) {
        ++lateGaps_;
        ++windowLate_;
        if (blocks_ < kWarmupBlocks) ++warmLate_;
        pendKind_ |= kLateStart;
      }
    }
    started_ = true;
    lastBegin_ = cycles;
  }
  bool warm() const { return blocks_ >= kWarmupBlocks; }
  uint32_t blocks() const { return blocks_; }   // == the number of the block now running
  // ctx: what was going on; nowMs: the uptime (for the record).
  void end(uint32_t cycles) { end(cycles, Context(), 0); }
  void end(uint32_t cycles, const Context& ctx, uint32_t nowMs) {
    const uint32_t used = cycles - lastBegin_;
    const bool w = warm();
    if (used > period_) {
      ++overruns_;
      ++windowLate_;
      if (!w) ++warmOverruns_;
      pendKind_ |= kUnderrun;
    }
    if (w) {
      // permille of the period, moving average (1/64), seeded by the first warm block
      const uint32_t pm = static_cast<uint32_t>((static_cast<uint64_t>(used) * 1000u) / period_);
      avgPm16_ = avgSeeded_ ? avgPm16_ + ((static_cast<int32_t>(pm << 4) - static_cast<int32_t>(avgPm16_)) >> 6)
                            : (pm << 4);
      avgSeeded_ = true;
      if (pm > windowPeakPm_) windowPeakPm_ = pm;
      if (used > windowMaxCycles_) windowMaxCycles_ = used;
      if (used > bootMaxCycles_) {
        bootMaxCycles_ = used;
        longest_ = make(0, used, ctx, nowMs);
      }
    } else if (used > warmMaxCycles_) {
      warmMaxCycles_ = used;
    }
    if (pendKind_) {
      Event e = make(pendKind_, used, ctx, nowMs);
      if (!w) e.ctx.flags |= kFlagWarmup;
      e.seq = ++events_;
      if (events_ <= static_cast<uint32_t>(kKeepFirst)) first_[events_ - 1] = e;
      else last_[(events_ - kKeepFirst - 1) % kKeepLast] = e;
      pendKind_ = 0;
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
  uint32_t overruns() const { return overruns_; }    // processing longer than the period
  uint32_t lateStarts() const { return lateGaps_; }  // started > 1.5 periods after the previous
  uint32_t warmupOverruns() const { return warmOverruns_; }
  uint32_t warmupLateStarts() const { return warmLate_; }
  uint32_t bootMaxUs() const { return toUs(bootMaxCycles_); }   // after the warm-up
  uint32_t warmupMaxUs() const { return toUs(warmMaxCycles_); }
  uint32_t periodUs() const { return toUs(period_); }
  uint32_t toUs(uint32_t cycles) const {
    return static_cast<uint32_t>((static_cast<uint64_t>(cycles) * 1000000u) / cpuHz_);
  }
  uint32_t eventCount() const { return events_; }
  // The kept records, oldest first: up to kKeepFirst + kKeepLast. Returns how many.
  int events(Event* out, int cap) const {
    int n = 0;
    const uint32_t total = events_;
    for (uint32_t i = 0; i < total && i < static_cast<uint32_t>(kKeepFirst) && n < cap; ++i) out[n++] = first_[i];
    if (total > static_cast<uint32_t>(kKeepFirst)) {
      const uint32_t later = total - kKeepFirst;
      const uint32_t keep = later < static_cast<uint32_t>(kKeepLast) ? later : static_cast<uint32_t>(kKeepLast);
      for (uint32_t i = later - keep; i < later && n < cap; ++i) out[n++] = last_[i % kKeepLast];
    }
    return n;
  }
  // The longest block after the warm-up, with its context (us 0: none yet).
  Event longest() const { return longest_; }

 private:
  Event make(uint8_t kind, uint32_t used, const Context& ctx, uint32_t nowMs) const {
    Event e{};
    e.ms = nowMs;
    e.block = blocks_;
    e.us = toUs(used);
    e.gapUs = toUs(gap_);
    e.kind = kind;
    e.ctx = ctx;
    return e;
  }

  uint32_t cpuHz_ = 1, period_ = 1;
  bool started_ = false, avgSeeded_ = false;
  uint32_t lastBegin_ = 0, gap_ = 0;
  uint32_t avgPm16_ = 0;
  uint32_t blocks_ = 0, overruns_ = 0, lateGaps_ = 0, bootMaxCycles_ = 0;
  uint32_t warmMaxCycles_ = 0, warmOverruns_ = 0, warmLate_ = 0;
  uint32_t windowPeakPm_ = 0, windowMaxCycles_ = 0, windowBlocks_ = 0, windowLate_ = 0;
  uint8_t pendKind_ = 0;
  uint32_t events_ = 0;
  Event first_[kKeepFirst] = {};
  Event last_[kKeepLast] = {};
  Event longest_ = {};
};

}  // namespace pgd
