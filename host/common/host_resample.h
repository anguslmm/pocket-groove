// Resampling on the host (docs/UX.md 10.3 "Resampling"): a pg::sampler::MicInput
// for SHIFT+L whose source is the engine's own output (pg/sampler/resampler.h), for
// the desktop app (host/app: it commits to samples/NN-resNN.wav), the tests and
// ui_shot (they commit to a MemoryPool, levelled as both platforms play it). The
// audio thread is whoever calls Engine::process (SDL's callback; the tests' and
// ui_shot's clock); poll() runs the writer side on the UI thread, every frame.
// The device's twin is src/platform/mic.cpp (a writer task, the flash).
// Host code (heap, the standard library).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "pg/engine.h"
#include "pg/sampler/resampler.h"
#include "pg/sampler/sample_pool.h"
#include "pg/sampler/sampler.h"

namespace hostrs {

struct VecSink final : pg::sampler::RecordSink {
  std::vector<int16_t> v;
  bool write(uint32_t offset, const int16_t* d, uint32_t n) override {
    if (offset != v.size()) return false;
    v.insert(v.end(), d, d + n);
    return true;
  }
};

class HostResample final : public pg::sampler::MicInput {
 public:
  static constexpr uint32_t kRing = 1u << 17;   // 3 s at 44.1 kHz: the UI thread may stall
  // Keeps the recording: (slot, raw samples at 22,050 Hz, count, error) -> ok. The
  // samples are as recorded (not levelled: the commit levels, as the platforms do).
  using Commit = std::function<bool(int slot, const int16_t* data, uint32_t n, std::string* err)>;

  void init(pg::Engine* eng, pg::sampler::SamplerVoice* voice, float sampleRate, Commit commit,
            std::function<bool(int)> erase = nullptr) {
    eng_ = eng;
    voice_ = voice;
    rate_ = sampleRate;
    commit_ = std::move(commit);
    erase_ = std::move(erase);
  }
  // The slot's most a recording may take (the tests: the does-not-fit case).
  uint32_t maxSamples = pg::sampler::kSlotMaxSamples;
  bool log = false;   // the desktop prints a line per recording

  bool ready(const char** why) override {
    *why = "";
    if (!eng_ || !commit_) {
      *why = "NO ENGINE";
      return false;
    }
    if (std::lround(rate_) != 44100) {   // the recorder halves 44.1 kHz into the pool's 22,050
      *why = "44.1K ONLY";
      return false;
    }
    return true;
  }
  bool slotErased(int) override { return true; }   // RAM / files: nothing to erase first
  bool erase(int slot) override { return erase_ ? erase_(slot) : true; }
  bool start(int slot) override {
    why_ = "";
    const char* why = "";
    if (armed_ || !ready(&why)) {
      why_ = armed_ ? "BUSY" : why;
      return false;
    }
    sink_.v.clear();
    sink_.v.reserve(pg::sampler::kSlotMaxSamples + 1024);
    rs_.start(&rec_, &sink_, ring_.data(), kRing, eng_->isPlaying(), maxSamples);
    slot_ = slot;
    voice_->setSilentSlot(slot);   // the slot plays nothing while it records
    eng_->setOutputTap(&rs_);
    armed_ = true;
    ++starts;
    return true;
  }
  void release() override {
    ++releases;
    if (armed_) rs_.release();
  }
  bool poll(pg::sampler::RecView* v) override {
    *v = pg::sampler::RecView{};
    v->why = why_;
    if (!armed_) return false;
    rs_.pump();
    fill(v);
    if (!rs_.ended()) return true;
    // over: the tap off, keep it, the slot plays again
    eng_->setOutputTap(nullptr);
    armed_ = false;
    pg::sampler::RecState s = rs_.state();
    lastReason = rs_.endReason();
    if (s == pg::sampler::RecState::Done || s == pg::sampler::RecState::Full) {
      std::string err;
      if (!commit_(slot_, sink_.v.data(), rs_.length(), &err)) {
        s = pg::sampler::RecState::Failed;
        why_ = "CANNOT WRITE";
        lastError = err;
      } else {
        ++commits;
      }
    } else if (s == pg::sampler::RecState::Failed) {
      why_ = rs_.endReason() == pg::sampler::Resampler::kOverrun ? "OVERRUN" : "WRITE";
    }
    voice_->setSilentSlot(-1);
    lastState = s;
    lastLength = rs_.length();
    lastStartFrame = rs_.startFrame();
    lastEndFrame = rs_.endFrame();
    if (log) {
      std::printf("[resample] slot %d: %s, %.3f s (%u samples), %s, %d bar line(s), peak %d\n", slot_ + 1,
                  pg::sampler::recStateName(s), double(rs_.length()) / pg::sampler::kPoolRate, unsigned(rs_.length()),
                  rs_.sync() ? "in time" : "from the first sound", rs_.bars(), int(rec_.peak()));
      std::fflush(stdout);
    }
    v->state = s;
    v->why = why_;
    return true;
  }

