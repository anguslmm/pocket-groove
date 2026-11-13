// Resampling: recording what the engine plays into a sample pool slot (docs/UX.md
// 10.3 "Resampling", docs/STATE.md). The portable part, shared by the device
// (src/platform/mic.*: a writer task, the flash) and the desktop and the tests
// (host/common/host_resample.h: RAM).
//
// THE SOURCE: the engine's final output (engine.h OUTPUT TAP: after the FX, the
// ducker, the master bus and its DJ filter; before the codec's analog volume and
// USB), summed to mono ((L + R) >> 1; the device's mono build has L == R). To
// resample one part, solo it (FN + its key) or mute the rest. No ADC is involved.
//
// THE TWO HALVES:
//   quantum()  the audio thread (Engine calls it after every quantum while it is
//              installed): one mono sum per quantum into a 64-frame history, and
//              from the start on a copy into the caller's ring. It decides where
//              the recording starts and ends, to the frame.
//   pump()     the writer thread: the ring into a Recorder in DIRECT mode
//              (recorder.h: the same half-band to 22,050 Hz, no DC block, no
//              threshold, no trim) and its sink; at the end it ends the Recorder
//              with the length the audio side decided.
//
// WHERE IT STARTS AND ENDS:
//   In time (the transport plays at start()): it starts on the next BAR LINE
//   (engine.h: the playing pattern's step 1, 17, 33 or 49) at the first frame of the
//   quantum whose notes it starts, and ends on a bar line too: the first one after
//   release(), or, when the slot fills up first, the last one that still fit (the
//   sample is whole bars). If not even one bar fits (impossible at 60..200 BPM with
//   16-step bars: one is at most 4 s of the slot's 6.2), the slot is filled from the
//   bar line on (endReason kNoBarFits). If the transport stops before the bar line,
//   it waits for the first sound instead (below); if it stops while recording, the
//   sample ends on the last bar line passed (none yet: where it stopped).
//   Stopped: it starts on the first sample at or above kSilence (-66 dBFS: the player
//   plays by hand; a reverb tail still ringing counts as sound) and ends at release()
//   or when the slot is full. Nothing is trimmed (a digital source has no key click).
//   A release before the start records nothing (Cancelled); a sample that is all
//   zeros is Cancelled too.
//
// SAMPLE-EXACT DECIMATION: the half-band (47 taps, 23 frames of history either side)
// is fed from kPre frames before the start to kPost after the end, and the Recorder
// drops the first kSkip outputs, so output sample k IS the half-band's value
// centred on input frame start + 2k: a bar of N frames (N even) is N / 2 samples,
// the first on the bar line's frame (host/tests/test_sampler_resample.cpp).
//
// THREADS: start() before the tap is installed, release() from any thread, quantum()
// on the audio thread only, pump() on one writer thread; the view's getters from any
// thread. No heap, no locks; the caller owns the ring (a power of two, 2048 frames
// is 46 ms) and the Recorder (about 4.2 KB), the device both from the heap while it
// records.
#pragma once
#include <atomic>
#include <cstdint>

#include "../engine.h"
#include "recorder.h"

namespace pg {
namespace sampler {

class Resampler final : public OutputTap {
 public:
  static constexpr uint32_t kPre = 24;     // input frames before the start the half-band needs (23, kept even)
  static constexpr uint32_t kPost = 22;    // after the end
  static constexpr uint32_t kSkip = 23;    // outputs the Recorder drops: (kPre + 22) / 2
  static constexpr int kHist = 64;         // the audio side's history (>= kPre + one quantum)
  static constexpr int kMaxBars = 24;      // bar lines remembered for the picture
  static constexpr int16_t kSilence = 16;  // stopped: |x| >= this is sound (-66 dBFS)
  static constexpr int kMaxFrames = 64;    // the most frames quantum() takes at once (the engine sends kQuantum)

  enum End : uint8_t {
    kNotYet = 0,
    kReleased,    // on release(): the bar line after it (in time) or the release itself
    kFull,        // the slot filled up: the last bar line that fit (in time), or full
    kNoBarFits,   // in time, but the first bar did not fit: the slot is full from the bar line on
    kStopped,     // the transport stopped while recording in time
    kCancelled,   // released before it started
    kOverrun,     // the writer fell behind: the ring was full (the timing would be wrong: Failed)
  };

  // Writer side, before the tap is installed: everything is reset. ring: power of two.
  // sync = the transport is playing now (in time); maxSamples at 22,050 Hz.
  void start(Recorder* rec, RecordSink* sink, int16_t* ring, uint32_t ringSize, bool sync,
             uint32_t maxSamples = kSlotMaxSamples);
  // The key went up (any thread).
  void release() { released_.store(true, std::memory_order_release); }

  // OutputTap: the audio thread.
  void quantum(const int16_t* stereo, int frames, uint8_t flags) override;

  // The writer thread: drains the ring into the Recorder; once the audio side is done
  // and everything is in, ends the Recorder. Call it until ended().
  void pump();
  bool ended() const { return ended_.load(std::memory_order_acquire); }
  // The audio side writes nothing more (the tap may be removed).
  bool tapDone() const { return tapDone_.load(std::memory_order_acquire); }

  // ---- the view (any thread) ----
  // Waiting before the start (counting to a bar line, or waiting for a sound), then
  // Recording, then the Recorder's end state.
  RecState state() const;
  bool sync() const { return sync_.load(std::memory_order_relaxed); }   // in time (may turn false: Stop)
  bool started() const { return startPub_.load(std::memory_order_acquire); }
  End endReason() const { return End(endPub_.load(std::memory_order_acquire)); }
  uint32_t length() const { return length_; }   // samples, after ended()
  // Bar lines inside the recording so far, in samples from its start (in time).
  int bars() const { return bars_.load(std::memory_order_acquire); }
  const uint32_t* barAt() const { return barAt_; }
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  // The start and end in frames counted from start() (the tests).
  uint32_t startFrame() const { return s_; }
  uint32_t endFrame() const { return e_; }

 private:
  void push(int16_t v);
  void setEnd(uint32_t d, End why);

  Recorder* rec_ = nullptr;
  RecordSink* sink_ = nullptr;
  int16_t* ring_ = nullptr;
  uint32_t mask_ = 0;
  uint32_t maxF_ = 0;          // the slot in input frames
  uint32_t maxS_ = 0;
  // audio thread
  int16_t hist_[kHist];
  uint32_t n_ = 0;             // frames seen since start()
  uint32_t s_ = 0, e_ = 0;     // start frame; end, in frames from the start
  uint32_t lastFit_ = 0;       // the last bar line that fit, frames from the start
  uint32_t head_ = 0;          // frames written to the ring (published in headPub_)
  bool started_ = false, endSet_ = false, done_ = false;
  uint8_t why_ = kNotYet;
  uint32_t barAt_[kMaxBars];
  std::atomic<int> bars_{0};
  // shared
  std::atomic<uint32_t> headPub_{0}, tail_{0}, dropped_{0};
  std::atomic<bool> released_{false}, sync_{false}, startPub_{false}, tapDone_{false}, ended_{false};
  std::atomic<uint8_t> endPub_{kNotYet};
  std::atomic<uint32_t> endLen_{0};   // the sample's length in samples, published with tapDone_
  uint32_t length_ = 0;
};

}  // namespace sampler
}  // namespace pg
