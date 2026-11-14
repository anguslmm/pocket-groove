// Recording from the codec's stereo line input on the Daisy (the first recording
// path; docs/DAISY.md "Line in"): the pg::sampler::MicInput the UI's gesture drives
// (hold L on a sampler page; over serial ">down l"), exactly as on the Cardputer:
// the same Recorder (threshold start with pre-roll, the trim at the end, the
// playback gain), into the same pool slot through pg's SampleWriter, which on the
// Daisy writes the SDRAM image (pool_mirror.h); the QSPI copy follows in the
// background, so the sample survives a restart.
//
//   audioIn()   the audio interrupt, every block: the codec's interleaved stereo
//               float block, while a recording is armed only. Folded to mono int16
//               into an SPSC ring (caller's memory: SDRAM on the device, 1.4 s at
//               48 kHz, so the main loop may stall for a flash erase or a UI frame
//               without losing input).
//   poll()      the main loop (every UI frame): the ring into the Recorder; at the
//               end the commit.
//
// MONO: the core's recorder takes mono. The fold-down is (L + R) / 2 (a stereo source
// keeps both sides and loses nothing to the sum), EXCEPT when the right input is
// silent: then L alone at full level. "Silent" is digital near-silence, every
// sample of a block below -60 dBFS: what a mono plug (TS: its sleeve grounds the
// right input) or a right input tied to AGND gives. A right input left open on a
// breadboard is NOT silent (it picks up hum), so it is summed: for a mono source,
// tie it to AGND. Why not always the sum: a mono source on L alone would lose 6 dB
// and gain R's noise. The choice starts as "left only" at every hold of L and
// becomes the sum at the first block in which R is not silent; it is frozen once the
// recording itself starts (Recording), so a sample never changes level half-way.
// The report says which it was, and the two input peaks.
//
// RATES: 48 kHz in, 48 kHz stored (recRatesFor(48000, kPoolRate)): no filter. A pool
// slot holds kSlotMaxSamples = 137,216 samples: 2.86 s at 48 kHz (the Cardputer's
// 22,050 Hz slot: 6.22 s). The slot's header says 48,000 Hz.
// THE CORE'S SampleWriter::commitRecording writes the header at kPoolRate (22,050)
// whatever the recording's rate, so this commits the header itself (commitAt():
// the same steps, the same checks, the rate passed in) and lets the writer go with
// cancel(), which rescans the now valid slot. A seam in the core would remove this
// (docs/DAISY.md "Seams").
// Portable (host/daisy/test_daisy_linein.cpp; the App in daisy_sim with --line-in).
#pragma once
#include <atomic>
#include <cstdint>

#include "pg/sampler/flash_pool.h"
#include "pg/sampler/recorder.h"

namespace pgd {

class LineMic final : public pg::sampler::MicInput {
 public:
  static constexpr int16_t kSilentPeak = 33;   // -60 dBFS: below it a block is silent

  // ring: ringSamples int16 (a power of two). rate: the codec's (48000).
  void init(pg::sampler::SampleWriter* writer, pg::sampler::FlashDriver* driver, pg::sampler::FlashPool* pool,
            int16_t* ring, uint32_t ringSamples, uint32_t rate);
  // Whether the input runs (the device: the audio started). false: ready() says NO INPUT.
  void setAvailable(bool on) { available_ = on; }

  // The audio interrupt: frames of interleaved stereo float (-1..1).
  void audioIn(const float* in, int frames);

  // ---- MicInput (the UI thread: the main loop) ----
  bool ready(const char** why) override;
  bool slotErased(int slot) override;
  bool erase(int slot) override;
  bool start(int slot) override;
  void release() override;
  bool poll(pg::sampler::RecView* v) override;

  // The commit with the slot's rate (see above). Public for the tests.
  static pg::sampler::SampleWriter::Result commitAt(pg::sampler::SampleWriter& w, pg::sampler::FlashDriver& drv,
                                                    pg::sampler::FlashPool& pool, uint32_t samples, const char* name,
                                                    uint32_t rate);

  // For the report.
  enum class Mono : uint8_t { LeftOnly, Sum };
  struct Last {
    bool valid;
    int8_t slot;
    pg::sampler::RecState state;
    Mono mono;
    const char* result;        // SampleWriter::resultName, or "-"
    uint32_t samples, rate, overruns, writes, writeErrors;
    uint16_t peakL, peakR, threshold, floorRms, peak;
  };
  const Last& last() const { return last_; }
  bool armed() const { return armed_.load(std::memory_order_acquire); }
  Mono mono() const { return Mono(mono_.load(std::memory_order_relaxed)); }
  uint32_t blocksIn() const { return blocks_.load(std::memory_order_relaxed); }

 private:
  struct Sink final : pg::sampler::RecordSink {
    pg::sampler::SampleWriter* w = nullptr;
    uint32_t writes = 0, errors = 0;
    bool write(uint32_t offset, const int16_t* d, uint32_t n) override;
  };
  void drain();
  void end(pg::sampler::RecState st, const char* why);

  pg::sampler::SampleWriter* writer_ = nullptr;
  pg::sampler::FlashDriver* drv_ = nullptr;
  pg::sampler::FlashPool* pool_ = nullptr;
  int16_t* ring_ = nullptr;
  uint32_t mask_ = 0;
  uint32_t rate_ = 48000;
  bool available_ = false;
  pg::sampler::Recorder rec_;
  Sink sink_;
  int slot_ = -1;
  const char* why_ = "";
  // shared with the interrupt
  std::atomic<bool> armed_{false};
  std::atomic<bool> frozen_{false};       // the mono choice no longer changes
  std::atomic<uint8_t> mono_{0};
  std::atomic<uint32_t> head_{0}, tail_{0}, overruns_{0}, blocks_{0};
  std::atomic<uint16_t> peakL_{0}, peakR_{0};
  Last last_{};
};

}  // namespace pgd
