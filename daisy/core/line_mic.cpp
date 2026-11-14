// See line_mic.h.
#include "line_mic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace pgd {

using pg::sampler::RecState;
using pg::sampler::SampleWriter;

bool LineMic::Sink::write(uint32_t offset, const int16_t* d, uint32_t n) {
  ++writes;
  const bool ok = w->write(offset * 2u, reinterpret_cast<const uint8_t*>(d), n * 2u) == SampleWriter::kOk;
  if (!ok) ++errors;
  return ok;
}

void LineMic::init(SampleWriter* writer, pg::sampler::FlashDriver* driver, pg::sampler::FlashPool* pool,
                   int16_t* ring, uint32_t ringSamples, uint32_t rate) {
  armed_.store(false, std::memory_order_release);
  writer_ = writer;
  drv_ = driver;
  pool_ = pool;
  uint32_t c = 1;
  while (c * 2 <= ringSamples && c < (1u << 30)) c *= 2;
  ring_ = ring;
  mask_ = ring ? c - 1 : 0;
  rate_ = rate;
  sink_.w = writer;
  slot_ = -1;
  why_ = "";
}

void LineMic::audioIn(const float* in, int frames) {
  if (!in || !armed_.load(std::memory_order_acquire)) return;
  blocks_.fetch_add(1, std::memory_order_relaxed);
  float pl = 0.0f, pr = 0.0f;
  for (int i = 0; i < frames; ++i) {
    const float l = std::fabs(in[2 * i]), r = std::fabs(in[2 * i + 1]);
    if (l > pl) pl = l;
    if (r > pr) pr = r;
  }
  const uint16_t ql = uint16_t(pl >= 1.0f ? 32767 : int(pl * 32767.0f));
  const uint16_t qr = uint16_t(pr >= 1.0f ? 32767 : int(pr * 32767.0f));
  if (ql > peakL_.load(std::memory_order_relaxed)) peakL_.store(ql, std::memory_order_relaxed);
  if (qr > peakR_.load(std::memory_order_relaxed)) peakR_.store(qr, std::memory_order_relaxed);
  if (!frozen_.load(std::memory_order_relaxed) && qr >= kSilentPeak)
    mono_.store(uint8_t(Mono::Sum), std::memory_order_relaxed);
  const bool sum = mono_.load(std::memory_order_relaxed) == uint8_t(Mono::Sum);
  const uint32_t h = head_.load(std::memory_order_relaxed);
  const uint32_t t = tail_.load(std::memory_order_acquire);
  if (uint32_t(frames) > mask_ + 1 - (h - t)) {
    overruns_.fetch_add(uint32_t(frames), std::memory_order_relaxed);
    return;
  }
  for (int i = 0; i < frames; ++i) {
    const float v = sum ? (in[2 * i] + in[2 * i + 1]) * 0.5f : in[2 * i];
    const float s = v * 32767.0f;
    const int q = s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : int(lrintf(s));
    ring_[(h + uint32_t(i)) & mask_] = int16_t(q);
  }
  head_.store(h + uint32_t(frames), std::memory_order_release);
}

bool LineMic::ready(const char** why) {
  if (!writer_ || !writer_->ready() || !ring_) {
    *why = "NO SAMPLE POOL";
    return false;
  }
  if (!available_) {
    *why = "NO INPUT";
    return false;
  }
  *why = "";
  return true;
}

bool LineMic::slotErased(int slot) { return writer_ && writer_->erased(slot); }

bool LineMic::erase(int slot) { return writer_ && writer_->remove(slot) == SampleWriter::kOk; }

bool LineMic::start(int slot) {
  why_ = "";
  const char* why = "";
  if (!ready(&why)) {
    why_ = why;
    return false;
  }
  if (armed_.load(std::memory_order_acquire)) {
    why_ = "BUSY";
    return false;
  }
  if (writer_->beginRecording(slot) != SampleWriter::kOk) {
    why_ = "NOT ERASED";
    return false;
  }
  sink_.writes = sink_.errors = 0;
  rec_.start(&sink_, pg::sampler::kSlotMaxSamples, pg::sampler::recRatesFor(rate_, pg::sampler::kPoolRate));
  if (rec_.state() == RecState::Failed) {
    writer_->cancel();
    why_ = "RATE";
    return false;
  }
  slot_ = slot;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  overruns_.store(0, std::memory_order_relaxed);
  blocks_.store(0, std::memory_order_relaxed);
  peakL_.store(0, std::memory_order_relaxed);
  peakR_.store(0, std::memory_order_relaxed);
  mono_.store(uint8_t(Mono::LeftOnly), std::memory_order_relaxed);
  frozen_.store(false, std::memory_order_relaxed);
  armed_.store(true, std::memory_order_release);   // the interrupt starts filling the ring
  return true;
}

void LineMic::release() {
  if (armed_.load(std::memory_order_acquire)) rec_.release();
}

