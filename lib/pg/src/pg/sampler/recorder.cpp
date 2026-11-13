// Recording from a microphone (recorder.h).
#include "recorder.h"

#include <cmath>
#include <cstring>

namespace pg {
namespace sampler {

// The half-band filter: an equiripple design (scipy remez, the Vaidyanathan-Nguyen
// half-band trick) of length 47 with the pass band to 9,000 Hz at 44,100 Hz, its
// odd-distance taps then scaled so the DC gain is exactly 1. Every even-distance
// tap but the centre is zero. host/tests/test_sampler_recorder.cpp measures the
// response this table gives.
const float HalfBand::kCoef[HalfBand::kUnique] = {
    3.167311932e-01f,  -1.011723259e-01f, 5.570091427e-02f,  -3.490107252e-02f,
    2.270457658e-02f,  -1.475643595e-02f, 9.365807923e-03f,  -5.699791280e-03f,
    3.262688132e-03f,  -1.711680933e-03f, 7.878507027e-04f,  -3.117241602e-04f,
};

void HalfBand::reset() {
  std::memset(buf_, 0, sizeof buf_);
  pos_ = 0;
}

float HalfBand::run(float a, float b) {
  pos_ = (pos_ + 1) & (kBuf - 1);
  buf_[pos_] = buf_[pos_ + kBuf] = a;
  pos_ = (pos_ + 1) & (kBuf - 1);
  buf_[pos_] = buf_[pos_ + kBuf] = b;
  // the newest sample at p, the oldest of the 47 at p - 46, the centre at p - 23
  const float* c = buf_ + pos_ + kBuf - (kTaps - 1) / 2;
  float y = 0.5f * c[0];
  for (int k = 0; k < kUnique; ++k) {
    const int d = 2 * k + 1;
    y += kCoef[k] * (c[-d] + c[d]);
  }
  return y;
}

const char* recStateName(RecState s) {
  switch (s) {
    case RecState::Off: return "OFF";
    case RecState::Settling: return "SETTLING";
    case RecState::Waiting: return "WAITING";
    case RecState::Recording: return "RECORDING";
    case RecState::Done: return "DONE";
    case RecState::Full: return "FULL";
    case RecState::Cancelled: return "CANCELLED";
    case RecState::Failed: return "FAILED";
  }
  return "?";
}

void Recorder::start(RecordSink* sink, uint32_t maxSamples) {
  sink_ = sink;
  direct_ = false;
  skip_ = 0;
  max_ = maxSamples < kMinSamples ? kMinSamples : (maxSamples > kSlotMaxSamples ? kSlotMaxSamples : maxSamples);
  hb_.reset();
  dc_.reset();
  havePend_ = false;
  pend_ = 0.0f;
  n_ = 0;
  winSum_ = 0.0f;
  winN_ = 0;
  floorMin_ = -1.0f;
  floor_ = 0;
  ringN_ = 0;
  stageN_ = 0;
  flushed_ = 0;
  final_ = 0;
  writes_ = errors_ = 0;
  levelAcc_ = levelN_ = 0;
  std::memset(cols_, 0, sizeof cols_);
  colCur_ = -1;
  released_.store(false, std::memory_order_relaxed);
  recorded_.store(0, std::memory_order_relaxed);
  level_.store(0, std::memory_order_relaxed);
  thr_.store(0, std::memory_order_relaxed);
  peak_.store(0, std::memory_order_relaxed);
  colsUsed_.store(0, std::memory_order_relaxed);
  state_.store(uint8_t(RecState::Settling), std::memory_order_release);
}

void Recorder::startDirect(RecordSink* sink, uint32_t maxSamples, uint32_t skip) {
  start(sink, maxSamples);
  direct_ = true;
  skip_ = skip;
  state_.store(uint8_t(RecState::Recording), std::memory_order_release);
}

void Recorder::end(RecState s, uint32_t len) {
  if (state() == RecState::Failed) return;
  if (s == RecState::Done || s == RecState::Full) {
    if (!flush()) return;
    const uint32_t r = recorded_.load(std::memory_order_relaxed);
    final_ = len < r ? len : r;
    if (final_ < kMinSamples) s = RecState::Cancelled;
  }
  state_.store(uint8_t(s), std::memory_order_release);
}

bool Recorder::ended() const {
  const RecState s = state();
  return s == RecState::Done || s == RecState::Full || s == RecState::Cancelled || s == RecState::Failed;
}

void Recorder::feed(const int16_t* in, int n) {
  for (int i = 0; i < n; ++i) {
    const RecState s = state();
    if (s != RecState::Settling && s != RecState::Waiting && s != RecState::Recording) return;
    const float x = float(in[i]);
    if (!havePend_) {
      pend_ = x;
      havePend_ = true;
      continue;
    }
    havePend_ = false;
    const float h = hb_.run(pend_, x);
    out(direct_ ? h : dc_.run(h));
  }
}

bool Recorder::flush() {
  if (stageN_ == 0) return true;
  ++writes_;
  const bool ok = sink_ && sink_->write(flushed_, stage_, uint32_t(stageN_));
  flushed_ += uint32_t(stageN_);
  stageN_ = 0;
  if (!ok) {
    ++errors_;
    state_.store(uint8_t(RecState::Failed), std::memory_order_release);
  }
  return ok;
}

void Recorder::put(int16_t v) {
  const uint32_t r = recorded_.load(std::memory_order_relaxed);
  if (r >= max_) return;
  stage_[stageN_++] = v;
  const int a = v < 0 ? -int(v) : int(v);
  if (a > peak_.load(std::memory_order_relaxed)) peak_.store(uint16_t(a > 32767 ? 32767 : a), std::memory_order_relaxed);
  const int c = int((uint64_t(r) * uint32_t(kOverviewCols)) / max_);
  if (c != colCur_) {
    colCur_ = c;
    cols_[c][0] = cols_[c][1] = v;
    colsUsed_.store(c + 1, std::memory_order_release);
  } else {
    if (v < cols_[c][0]) cols_[c][0] = v;
    if (v > cols_[c][1]) cols_[c][1] = v;
  }
  recorded_.store(r + 1, std::memory_order_release);
  if (stageN_ == kStage && !flush()) return;
  if (r + 1 == max_) {
    if (!flush()) return;
    final_ = max_;
    state_.store(uint8_t(RecState::Full), std::memory_order_release);
  }
}

void Recorder::out(float y) {
  const uint32_t n = n_++;
  const float ay = y < 0.0f ? -y : y;
  const int a = ay > 32767.0f ? 32767 : int(ay);
  if (a > levelAcc_) levelAcc_ = a;
  if (++levelN_ >= kLevelEvery) {
    level_.store(uint16_t(levelAcc_), std::memory_order_relaxed);
    levelAcc_ = levelN_ = 0;
  }
  const float yr = y + (y < 0.0f ? -0.5f : 0.5f);
  const int16_t v = int16_t(yr > 32767.0f ? 32767 : (yr < -32768.0f ? -32768 : int(yr)));
  const RecState s = state();
  if (s == RecState::Recording) {
    if (skip_) {
      --skip_;
      return;
    }
    put(v);
    return;
  }
  ring_[ringN_ & (kRing - 1)] = v;
  ++ringN_;
  if (s == RecState::Settling) {
    if (n >= kSkip) {
      winSum_ += y * y;
      if (++winN_ == kWindow) {
        const float ms = winSum_ / float(kWindow);
        if (floorMin_ < 0.0f || ms < floorMin_) floorMin_ = ms;
        winSum_ = 0.0f;
        winN_ = 0;
      }
    }
    if (n + 1 >= kSettle) {
      const float rms = floorMin_ > 0.0f ? std::sqrt(floorMin_) : 0.0f;
      floor_ = uint16_t(rms > 32767.0f ? 32767.0f : rms);
      float t = rms * kThrOverFloor;
      if (t < float(kThrMin)) t = float(kThrMin);
      if (t > float(kThrMax)) t = float(kThrMax);
      thr_.store(uint16_t(t), std::memory_order_relaxed);
      state_.store(uint8_t(RecState::Waiting), std::memory_order_release);
    }
    return;
  }
  // Waiting
  if (a > int(thr_.load(std::memory_order_relaxed))) {
    state_.store(uint8_t(RecState::Recording), std::memory_order_release);
    const uint32_t pre = ringN_ < kPreRoll ? ringN_ : kPreRoll;   // includes this sample
    for (uint32_t k = ringN_ - pre; k != ringN_; ++k) {
      put(ring_[k & (kRing - 1)]);
      if (state() != RecState::Recording) return;
    }
  }
}

void Recorder::finish() {
  const RecState s = state();
  if (s == RecState::Recording) {
    if (!flush()) return;
    const uint32_t r = recorded_.load(std::memory_order_relaxed);
    const uint32_t len = r > kTail ? r - kTail : 0;
    if (len < kMinSamples) {
      state_.store(uint8_t(RecState::Cancelled), std::memory_order_release);
      return;
    }
    final_ = len;
    state_.store(uint8_t(RecState::Done), std::memory_order_release);
  } else if (s == RecState::Settling || s == RecState::Waiting) {
    state_.store(uint8_t(RecState::Cancelled), std::memory_order_release);
  }
}

}  // namespace sampler
}  // namespace pg
