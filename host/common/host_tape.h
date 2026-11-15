// The tape on the host (docs/DESKTOP.md "Tape"): its memory from the heap, and the
// lift (pg/tape/tape_io.h SlotWriter) into a sample pool slot for the desktop app, the
// tests and ui_shot. Host code (heap, the standard library).
//
// THE LIFT keeps the pool's rule for the stored rate (recorder.h recRatesFor): a tape at
// 44.1 kHz goes into 22,050 Hz slots through the recorder's half-band (fed the loop's
// end in front and its start behind, the loop being circular, and the filter's delay
// taken off as the resampler does, so the slot's sample k is centred on tape sample
// 2k), a tape at 48 kHz is stored as it is. Then `commit` keeps it (the desktop:
// samples/NN-tapNN.wav; the tests: a MemoryPool), levelled at playback as recordings are.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pg/sampler/recorder.h"
#include "pg/sampler/sample_format.h"
#include "pg/tape/tape_io.h"

namespace hosttape {

class Lift final : public pg::tape::SlotWriter {
 public:
  using Commit = std::function<bool(int slot, const int16_t* data, uint32_t n, uint32_t rate, std::string* err)>;
  Commit commit;

  uint32_t capacity(uint32_t rate) const override {
    const pg::sampler::RecRates r = pg::sampler::recRatesFor(rate, pg::sampler::kPoolRate);
    return pg::sampler::kSlotMaxSamples * r.factor();
  }
  bool lift(int slot, const pg::tape::Tape& tape, int t, uint32_t start, uint32_t n, const char** why) override {
    err_.clear();
    if (!commit) { *why = "NO POOL"; return false; }
    const pg::sampler::RecRates r = pg::sampler::recRatesFor(tape.rate(), pg::sampler::kPoolRate);
    if (!r.valid()) { *why = "RATE"; return false; }
    std::vector<int16_t> out;
    if (r.factor() == 1) {
      out.resize(n);
      tape.read(t, start, out.data(), n);
    } else {
      // 24 samples in front (the lifted part's end) and 22 behind (its start): it is a loop
      const uint32_t pre = 24, post = 22;
      std::vector<int16_t> in(size_t(pre) + n + post, 0);
      const uint32_t L = n ? n : 1;
      for (uint32_t i = 0; i < pre + n + post; ++i) {
        int64_t a = int64_t(start) + int64_t(i) - int64_t(pre);
        a %= int64_t(L);
        if (a < 0) a += L;
        tape.read(t, uint32_t(a), &in[i], 1);
      }
      pg::sampler::HalfBand hb;
      hb.reset();
      const uint32_t skip = 23;
      for (uint32_t i = 0; i + 1 < in.size(); i += 2) {
        const float y = hb.run(float(in[i]), float(in[i + 1]));
        if (i / 2 < skip) continue;
        if (out.size() >= n / 2) break;
        const long v = std::lround(double(y));
        out.push_back(int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)));
      }
    }
    if (!commit(slot, out.data(), uint32_t(out.size()), r.out, &err_)) {
      *why = err_.empty() ? "FAILED" : err_.c_str();
      return false;
    }
    return true;
  }

 private:
  std::string err_;
};

// A tape with `seconds` of track time (all tracks together) at `rate`, on the heap.
struct HostTape {
  std::vector<uint8_t> mem;
  pg::tape::Tape tape;
  bool init(uint32_t seconds, uint32_t rate) {
    mem.assign(pg::tape::Tape::bytesFor(seconds, rate), 0);
    return tape.init(mem.data(), mem.size(), rate);
  }
};

}  // namespace hosttape
