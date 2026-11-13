// A simulated microphone for the host tests and ui_shot (stage F2): a
// pg::sampler::MicInput that runs the real Recorder on a synthetic room (Gaussian
// noise) plus, while `amp` is above 0, a sung vowel (a 220 Hz tone with harmonics),
// fed as much input as the caller's clock says has passed since the last poll. A
// finished recording lands in a MemoryPool slot, as the desktop's does (the device
// commits to flash instead). Erase state per slot, so the dirty-slot path can be
// driven too. Host code (heap, the standard library).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

#include "pg/sampler/recorder.h"
#include "pg/sampler/sample_pool.h"

namespace simmic {

struct VecSink final : pg::sampler::RecordSink {
  std::vector<int16_t> v;
  bool write(uint32_t offset, const int16_t* d, uint32_t n) override {
    if (offset != v.size()) return false;
    v.insert(v.end(), d, d + n);
    return true;
  }
};

class SimMic final : public pg::sampler::MicInput {
 public:
  pg::sampler::MemoryPool* pool = nullptr;
  std::function<uint32_t()> clockMs;
  double amp = 0.0;        // the sound's peak (0 = only the room)
  double noiseRms = 20.0;  // the room
  bool isReady = true;
  const char* notReady = "NO MIC";
  bool erasedFlag[pg::sampler::kPoolSlots];
  int erases = 0, starts = 0, releases = 0, commits = 0;

  SimMic() {
    for (bool& e : erasedFlag) e = true;
  }

  bool ready(const char** why) override {
    *why = notReady;
    return isReady;
  }
  bool slotErased(int slot) override { return erasedFlag[slot]; }
  bool erase(int slot) override {
    if (pool) pool->clear(slot);
    erasedFlag[slot] = true;
    ++erases;
    return true;
  }
  bool start(int slot) override {
    why_ = "";
    if (!erasedFlag[slot]) {
      why_ = "NOT ERASED";
      return false;
    }
    sink_.v.clear();
    rec_.start(&sink_);
    slot_ = slot;
    armed_ = true;
    last_ = clockMs ? clockMs() : 0;
    ++starts;
    return true;
  }
  void release() override {
    ++releases;
    if (armed_) rec_.release();
  }
  bool poll(pg::sampler::RecView* v) override {
    *v = pg::sampler::RecView{};
    v->why = why_;
    if (!armed_) return false;
    const uint32_t now = clockMs ? clockMs() : last_ + 33;
    feedFor(now - last_);
    last_ = now;
    if (rec_.released() && !rec_.ended()) rec_.finish();
    v->state = rec_.state();
    v->slot = slot_;
    v->samples = rec_.ended() ? rec_.length() : rec_.recorded();
    v->level = rec_.level();
    v->threshold = rec_.threshold();
    v->peak = rec_.peak();
    v->cols = rec_.cols();
    v->colsUsed = rec_.colsUsed();
    if (rec_.ended()) {
      armed_ = false;
      const pg::sampler::RecState s = rec_.state();
      if ((s == pg::sampler::RecState::Done || s == pg::sampler::RecState::Full) && pool) {
        // levelled as both platforms play it (the desktop's converter on load, the
        // device's header gain): the peak at -1 dBFS, at most +24 dB
        std::vector<int16_t>& b = bufs_[slot_];
        b.assign(pg::sampler::kSlotMaxSamples, 0);
        int pk = 1;
        for (uint32_t i = 0; i < rec_.length(); ++i) pk = std::max(pk, std::abs(int(sink_.v[i])));
        double g = double(pg::sampler::kRecordTargetPeak) / double(pk);
        if (g > double(pg::sampler::kRecordMaxGain)) g = double(pg::sampler::kRecordMaxGain);
        if (g < 1.0) g = 1.0;
        for (uint32_t i = 0; i < rec_.length(); ++i) {
          const long v = std::lround(double(sink_.v[i]) * g);
          b[i] = int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        char name[16];
        std::snprintf(name, sizeof name, "REC%02d", (slot_ + 1) % 100);
        pool->set(slot_, b.data(), rec_.length(), name);
        erasedFlag[slot_] = false;
        ++commits;
      }
    }
    return true;
  }
  bool armed() const { return armed_; }
  const pg::sampler::Recorder& recorder() const { return rec_; }

 private:
  void feedFor(uint32_t ms) {
    const size_t n = size_t(ms) * 441 / 10;
    std::normal_distribution<double> nd(0.0, noiseRms);
    int16_t buf[256];
    size_t done = 0;
    while (done < n && !rec_.ended()) {
      const int k = int(n - done < 256 ? n - done : 256);
      for (int i = 0; i < k; ++i) {
        double x = nd(rng_);
        if (amp > 0.0) {
          const double w = 2.0 * 3.14159265358979 * 220.0 * phase_;
          x += amp * (0.6 * std::sin(w) + 0.3 * std::sin(2.0 * w) + 0.1 * std::sin(3.0 * w));
        }
        phase_ += 1.0 / 44100.0;
        buf[i] = int16_t(x > 32767.0 ? 32767 : (x < -32768.0 ? -32768 : int(std::lround(x))));
      }
      rec_.feed(buf, k);
      done += size_t(k);
    }
  }

  pg::sampler::Recorder rec_;
  VecSink sink_;
  std::vector<int16_t> bufs_[pg::sampler::kPoolSlots];
  std::mt19937 rng_{3};
  double phase_ = 0.0;
  bool armed_ = false;
  int slot_ = 0;
  uint32_t last_ = 0;
  const char* why_ = "";
};

}  // namespace simmic