  bool armed() const { return armed_; }
  const pg::sampler::Resampler& resampler() const { return rs_; }
  const std::vector<int16_t>& recorded() const { return sink_.v; }   // raw, as the recorder wrote it
  int starts = 0, releases = 0, commits = 0;
  pg::sampler::RecState lastState = pg::sampler::RecState::Off;
  pg::sampler::Resampler::End lastReason = pg::sampler::Resampler::kNotYet;
  uint32_t lastLength = 0, lastStartFrame = 0, lastEndFrame = 0;
  std::string lastError;

 private:
  void fill(pg::sampler::RecView* v) const {
    v->state = rs_.state();
    v->slot = slot_;
    v->samples = rs_.ended() ? rs_.length() : rec_.recorded();
    v->level = rec_.level();
    v->peak = rec_.peak();
    v->cols = rec_.cols();
    v->colsUsed = rs_.started() ? rec_.colsUsed() : 0;
    v->resample = true;
    v->sync = rs_.sync();
    v->bars = rs_.bars();
    v->barAt = rs_.barAt();
  }

  pg::Engine* eng_ = nullptr;
  pg::sampler::SamplerVoice* voice_ = nullptr;
  float rate_ = 44100.0f;
  Commit commit_;
  std::function<bool(int)> erase_;
  pg::sampler::Recorder rec_;
  pg::sampler::Resampler rs_;
  VecSink sink_;
  std::vector<int16_t> ring_ = std::vector<int16_t>(kRing);
  bool armed_ = false;
  int slot_ = 0;
  const char* why_ = "";
};

// The commit the tests and ui_shot use: into a MemoryPool slot, levelled as both
// platforms play it (the peak at -1 dBFS, at most +24 dB: the device's header gain,
// the desktop's converter), named RESnn. bufs: one kSlotMaxSamples buffer per slot.
inline bool commitToPool(pg::sampler::MemoryPool* pool, std::vector<int16_t>* bufs, int slot, const int16_t* d,
                         uint32_t n, std::string* err) {
  std::vector<int16_t>& b = bufs[slot];
  b.assign(pg::sampler::kSlotMaxSamples, 0);
  int pk = 1;
  for (uint32_t i = 0; i < n; ++i) pk = std::max(pk, std::abs(int(d[i])));
  double g = double(pg::sampler::kRecordTargetPeak) / double(pk);
  if (g > double(pg::sampler::kRecordMaxGain)) g = double(pg::sampler::kRecordMaxGain);
  if (g < 1.0) g = 1.0;
  for (uint32_t i = 0; i < n; ++i) {
    const long v = std::lround(double(d[i]) * g);
    b[i] = int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
  }
  char name[16];
  std::snprintf(name, sizeof name, "RES%02d", (slot + 1) % 100);
  if (!pool->set(slot, b.data(), n, name)) {
    *err = "pool refused it";
    return false;
  }
  return true;
}

}  // namespace hostrs
