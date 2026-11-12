// SDL audio output: SDL's callback thread is the audio thread and the only
// caller of Instrument::process, exactly as the device's pg_audio task (the
// single-writer contract in docs/ARCHITECTURE.md). The callback never locks,
// allocates or does I/O; it copies what it rendered into a lock-free ring that
// the main thread drains (live recording) and publishes its load in atomics.
#pragma once
#include <atomic>
#include <cstdint>

#include "pg/config.h"

namespace pg { class Instrument; }

namespace desk {

// Single-producer (audio callback) / single-consumer (main thread) ring of
// interleaved stereo int16 frames. Every rendered frame goes in; the consumer
// writes them to a WAV while recording and skips them otherwise. Frame indices
// are absolute (frames rendered since the audio started), so a recording knows
// exactly which engine frame it starts at.
class FrameRing {
 public:
  static constexpr uint32_t kFrames = 1u << 20;   // ~24 s at 44.1 kHz: survives a stalled main thread

  void push(const int16_t* lr, uint32_t frames);           // producer
  // Consumer: up to `max` frames from the read position; returns the count.
  uint32_t peek(const int16_t** a, uint32_t* na, const int16_t** b, uint32_t* nb, uint32_t max) const;
  void consume(uint32_t frames) { r_.store(r_.load(std::memory_order_relaxed) + frames, std::memory_order_release); }
  uint64_t readPos() const { return r_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  int16_t buf_[kFrames * 2];
  std::atomic<uint64_t> w_{0}, r_{0}, dropped_{0};
};

struct AudioStats {
  float loadPct;        // render time / buffer period, smoothed
  float peakLoadPct;    // max since the previous takeStats()
  uint32_t late;        // callbacks whose render took longer than their period
  uint32_t gaps;        // callbacks that came > 2 periods after the previous one
  uint32_t callbacks;
  int bufferFrames;     // what the device gave us
  int sampleRate;
};

class AudioOut {
 public:
  // Opens the default output device: `rate` Hz, stereo, int16, `bufferFrames`
  // per callback (SDL converts if the hardware differs). Not started.
  bool open(pg::Instrument* inst, int rate, int bufferFrames, char* err, int errCap);
  void start();
  void close();
  bool isOpen() const { return dev_ != 0; }

  AudioStats stats();   // main thread
  FrameRing& ring() { return ring_; }
  uint64_t framesRendered() const { return rendered_.load(std::memory_order_relaxed); }

 private:
  static void callback(void* user, uint8_t* stream, int len);
  void render(int16_t* out, int frames);

  pg::Instrument* inst_ = nullptr;
  uint32_t dev_ = 0;
  int rate_ = 44100, buffer_ = 256;
  // The engine renders whole quanta (16 frames); SDL may ask for any count,
  // so up to 15 rendered frames wait here for the next callback.
  int16_t carry_[pg::kQuantum * 2] = {};
  int carryPos_ = pg::kQuantum, carryLen_ = pg::kQuantum;
  uint64_t lastCallTicks_ = 0;
  double tickHz_ = 1.0;
  FrameRing ring_;
  std::atomic<uint64_t> rendered_{0};
  std::atomic<float> load_{0.0f}, peak_{0.0f};
  std::atomic<uint32_t> late_{0}, gaps_{0}, calls_{0};
  std::atomic<int> obtainedFrames_{0};
};

}  // namespace desk
