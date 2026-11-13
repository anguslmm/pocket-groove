// The desktop's microphone. See mic.h.
#include "mic.h"

#include <cstdio>

#include "samples.h"

namespace desk {

using pg::sampler::RecState;

void Mic::onAudio(const int16_t* mono, int n, void* user) {
  Mic* m = static_cast<Mic*>(user);
  const uint32_t head = m->head_.load(std::memory_order_relaxed);
  const uint32_t tail = m->tail_.load(std::memory_order_acquire);
  const uint32_t space = kRing - (head - tail);
  const uint32_t k = uint32_t(n) < space ? uint32_t(n) : space;
  for (uint32_t i = 0; i < k; ++i) m->ring_[(head + i) & (kRing - 1)] = mono[i];
  m->head_.store(head + k, std::memory_order_release);
  if (k < uint32_t(n)) m->dropped_.fetch_add(uint32_t(n) - k, std::memory_order_relaxed);
}

bool Mic::ready(const char** why) {
  if (!samples_ || !src_) {
    *why = "NO MIC";
    return false;
  }
  *why = "";
  return true;
}

bool Mic::erase(int slot) {
  std::string msg;
  return samples_ && samples_->clearSlot(slot, &msg);
}

bool Mic::start(int slot) {
  why_ = "";
  if (armed_ || !samples_ || !src_) {
    why_ = "NO MIC";
    return false;
  }
  head_.store(0);
  tail_.store(0);
  dropped_.store(0);
  sink_.v.clear();
  sink_.v.reserve(pg::sampler::kSlotMaxSamples + 1024);
  rec_.start(&sink_);
  err_.clear();
  if (!src_->open(&Mic::onAudio, this, &err_)) {
    std::printf("[mic] cannot open %s: %s\n", src_->name().c_str(), err_.c_str());
    why_ = "CANNOT OPEN";
    return false;
  }
  slot_ = slot;
  armed_ = true;
  std::printf("[mic] recording into slot %d from %s (hold L; it starts at the sound)\n", slot + 1,
              src_->name().c_str());
  return true;
}

void Mic::release() {
  if (armed_) rec_.release();
}

void Mic::drain() {
  int16_t buf[512];
  for (;;) {
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    uint32_t n = head - tail;
    if (n == 0) return;
    if (n > 512) n = 512;
    for (uint32_t i = 0; i < n; ++i) buf[i] = ring_[(tail + i) & (kRing - 1)];
    tail_.store(tail + n, std::memory_order_release);
    rec_.feed(buf, int(n));
  }
}

bool Mic::poll(pg::sampler::RecView* v) {
  *v = pg::sampler::RecView{};
  v->why = why_;
  if (!armed_) return false;
  const bool rel = rec_.released();
  drain();
  if (rel && !rec_.ended()) rec_.finish();
  v->state = rec_.state();
  v->slot = slot_;
  v->samples = rec_.ended() ? rec_.length() : rec_.recorded();
  v->level = rec_.level();
  v->threshold = rec_.threshold();
  v->peak = rec_.peak();
  v->cols = rec_.cols();
  v->colsUsed = rec_.colsUsed();
  if (rec_.ended()) {
    finishSession();
    v->state = why_[0] ? RecState::Failed : rec_.state();
    v->why = why_;
  }
  return true;
}

void Mic::finishSession() {
  src_->close();
  armed_ = false;
  const RecState s = rec_.state();
  const uint32_t drops = dropped_.load();
  if (s == RecState::Done || s == RecState::Full) {
    std::string msg;
    if (!samples_->putRecording(slot_, sink_.v.data(), rec_.length(), &msg)) {
      why_ = "CANNOT WRITE";
      std::printf("[mic] slot %d: %s\n", slot_ + 1, msg.c_str());
      return;
    }
    lastFile_ = samples_->fileOf(slot_);
    std::printf("[mic] slot %d: %s, %.2f s recorded, peak %d, threshold %d, room rms %d, %u samples dropped\n",
                slot_ + 1, lastFile_.c_str(), double(rec_.length()) / pg::sampler::kPoolRate, int(rec_.peak()),
                int(rec_.threshold()), int(rec_.floorRms()), unsigned(drops));
  } else {
    std::printf("[mic] slot %d: %s (nothing kept)\n", slot_ + 1, pg::sampler::recStateName(s));
  }
}

}  // namespace desk