void LineMic::drain() {
  int16_t buf[256];
  for (;;) {
    const uint32_t h = head_.load(std::memory_order_acquire);
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    uint32_t n = h - t;
    if (n == 0) return;
    if (n > 256) n = 256;
    for (uint32_t i = 0; i < n; ++i) buf[i] = ring_[(t + i) & mask_];
    tail_.store(t + n, std::memory_order_release);
    rec_.feed(buf, int(n));
    if (rec_.state() == RecState::Recording) frozen_.store(true, std::memory_order_relaxed);
  }
}

SampleWriter::Result LineMic::commitAt(SampleWriter& w, pg::sampler::FlashDriver& drv, pg::sampler::FlashPool& pool,
                                       uint32_t samples, const char* name, uint32_t rate) {
  using namespace pg::sampler;
  if (!w.recording() || !pool.base()) return SampleWriter::kNoSession;
  const int slot = w.slot();
  if (samples < kMinSamples || samples > kSlotMaxSamples || samples * 2u > w.written()) return SampleWriter::kBadSize;
  const uint32_t head = uint32_t(slot) * kSlotBytes, audio = head + kSectorBytes, bytes = samples * 2u;
  uint8_t buf[256];
  uint32_t c = 0;
  for (uint32_t o = 0; o < bytes;) {   // the audio as the flash holds it ...
    const uint32_t n = bytes - o < sizeof buf ? bytes - o : uint32_t(sizeof buf);
    if (!drv.read(audio + o, buf, n)) return SampleWriter::kFlash;
    c = crc32(buf, n, c);
    o += n;
  }
  SlotHeader h;   // ... and as the voice reads it: the same CRC
  makeHeader(reinterpret_cast<const int16_t*>(pool.base() + audio), samples, name, &h, rate);
  if (h.dataCrc != c) return SampleWriter::kVerify;
  setPlaybackGain(&h);
  if (!drv.write(head, &h, sizeof h)) return SampleWriter::kFlash;
  SlotHeader back;
  if (!drv.read(head, &back, sizeof back) || std::memcmp(&back, &h, sizeof h) != 0) return SampleWriter::kVerify;
  w.cancel();   // the writer lets the slot go; its rescan finds the valid header
  return pool.header(slot) ? SampleWriter::kOk : SampleWriter::kVerify;
}

void LineMic::end(RecState st, const char* why) {
  last_.state = st;
  why_ = why;
}

bool LineMic::poll(pg::sampler::RecView* v) {
  *v = pg::sampler::RecView{};
  v->why = why_;
  if (!armed_.load(std::memory_order_acquire)) return false;
  const bool rel = rec_.released();
  drain();
  if (rel && !rec_.ended()) rec_.finish();
  v->slot = slot_;
  v->rate = rec_.rates().out;
  v->capacity = pg::sampler::kSlotMaxSamples;
  v->state = rec_.state();
  v->samples = rec_.ended() ? rec_.length() : rec_.recorded();
  v->level = rec_.level();
  v->threshold = rec_.threshold();
  v->peak = rec_.peak();
  v->cols = rec_.cols();
  v->colsUsed = rec_.colsUsed();
  if (!rec_.ended()) return true;
  // the end: the interrupt stops, then the commit (or the slot is let go)
  armed_.store(false, std::memory_order_release);
  RecState st = rec_.state();
  last_ = Last{};
  last_.valid = true;
  last_.slot = int8_t(slot_);
  last_.result = "-";
  if (st == RecState::Done || st == RecState::Full) {
    char name[12];
    std::snprintf(name, sizeof name, "REC%02d", (slot_ + 1) % 100);
    const SampleWriter::Result r = commitAt(*writer_, *drv_, *pool_, rec_.length(), name, rec_.rates().out);
    last_.result = SampleWriter::resultName(r);
    if (r != SampleWriter::kOk) {
      writer_->cancel();
      st = RecState::Failed;
      why_ = r == SampleWriter::kVerify ? "VERIFY" : "COMMIT";
    }
  } else {
    writer_->cancel();   // the slot stays empty
    if (st == RecState::Failed) why_ = "WRITE";
  }
  end(st, why_);
  last_.mono = mono();
  last_.samples = rec_.ended() ? rec_.length() : rec_.recorded();
  last_.rate = rec_.rates().out;
  last_.overruns = overruns_.load(std::memory_order_relaxed);
  last_.writes = sink_.writes;
  last_.writeErrors = sink_.errors;
  last_.peakL = peakL_.load(std::memory_order_relaxed);
  last_.peakR = peakR_.load(std::memory_order_relaxed);
  last_.threshold = rec_.threshold();
  last_.floorRms = rec_.floorRms();
  last_.peak = rec_.peak();
  v->state = st;
  v->why = why_;
  return true;
}

}  // namespace pgd
