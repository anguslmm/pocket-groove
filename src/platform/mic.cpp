// Recording from the built-in microphone. See mic.h.
#include "mic.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "audio_out.h"
#include "sample_flash.h"

namespace plat {
namespace mic {

namespace audio = plat::audio;
using pg::sampler::RecState;
using pg::sampler::SampleWriter;

namespace {

uint32_t nowUs() { return static_cast<uint32_t>(esp_timer_get_time()); }
uint32_t heapFree() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); }
uint32_t heapMinEver() {
  return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// The slot's writer as the Recorder's sink: 512-byte flash writes, timed.
struct FlashSink final : pg::sampler::RecordSink {
  uint32_t writes = 0, errors = 0, maxUs = 0, sumUs = 0;
  bool write(uint32_t offset, const int16_t* d, uint32_t n) override {
    const uint32_t t0 = nowUs();
    const bool ok = samples::writer().write(offset * 2u, reinterpret_cast<const uint8_t*>(d), n * 2u) == SampleWriter::kOk;
    const uint32_t dt = nowUs() - t0;
    ++writes;
    sumUs += dt;
    if (dt > maxUs) maxUs = dt;
    if (!ok) ++errors;
    return ok;
  }
};

// Everything a recording needs, from the heap for its duration.
struct Session {
  pg::sampler::Recorder rec;
  audio::MicRing ring;
  int16_t ringBuf[kRingSamples];
  FlashSink sink;
  TaskHandle_t task = nullptr;
  std::atomic<bool> done{false}, abort{false};
  uint32_t stackFree = 0;
  uint32_t t0Us = 0;
};

// The last recording, for the report (the only static state: ~100 bytes).
struct Last {
  bool valid;
  int8_t slot;
  uint8_t state;            // RecState at the end
  const char* commit;       // SampleWriter::resultName or "-"
  uint32_t samples, peak, gain256, threshold, floorRms;
  uint32_t ringDropped, rxOverflows, readErrors, writes, writeErrors, maxWriteUs, avgWriteUs;
  uint32_t maxPullUs, maxBlockUs, maxGapUs, underruns, stackFree;
  uint32_t heapBefore, heapMinAfter, sessionBytes, ms, commitMs;
  uint32_t eraseMs, erasedCheckUs;
};
Last s_last = {};
Session* s_ss = nullptr;
const char* s_why = "";
uint32_t s_under0 = 0;

void writerTask(void* arg) {
  Session* ss = static_cast<Session*>(arg);
  int16_t buf[256];
  for (;;) {
    const bool rel = ss->rec.released();
    for (;;) {   // everything the ring holds now
      const uint32_t head = ss->ring.head.load(std::memory_order_acquire);
      const uint32_t tail = ss->ring.tail.load(std::memory_order_relaxed);
      uint32_t n = head - tail;
      if (n == 0) break;
      if (n > 256) n = 256;
      for (uint32_t i = 0; i < n; ++i) buf[i] = ss->ringBuf[(tail + i) & ss->ring.mask];
      ss->ring.tail.store(tail + n, std::memory_order_release);
      ss->rec.feed(buf, static_cast<int>(n));
    }
    if (rel && !ss->rec.ended()) ss->rec.finish();
    if (ss->rec.ended() || ss->abort.load()) break;
    vTaskDelay(pdMS_TO_TICKS(4));
  }
  ss->stackFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  ss->done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void freeSession() {
  if (!s_ss) return;
  s_ss->~Session();
  heap_caps_free(s_ss);
  s_ss = nullptr;
}

class DeviceMic final : public pg::sampler::MicInput {
 public:
  bool ready(const char** why) override {
    if (!samples::info().ok) {
      *why = "NO SAMPLE POOL";
      return false;
    }
    return audio::micAvailable(why);
  }

  bool slotErased(int slot) override {
    const uint32_t t0 = nowUs();
    const bool e = samples::writer().erased(slot);
    s_last.erasedCheckUs = nowUs() - t0;
    return e;
  }

  bool erase(int slot) override {
    audio::fadeOut();
    const uint32_t t0 = nowUs();
    const SampleWriter::Result r = samples::writer().remove(slot);
    s_last.eraseMs = (nowUs() - t0) / 1000u;
    audio::fadeIn();
    return r == SampleWriter::kOk;
  }

  bool start(int slot) override {
    s_why = "";
    if (s_ss) {
      s_why = "BUSY";
      return false;
    }
    const char* why = "";
    if (!ready(&why)) {
      s_why = why;
      return false;
    }
    const uint32_t heap0 = heapFree();
    if (samples::writer().beginRecording(slot) != SampleWriter::kOk) {
      s_why = "NOT ERASED";
      return false;
    }
    void* mem = heap_caps_malloc(sizeof(Session), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem) {
      samples::writer().cancel();
      s_why = "NO MEMORY";
      return false;
    }
    s_ss = new (mem) Session();
    s_ss->ring.buf = s_ss->ringBuf;
    s_ss->ring.mask = kRingSamples - 1;
    s_ss->rec.start(&s_ss->sink);
    s_ss->t0Us = nowUs();
    audio::micResetStats();
    s_under0 = audio::stats().underruns;
    if (!audio::micBegin(&s_ss->ring)) {
      freeSession();
      samples::writer().cancel();
      s_why = "NO INPUT";
      return false;
    }
    if (xTaskCreatePinnedToCore(writerTask, "pg_mic", kTaskStack, s_ss, kTaskPrio, &s_ss->task, 1) != pdPASS) {
      audio::micEnd();
      freeSession();
      samples::writer().cancel();
      s_why = "NO MEMORY";
      return false;
    }
    const uint32_t eraseMs = s_last.eraseMs, checkUs = s_last.erasedCheckUs;   // from just before
    s_last = Last{};
    s_last.eraseMs = eraseMs;
    s_last.erasedCheckUs = checkUs;
    s_last.valid = true;
    s_last.slot = static_cast<int8_t>(slot);
    s_last.state = static_cast<uint8_t>(RecState::Settling);
    s_last.commit = "-";
    s_last.heapBefore = heap0;
    s_last.sessionBytes = static_cast<uint32_t>(sizeof(Session)) + kTaskStack;
    return true;
  }

  void release() override {
    if (s_ss) s_ss->rec.release();
  }

  bool poll(pg::sampler::RecView* v) override {
    *v = pg::sampler::RecView{};
    v->why = s_why;
    if (!s_ss) return false;
    Session& ss = *s_ss;
    v->state = ss.rec.state();
    v->slot = s_last.slot;
    v->samples = ss.rec.ended() ? ss.rec.length() : ss.rec.recorded();
    v->level = ss.rec.level();
    v->threshold = ss.rec.threshold();
    v->peak = ss.rec.peak();
    v->cols = ss.rec.cols();
    v->colsUsed = ss.rec.colsUsed();
    if (!ss.done.load(std::memory_order_acquire)) return true;
    // the writer task has ended: stop the input, commit, free
    audio::micEnd();
    RecState st = ss.rec.state();
    const uint32_t t0 = nowUs();
    if (st == RecState::Done || st == RecState::Full) {
      char name[12];
      snprintf(name, sizeof name, "REC%02d", (s_last.slot + 1) % 100);
      const SampleWriter::Result r = samples::writer().commitRecording(ss.rec.length(), name);
      s_last.commit = SampleWriter::resultName(r);
      if (r != SampleWriter::kOk) {
        samples::writer().cancel();
        st = RecState::Failed;
        s_why = r == SampleWriter::kVerify ? "VERIFY" : "COMMIT";
      }
    } else {
      samples::writer().cancel();   // the slot stays empty (and erased, unless a blip was written)
      if (st == RecState::Failed) s_why = "FLASH WRITE";
    }
    s_last.commitMs = (nowUs() - t0) / 1000u;
    const audio::MicStats ms = audio::micStats();
    s_last.state = static_cast<uint8_t>(st);
    s_last.samples = ss.rec.ended() ? ss.rec.length() : ss.rec.recorded();
    s_last.peak = ss.rec.peak();
    s_last.threshold = ss.rec.threshold();
    s_last.floorRms = ss.rec.floorRms();
    const pg::sampler::SlotHeader* h = samples::pool().header(s_last.slot);
    s_last.gain256 = h ? (h->gain ? h->gain : 256u) : 0u;
    s_last.ringDropped = ss.ring.dropped.load();
    s_last.rxOverflows = ms.rxOverflows;
    s_last.readErrors = ms.readErrors;
    s_last.writes = ss.sink.writes;
    s_last.writeErrors = ss.sink.errors;
    s_last.maxWriteUs = ss.sink.maxUs;
    s_last.avgWriteUs = ss.sink.writes ? ss.sink.sumUs / ss.sink.writes : 0;
    s_last.maxPullUs = static_cast<uint32_t>(ms.maxPullUs);
    s_last.maxBlockUs = static_cast<uint32_t>(ms.maxBlockUs);
    s_last.maxGapUs = static_cast<uint32_t>(ms.maxGapUs);
    s_last.underruns = audio::stats().underruns - s_under0;
    s_last.stackFree = ss.stackFree;
    s_last.ms = (nowUs() - ss.t0Us) / 1000u;
    freeSession();
    s_last.heapMinAfter = heapMinEver();
    v->state = st;
    v->why = s_why;
    return true;
  }
};

DeviceMic s_mic;

float dbfs(double v) { return v > 0.0 ? static_cast<float>(20.0 * std::log10(v / 32768.0)) : -999.0f; }
int tenths(float v) { return static_cast<int>(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f)); }

}  // namespace

pg::sampler::MicInput& input() { return s_mic; }
bool busy() { return s_ss != nullptr; }

void report(void (*line)(const char* text)) {
  char b[400];
  // registers, the pin
  {
    const audio::MicStats ms = audio::micStats();
    const audio::PinState p = audio::micPin();
    const char* why = "";
    const bool avail = audio::micAvailable(&why);
    snprintf(b, sizeof b,
             "[mic] %s%s, rx %s%s, adc %s, recording %s; gpio46 oe=%u out=%u level=%u ie=%u out_sel=%u "
             "(256 = plain GPIO: oe=1 there means the ESP drives the mic line), i2s1 data-in from gpio %u\n",
             avail ? "AVAILABLE" : "NOT AVAILABLE: ", avail ? "" : why,
             ms.rxRegistered ? (ms.rxInitialised ? "initialised" : "registered") : "NOT REGISTERED",
             ms.attached ? " + attached" : "", ms.adcOn ? "on" : "off (first use powers it)", s_ss ? "YES" : "no",
             p.oe, p.out, p.level, p.ie, p.outSel, p.inSel & 0x3Fu);
    line(b);
    static const uint8_t kShow[] = {0x00, 0x01, 0x02, 0x09, 0x0A, 0x0D, 0x0E, 0x12, 0x13, 0x14,
                                    0x15, 0x16, 0x17, 0x1B, 0x1C, 0x32, 0x37, 0x44, 0x45};
    int n = snprintf(b, sizeof b, "[mic] codec regs:");
    int na = 0;
    const uint8_t (*adc)[2] = audio::micAdcRegs(&na);
    int diffs = 0;
    for (uint8_t r : kShow) {
      uint8_t v = 0;
      const bool ok = audio::codecRead(r, &v);
      n += snprintf(b + n, sizeof b - static_cast<size_t>(n), ok ? " %02X=%02X" : " %02X=??", r, v);
      for (int i = 0; i < na && ms.adcOn; ++i)
        if (adc[i][0] == r && (!ok || v != adc[i][1])) {
          ++diffs;
          n += snprintf(b + n, sizeof b - static_cast<size_t>(n), "(want %02X)", adc[i][1]);
        }
    }
    snprintf(b + n, sizeof b - static_cast<size_t>(n), " | ADC set %s\n",
             ms.adcOn ? (diffs ? "DIFFERS" : "as written") : "not written yet");
    line(b);
  }
  // the input now: ~1 s of the left slot with the RX running (not while recording)
  if (!s_ss) {
    const char* why = "";
    if (!audio::micAvailable(&why)) {
      snprintf(b, sizeof b, "[mic] input now: not measured (%s)\n", why);
      line(b);
    } else {
      const uint32_t heap0 = heapFree();
      constexpr uint32_t kN = 2048;
      void* mem = heap_caps_malloc(sizeof(audio::MicRing) + kN * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!mem) {
        line("[mic] input now: not measured (no memory for the ring)\n");
      } else {
        audio::MicRing* ring = new (mem) audio::MicRing();
        ring->buf = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(mem) + sizeof(audio::MicRing));
        ring->mask = kN - 1;
        audio::micResetStats();
        const uint32_t under0 = audio::stats().underruns;
        if (!audio::micBegin(ring)) {
          snprintf(b, sizeof b, "[mic] input now: micBegin failed: %s\n", audio::lastError());
          line(b);
        } else {
          double sum = 0, sum2 = 0;
          int mn = 32767, mx = -32768, first = 0;
          uint32_t count = 0, same = 0;
          bool haveFirst = false;
          const uint32_t tStart = nowUs();
          for (;;) {
            const uint32_t el = nowUs() - tStart;
            if (el > 1300000u) break;
            const uint32_t head = ring->head.load(std::memory_order_acquire);
            uint32_t tail = ring->tail.load(std::memory_order_relaxed);
            for (; tail != head; ++tail) {
              if (el < 300000u) continue;   // the first 300 ms: the ADC and the filters settle
              const int v = ring->buf[tail & ring->mask];
              if (!haveFirst) { first = v; haveFirst = true; }
              if (v == first) ++same;
              sum += v;
              sum2 += static_cast<double>(v) * v;
              if (v < mn) mn = v;
              if (v > mx) mx = v;
              ++count;
            }
            ring->tail.store(tail, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(5));
          }
          audio::micEnd();
          const audio::MicStats ms = audio::micStats();
          if (count == 0) {
            snprintf(b, sizeof b,
                     "[mic] input now: NO SAMPLES in 1 s (rx blocks %lu, read errors %lu, overflows %lu)\n",
                     static_cast<unsigned long>(ms.blocks), static_cast<unsigned long>(ms.readErrors),
                     static_cast<unsigned long>(ms.rxOverflows));
          } else {
            const double mean = sum / count;
            const double var = sum2 / count - mean * mean;
            const double rms = var > 0 ? std::sqrt(var) : 0.0;
            const int pk = (mx > -mn ? mx : -mn);
            snprintf(b, sizeof b,
                     "[mic] input now (1.0 s, left slot, PGA 30 dB): rms %d.%d dBFS (DC removed), peak %d.%d dBFS, "
                     "DC %+ld LSB, min %d max %d, %lu samples (44100 expected)%s; rx overflows %lu, ring drops %lu, "
                     "underruns %lu, longest rx pull %luus\n",
                     tenths(dbfs(rms)) / 10, std::abs(tenths(dbfs(rms)) % 10), tenths(dbfs(pk)) / 10,
                     std::abs(tenths(dbfs(pk)) % 10), static_cast<long>(std::lround(mean)), mn, mx,
                     static_cast<unsigned long>(count),
                     same == count ? (first == -1 ? " STUCK AT -1: the ESP drives GPIO46 (fix not applied?)"
                                                  : " CONSTANT: no signal from the ADC")
                                   : "",
                     static_cast<unsigned long>(ms.rxOverflows), static_cast<unsigned long>(ring->dropped.load()),
                     static_cast<unsigned long>(audio::stats().underruns - under0),
                     static_cast<unsigned long>(ms.maxPullUs));
          }
          line(b);
        }
        ring->~MicRing();
        heap_caps_free(mem);
        snprintf(b, sizeof b,
                 "[mic] the measurement took 1.3 s of the UI loop and %lu B of heap for its ring (heap free %lu)\n",
                 static_cast<unsigned long>(heap0 - heapFree() + sizeof(audio::MicRing) + kN * 2u),
                 static_cast<unsigned long>(heap0));
        line(b);
      }
    }
  } else {
    const pg::sampler::Recorder& r = s_ss->rec;
    snprintf(b, sizeof b, "[mic] recording now: %s, %lu samples, level %u, threshold %u\n",
             pg::sampler::recStateName(r.state()), static_cast<unsigned long>(r.recorded()), r.level(),
             r.threshold());
    line(b);
  }
  // the last recording
  if (!s_last.valid) {
    snprintf(b, sizeof b, "[mic] last recording: none since boot (last erase %lu ms, erased check %lu us)\n",
             static_cast<unsigned long>(s_last.eraseMs), static_cast<unsigned long>(s_last.erasedCheckUs));
    line(b);
    return;
  }
  const uint32_t cs = s_last.samples * 100u / pg::sampler::kPoolRate;
  snprintf(b, sizeof b,
           "[mic] last recording: slot %d, %s, commit %s (%lu ms), %lu samples (%lu.%02lu s), peak %lu (%d.%d dBFS), "
           "gain x%lu.%02lu, threshold %lu, room rms %lu, %lu ms from L down to the end\n",
           s_last.slot + 1, pg::sampler::recStateName(static_cast<RecState>(s_last.state)), s_last.commit,
           static_cast<unsigned long>(s_last.commitMs), static_cast<unsigned long>(s_last.samples),
           static_cast<unsigned long>(cs / 100), static_cast<unsigned long>(cs % 100),
           static_cast<unsigned long>(s_last.peak), tenths(dbfs(s_last.peak)) / 10,
           std::abs(tenths(dbfs(s_last.peak)) % 10), static_cast<unsigned long>(s_last.gain256 / 256u),
           static_cast<unsigned long>((s_last.gain256 % 256u) * 100u / 256u),
           static_cast<unsigned long>(s_last.threshold), static_cast<unsigned long>(s_last.floorRms),
           static_cast<unsigned long>(s_last.ms));
  line(b);
  snprintf(b, sizeof b,
           "[mic] last recording: ring drops %lu, rx overflows %lu, rx read errors %lu, flash writes %lu (errors %lu, "
           "avg %luus, longest %luus); audio while recording: longest block %luus, longest gap %luus, underruns "
           "%lu, longest rx pull %luus\n",
           static_cast<unsigned long>(s_last.ringDropped), static_cast<unsigned long>(s_last.rxOverflows),
           static_cast<unsigned long>(s_last.readErrors), static_cast<unsigned long>(s_last.writes),
           static_cast<unsigned long>(s_last.writeErrors), static_cast<unsigned long>(s_last.avgWriteUs),
           static_cast<unsigned long>(s_last.maxWriteUs), static_cast<unsigned long>(s_last.maxBlockUs),
           static_cast<unsigned long>(s_last.maxGapUs), static_cast<unsigned long>(s_last.underruns),
           static_cast<unsigned long>(s_last.maxPullUs));
  line(b);
  snprintf(b, sizeof b,
           "[mic] last recording: writer stack free %lu of %lu, heap %lu B for it (heap free before %lu, min-ever "
           "after %lu), last erase %lu ms (output faded), erased check %lu us\n",
           static_cast<unsigned long>(s_last.stackFree), static_cast<unsigned long>(kTaskStack),
           static_cast<unsigned long>(s_last.sessionBytes), static_cast<unsigned long>(s_last.heapBefore),
           static_cast<unsigned long>(s_last.heapMinAfter), static_cast<unsigned long>(s_last.eraseMs),
           static_cast<unsigned long>(s_last.erasedCheckUs));
  line(b);
}

}  // namespace mic
}  // namespace plat
