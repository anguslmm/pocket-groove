#include "audio.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>

#include "pg/instrument/instrument.h"

namespace desk {

// ---- FrameRing --------------------------------------------------------------------

void FrameRing::push(const int16_t* lr, uint32_t frames) {
  const uint64_t w = w_.load(std::memory_order_relaxed);
  const uint64_t r = r_.load(std::memory_order_acquire);
  if (w + frames - r > kFrames) {   // the consumer stalled: drop this block, never overwrite
    dropped_.fetch_add(frames, std::memory_order_relaxed);
    return;
  }
  const uint32_t at = uint32_t(w % kFrames);
  const uint32_t first = frames < kFrames - at ? frames : kFrames - at;
  std::memcpy(buf_ + size_t(at) * 2, lr, size_t(first) * 4);
  if (first < frames) std::memcpy(buf_, lr + size_t(first) * 2, size_t(frames - first) * 4);
  w_.store(w + frames, std::memory_order_release);
}

uint32_t FrameRing::peek(const int16_t** a, uint32_t* na, const int16_t** b, uint32_t* nb, uint32_t max) const {
  const uint64_t r = r_.load(std::memory_order_relaxed);
  const uint64_t w = w_.load(std::memory_order_acquire);
  uint64_t n = w - r;
  if (n > max) n = max;
  const uint32_t at = uint32_t(r % kFrames);
  const uint32_t first = uint32_t(n < kFrames - at ? n : kFrames - at);
  *a = buf_ + size_t(at) * 2;
  *na = first;
  *b = buf_;
  *nb = uint32_t(n) - first;
  return uint32_t(n);
}

// ---- AudioOut ---------------------------------------------------------------------

bool AudioOut::open(pg::Instrument* inst, int rate, int bufferFrames, char* err, int errCap) {
  inst_ = inst;
  rate_ = rate;
  buffer_ = bufferFrames;
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = rate;
  want.format = AUDIO_S16SYS;   // the engine renders interleaved int16
  want.channels = 2;
  want.samples = Uint16(bufferFrames);
  want.callback = &AudioOut::callback;
  want.userdata = this;
  // allowed_changes = 0: SDL converts if the hardware runs at another rate or
  // format, so the engine always runs at exactly `rate` (44.1k or 32k, the two
  // the device uses), whatever the Mac's output is set to.
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev_) {
    std::snprintf(err, size_t(errCap), "%s", SDL_GetError());
    return false;
  }
  if (have.format != AUDIO_S16SYS || have.channels != 2) {   // allowed_changes = 0 promises this; check anyway
    std::snprintf(err, size_t(errCap), "the output opened as format 0x%x, %d channels (not s16 stereo)",
                  unsigned(have.format), int(have.channels));
    SDL_CloseAudioDevice(dev_);
    dev_ = 0;
    return false;
  }
  obtainedFrames_.store(have.samples);
  desired_ = CaptureSpec{uint16_t(want.format), int(want.channels), want.freq, int(want.samples)};
  obtained_ = CaptureSpec{uint16_t(have.format), int(have.channels), have.freq, int(have.samples)};
  tickHz_ = double(SDL_GetPerformanceFrequency());
  return true;
}

void AudioOut::start() {
  if (dev_) SDL_PauseAudioDevice(dev_, 0);
}

void AudioOut::close() {
  if (dev_) SDL_CloseAudioDevice(dev_);   // waits for a running callback to return
  dev_ = 0;
}

AudioStats AudioOut::stats() {
  AudioStats s;
  s.loadPct = load_.load(std::memory_order_relaxed);
  s.peakLoadPct = peak_.exchange(0.0f, std::memory_order_relaxed);
  s.late = late_.load(std::memory_order_relaxed);
  s.gaps = gaps_.load(std::memory_order_relaxed);
  s.callbacks = calls_.load(std::memory_order_relaxed);
  s.bufferFrames = obtainedFrames_.load(std::memory_order_relaxed);
  s.sampleRate = rate_;
  return s;
}

void AudioOut::render(int16_t* out, int frames) {
  inst_->process(out, frames);
  if (listening_.load(std::memory_order_acquire)) listen_.load(std::memory_order_acquire)->mixInto(out, frames, rate_);
  ring_.push(out, uint32_t(frames));
  rendered_.store(rendered_.load(std::memory_order_relaxed) + uint64_t(frames), std::memory_order_relaxed);
}

void AudioOut::callback(void* user, uint8_t* stream, int len) {
  AudioOut& a = *static_cast<AudioOut*>(user);
  const uint64_t t0 = SDL_GetPerformanceCounter();
  int16_t* out = reinterpret_cast<int16_t*>(stream);
  int frames = len / 4;
  const int total = frames;
  a.cbStats_.onCallback(total);
  // 1. what is left of the last quantum
  while (frames > 0 && a.carryPos_ < a.carryLen_) {
    out[0] = a.carry_[a.carryPos_ * 2];
    out[1] = a.carry_[a.carryPos_ * 2 + 1];
    out += 2;
    ++a.carryPos_;
    --frames;
  }
  // 2. whole quanta straight into SDL's buffer
  const int whole = frames - frames % pg::kQuantum;
  if (whole > 0) {
    a.render(out, whole);
    out += whole * 2;
    frames -= whole;
  }
  // 3. one more quantum for the remainder, the rest carried over
  if (frames > 0) {
    a.render(a.carry_, pg::kQuantum);
    std::memcpy(out, a.carry_, size_t(frames) * 4);
    a.carryPos_ = frames;
    a.carryLen_ = pg::kQuantum;
  }

  const uint64_t t1 = SDL_GetPerformanceCounter();
  const double periodS = double(total) / double(a.rate_);
  const float load = float(double(t1 - t0) / a.tickHz_ / periodS * 100.0);
  const float prev = a.load_.load(std::memory_order_relaxed);
  a.load_.store(prev + (load - prev) * 0.05f, std::memory_order_relaxed);
  if (load > a.peak_.load(std::memory_order_relaxed)) a.peak_.store(load, std::memory_order_relaxed);
  if (load > 100.0f) a.late_.fetch_add(1, std::memory_order_relaxed);
  if (a.lastCallTicks_ && double(t0 - a.lastCallTicks_) / a.tickHz_ > 2.0 * periodS)
    a.gaps_.fetch_add(1, std::memory_order_relaxed);
  a.lastCallTicks_ = t0;
  a.calls_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace desk
