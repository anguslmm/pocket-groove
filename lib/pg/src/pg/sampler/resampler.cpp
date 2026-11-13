// Resampling: recording the engine's output into a slot (resampler.h).
#include "resampler.h"

#include <cstring>

namespace pg {
namespace sampler {

void Resampler::start(Recorder* rec, RecordSink* sink, int16_t* ring, uint32_t ringSize, bool sync,
                      uint32_t maxSamples) {
  rec_ = rec;
  sink_ = sink;
  ring_ = ring;
  mask_ = ringSize - 1;
  maxS_ = maxSamples < kMinSamples ? kMinSamples : (maxSamples > kSlotMaxSamples ? kSlotMaxSamples : maxSamples);
  maxF_ = 2 * maxS_;
  std::memset(hist_, 0, sizeof hist_);
  n_ = s_ = e_ = lastFit_ = head_ = 0;
  started_ = endSet_ = done_ = false;
  why_ = kNotYet;
  length_ = 0;
  bars_.store(0, std::memory_order_relaxed);
  headPub_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  released_.store(false, std::memory_order_relaxed);
  sync_.store(sync, std::memory_order_relaxed);
  startPub_.store(false, std::memory_order_relaxed);
  tapDone_.store(false, std::memory_order_relaxed);
  ended_.store(false, std::memory_order_relaxed);
  endPub_.store(kNotYet, std::memory_order_relaxed);
  endLen_.store(0, std::memory_order_relaxed);
  rec_->startDirect(sink, maxS_, kSkip);
  std::atomic_thread_fence(std::memory_order_release);
}

void Resampler::push(int16_t v) {
  if (head_ - tail_.load(std::memory_order_acquire) > mask_) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    setEnd(0, kOverrun);   // the timing is lost: over, whatever it was doing
    return;
  }
  ring_[head_ & mask_] = v;
  ++head_;
}

void Resampler::setEnd(uint32_t d, End why) {
  endSet_ = true;
  e_ = d;
  why_ = why;
}

// The audio thread, after every quantum. Not recording (before start, after the
// end) it does nothing but the mono sum into the history and a few compares.
void Resampler::quantum(const int16_t* st, int frames, uint8_t flags) {
  if (done_) return;
  if (frames > kMaxFrames) frames = kMaxFrames;
  const bool playing = (flags & kTapPlaying) != 0, bar = (flags & kTapBar) != 0;
  const bool rel = released_.load(std::memory_order_acquire);
  int16_t m[kMaxFrames];
  for (int i = 0; i < frames; ++i) m[i] = int16_t((int32_t(st[2 * i]) + int32_t(st[2 * i + 1])) >> 1);
  if (!started_) {
    for (int i = 0; i < frames; ++i) hist_[(n_ + uint32_t(i)) & (kHist - 1)] = m[i];
    bool sync = sync_.load(std::memory_order_relaxed);
    int at = -1;
    if (rel) {   // let go before it started: nothing
      why_ = kCancelled;
      done_ = true;
      endPub_.store(why_, std::memory_order_relaxed);
      tapDone_.store(true, std::memory_order_release);
      return;
    }
    if (sync && !playing) {   // the transport stopped before the bar line: from the first sound
      sync = false;
      sync_.store(false, std::memory_order_relaxed);
    }
    if (sync) {
      if (bar) at = 0;
    } else {
      for (int i = 0; i < frames; ++i)
        if (m[i] >= kSilence || m[i] <= -kSilence) {
          at = i;
          break;
        }
    }
    if (at < 0) {
      n_ += uint32_t(frames);
      return;
    }
    started_ = true;
    s_ = n_ + uint32_t(at);
    // the half-band's history in front, then this quantum from the start on
    for (uint32_t f = s_ - kPre; f != n_ + uint32_t(frames); ++f) push(hist_[f & (kHist - 1)]);
    n_ += uint32_t(frames);
    startPub_.store(true, std::memory_order_release);
  } else {
    if (!endSet_) {
      const uint32_t d = n_ - s_;   // frames recorded before this quantum
      if (sync_.load(std::memory_order_relaxed)) {
        if (bar && d <= maxF_) {
          lastFit_ = d;
          const int b = bars_.load(std::memory_order_relaxed);
          if (b < kMaxBars) {
            barAt_[b] = d / 2;
            bars_.store(b + 1, std::memory_order_release);
          }
          if (rel) setEnd(d, kReleased);
        }
        if (!endSet_ && !playing) setEnd(lastFit_ > 0 ? lastFit_ : d, kStopped);
      } else if (rel) {
        setEnd(d + (d & 1), kReleased);   // even: whole decimated samples
      }
    }
    // the quantum into the ring: one check for room, then a plain copy
    if (head_ + uint32_t(frames) - tail_.load(std::memory_order_acquire) > mask_ + 1) {
      dropped_.fetch_add(uint32_t(frames), std::memory_order_relaxed);
      setEnd(0, kOverrun);   // the timing is lost: over, whatever it was doing
    } else {
      for (int i = 0; i < frames; ++i) ring_[(head_ + uint32_t(i)) & mask_] = m[i];
      head_ += uint32_t(frames);
    }
    n_ += uint32_t(frames);
  }
  if (!endSet_ && n_ - s_ >= maxF_) {
    if (!sync_.load(std::memory_order_relaxed)) setEnd(maxF_, kFull);
    else if (lastFit_ > 0) setEnd(lastFit_, kFull);
    else setEnd(maxF_, kNoBarFits);
  }
  headPub_.store(head_, std::memory_order_release);
  if (endSet_ && (why_ == kOverrun || n_ - s_ >= e_ + kPost)) {
    done_ = true;
    endLen_.store((e_ + 1) / 2, std::memory_order_relaxed);
    endPub_.store(why_, std::memory_order_relaxed);
    tapDone_.store(true, std::memory_order_release);
  }
}

void Resampler::pump() {
  if (ended()) return;
  // what the audio side said last, before reading the ring: if it is done, the ring
  // already holds everything it will ever write
  const bool tapDone = tapDone_.load(std::memory_order_acquire);
  for (;;) {
    const uint32_t head = headPub_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (head == tail) break;
    const uint32_t at = tail & mask_;
    uint32_t n = head - tail;
    if (n > mask_ + 1 - at) n = mask_ + 1 - at;   // to the ring's end; the rest next time round
    if (n > 512) n = 512;
    if (rec_->state() == RecState::Recording) rec_->feed(ring_ + at, int(n));
    tail_.store(tail + n, std::memory_order_release);
  }
  if (rec_->state() == RecState::Failed) {   // the sink refused a write: over now (the caller removes the tap)
    length_ = 0;
    ended_.store(true, std::memory_order_release);
    return;
  }
  if (!tapDone) return;
  const End why = End(endPub_.load(std::memory_order_relaxed));
  RecState s = RecState::Done;
  if (why == kCancelled) s = RecState::Cancelled;
  else if (why == kOverrun) s = RecState::Failed;
  else if (why == kFull || why == kNoBarFits) s = RecState::Full;
  if (s != RecState::Failed && s != RecState::Cancelled && rec_->peak() == 0) s = RecState::Cancelled;   // silence
  rec_->end(s, endLen_.load(std::memory_order_relaxed));
  length_ = rec_->length();
  ended_.store(true, std::memory_order_release);
}

RecState Resampler::state() const {
  if (ended()) return rec_->state();
  return started() ? RecState::Recording : RecState::Waiting;
}

}  // namespace sampler
}  // namespace pg
