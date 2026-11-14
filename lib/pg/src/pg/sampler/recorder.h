// Recording from a microphone into a sample pool slot (stage F2, docs/STATE.md,
// docs/UX.md 10.3 "Recording"): the portable part. The platform owns the input
// (the device: the codec's ADC over full-duplex I2S, src/platform/mic.*; the
// desktop: SDL's capture, host/app/mic.*) and the slot's storage; this file has the
// signal path and the state machine both share, and the interface the UI drives.
//
// SIGNAL PATH (Recorder::feed, on the platform's writer thread, never the audio thread):
//   int16 mono at the input rate -> [half-band low-pass + decimate by 2, only when the
//   stored rate is half the input rate (47 taps, 12 unique coefficients; at 44.1 kHz:
//   flat to 9 kHz within 0.003 dB, -6 dB at 11,025 Hz, <= -69 dB from 13,050 Hz on,
//   i.e. everything that would fold back below 9 kHz; the band edges scale with the
//   input rate)] -> DC block (one pole, 17.5 Hz) -> int16 at the stored rate into the
//   sink, 256 samples (one 512-byte flash write) at a time. No input monitoring.
// RATES (RecRates, a property of the platform and its pool): the Cardputer records
//   its 44,100 Hz ADC (and the engine at 44.1 kHz) into 22,050 Hz slots, the default;
//   a platform with room for it stores at the input rate (the Daisy: 48,000 in, 48,000
//   stored), and then nothing filters. The times below (settle, window, pre-roll,
//   tail, the meter) are in milliseconds and the same at every stored rate.
//
// STATE MACHINE (one writer thread; the UI reads the published state and calls
// release() from its own thread):
//   Settling   the first kSettleMs: nothing starts (the click of the key that armed
//              it, the converter's start); from kSkipMs on, the quietest 10 ms
//              window's RMS is the room's floor
//   Waiting    threshold = the floor + 20 dB, clamped to kThrMin..kThrMax; the last
//              kPreRollMs are kept in a ring; the first sample above the threshold
//              starts the recording with that pre-roll in front of it
//   Recording  every sample goes to the sink; Full when the slot is (kSlotMaxSamples,
//              6.22 s): it stops by itself
//   release()  (the key went up) -> finish(): Waiting/Settling = Cancelled (nothing
//              written); Recording = Done, the last kTailMs dropped (the click of the
//              key going up), Cancelled if less than kMinSamples is left
//   Failed     the sink refused a write
// The level is made right at playback (sample_format.h setPlaybackGain): nothing here
// scales the signal, so nothing has to be rewritten once the peak is known.
//
// Portable C++17, no heap: the device allocates a Recorder (about 6.2 KB: the
// pre-roll ring has room for 40 ms at 48 kHz) only while it records.
#pragma once
#include <atomic>
#include <cstdint>

#include "sample_format.h"

namespace pg {
namespace sampler {

// The rates of a recording: in = the source's (the ADC's, the engine's), out = what
// the slot stores (its header's rate). Valid: out == in (stored as it comes) or
// out == in / 2 (the half-band), out within kMinRate..kMaxStoredRate.
struct RecRates {
  static constexpr uint32_t kMaxStoredRate = 48000;   // the pre-roll ring's size
  uint32_t in = 44100;
  uint32_t out = kPoolRate;
  constexpr bool valid() const { return out >= kMinRate && out <= kMaxStoredRate && (in == out || in == 2 * out); }
  constexpr uint32_t factor() const { return in == out ? 1u : 2u; }   // input samples per stored sample
};
// What a platform stores for an input rate given its pool's rate: half the input
// when that is the pool's rate (the Cardputer: 44,100 -> 22,050), else the input rate
// itself (the Daisy: 48,000 -> 48,000).
constexpr RecRates recRatesFor(uint32_t inRate, uint32_t poolRate) {
  return RecRates{inRate, inRate == 2 * poolRate ? poolRate : inRate};
}

// A time in samples at `rate` (rounded down, as the recorder always counted).
constexpr uint32_t recSamples(uint32_t rate, uint32_t ms) { return rate * ms / 1000; }

// 2:1 decimation (44.1 kHz -> 22.05 kHz on the Cardputer): two samples in, one out.
class HalfBand {
 public:
  static constexpr int kTaps = 47;
  static constexpr int kUnique = 12;
  static const float kCoef[kUnique];   // at distance 1, 3, ..., 23 from the centre (which is 0.5)
  void reset();
  float run(float a, float b);

 private:
  static constexpr int kBuf = 64;
  float buf_[2 * kBuf];
  int pos_ = 0;
};

// One-pole DC blocker: y = x - x1 + R * y1.
struct DcBlock {
  static constexpr float kR = 0.995f;   // -3 dB at 17.5 Hz at 22,050 Hz
  float r = kR;
  float x1 = 0.0f, y1 = 0.0f;
  void reset() { x1 = y1 = 0.0f; }
  // The same corner at `rate` (exactly kR at 22,050 Hz).
  void setRate(uint32_t rate) { r = rate == 22050 ? kR : 1.0f - 110.25f / float(rate); }
  float run(float x) {
    const float y = x - x1 + r * y1;
    x1 = x;
    y1 = y;
    return y;
  }
};

enum class RecState : uint8_t {
  Off = 0,
  Settling,    // armed, listening to the room (no start yet)
  Waiting,     // waiting for the level to cross the threshold
  Recording,
  Done,        // released after a recording: length() samples are the sample
  Full,        // the slot filled up: length() = the maximum
  Cancelled,   // released before any sound (or too short): nothing to commit
  Failed,      // the sink refused a write
};
const char* recStateName(RecState s);   // "OFF", "SETTLING", ... (the report)

// Where the samples go (the device: SampleWriter::write on the flash; the desktop and
// the tests: RAM). Offsets in samples, in order.
class RecordSink {
 public:
  virtual bool write(uint32_t offset, const int16_t* data, uint32_t n) = 0;

 protected:
  ~RecordSink() = default;
};

class Recorder {
 public:
  // The Cardputer's rates (the defaults) and the counts at its 22,050 Hz; at another
  // stored rate the same milliseconds (preRollSamples(), tailSamples()).
  static constexpr uint32_t kInRate = 44100;
  static constexpr uint32_t kOutRate = kPoolRate;   // 22,050
  static constexpr uint32_t kSkipMs = 50, kSettleMs = 150, kWindowMs = 10;
  static constexpr uint32_t kPreRollMs = 40, kTailMs = 60;
  static constexpr uint32_t samplesIn(uint32_t rate, uint32_t ms) { return recSamples(rate, ms); }
  static constexpr uint32_t kSkip = recSamples(kOutRate, kSkipMs);           // 1102
  static constexpr uint32_t kSettle = recSamples(kOutRate, kSettleMs);       // 3307
  static constexpr uint32_t kWindow = recSamples(kOutRate, kWindowMs);       // 220
  static constexpr uint32_t kPreRoll = recSamples(kOutRate, kPreRollMs);     // 882
  static constexpr uint32_t kTail = recSamples(kOutRate, kTailMs);           // 1323
  static constexpr uint16_t kThrMin = 300;    // -40.8 dBFS: a quiet room
  static constexpr uint16_t kThrMax = 3300;   // -19.9 dBFS: a loud one (the groove on the speaker)
  static constexpr float kThrOverFloor = 10.0f;   // +20 dB over the floor's RMS
  static constexpr int kStage = 256;          // samples per sink write (512 bytes)
  static constexpr int kRing = 2048;          // the pre-roll ring (power of two >= the pre-roll at kMaxStoredRate)
  static constexpr int kLevelEvery = 1024;    // the meter's window (46 ms; scaled at other stored rates)
  // The most a recording may be asked to hold (a platform's slot; the Cardputer's
  // kSlotMaxSamples is the default).
  static constexpr uint32_t kMaxSamples = 1u << 28;
  static_assert(recSamples(RecRates::kMaxStoredRate, kPreRollMs) <= uint32_t(kRing), "pre-roll ring too small");

  // Starts listening (any state; everything is reset). maxSamples <= kMaxSamples.
  // rates must be valid() (else the recorder is Failed at once).
  void start(RecordSink* sink, uint32_t maxSamples = kSlotMaxSamples, RecRates rates = RecRates{});
  // DIRECT mode (resampling, resampler.h): a digital source that has no room, no key
  // click and no DC. Recording at once: the half-band only (no DC block, no
  // threshold, no pre-roll, no tail), the first `skip` decimated samples dropped (the
  // filter's warm-up: the source sends the history it needs in front). It fills up
  // as start()'s does (Full); the source ends it with end().
  void startDirect(RecordSink* sink, uint32_t maxSamples, uint32_t skip, RecRates rates = RecRates{});
  // Writer thread, direct mode: flushes and ends as `s` (Done / Full with the
  // sample's length `len`, at most what was recorded; shorter than kMinSamples is
  // Cancelled; Cancelled / Failed as they are). A recorder that Failed stays so.
  void end(RecState s, uint32_t len);
  // Writer thread: input at rates().in. Does nothing outside Settling / Waiting / Recording.
  void feed(const int16_t* in, int n);
  // Any thread: the key went up. The writer thread then calls finish().
  void release() { released_.store(true, std::memory_order_release); }
  bool released() const { return released_.load(std::memory_order_acquire); }
  // Writer thread, after release() (and whatever input was there): flushes and ends.
  void finish();
  // Any thread.
  RecState state() const { return RecState(state_.load(std::memory_order_acquire)); }
  bool ended() const;   // Done, Full, Cancelled or Failed
  uint32_t recorded() const { return recorded_.load(std::memory_order_acquire); }   // samples so far
  uint32_t length() const { return final_; }   // the sample's length (Done / Full), after ended()
  uint16_t level() const { return level_.load(std::memory_order_relaxed); }   // input peak, last ~46 ms
  uint16_t threshold() const { return thr_.load(std::memory_order_relaxed); }  // 0 while settling
  uint16_t floorRms() const { return floor_; }
  uint16_t peak() const { return peak_.load(std::memory_order_relaxed); }      // of what was recorded
  // The growing picture: per kOverviewCols column of a FULL slot (kSlotMaxSamples / 232
  // samples each), min / max of the recorded samples; columns 0..colsUsed()-1 are final.
  const int16_t (*cols() const)[2] { return cols_; }
  int colsUsed() const { return colsUsed_.load(std::memory_order_acquire); }
  uint32_t sinkWrites() const { return writes_; }
  uint32_t sinkErrors() const { return errors_; }
  // This recording's rates, and its times in stored samples.
  RecRates rates() const { return rates_; }
  uint32_t preRollSamples() const { return preRoll_; }
  uint32_t tailSamples() const { return tail_; }

 private:
  void out(float y);
  void put(int16_t v);
  bool flush();

  RecordSink* sink_ = nullptr;
  uint32_t max_ = kSlotMaxSamples;
  RecRates rates_{};
  uint32_t skipN_ = kSkip, settle_ = kSettle, window_ = kWindow, preRoll_ = kPreRoll, tail_ = kTail;
  int levelEvery_ = kLevelEvery;
  HalfBand hb_;
  DcBlock dc_;
  bool direct_ = false;
  uint32_t skip_ = 0;
  bool havePend_ = false;
  float pend_ = 0.0f;
  uint32_t n_ = 0;   // output samples seen since start
  // floor
  float winSum_ = 0.0f;
  uint32_t winN_ = 0;
  float floorMin_ = -1.0f;
  uint16_t floor_ = 0;
  // pre-roll
  int16_t ring_[kRing];
  uint32_t ringN_ = 0;
  // staging
  int16_t stage_[kStage];
  int stageN_ = 0;
  uint32_t flushed_ = 0;   // samples handed to the sink
  uint32_t final_ = 0;
  uint32_t writes_ = 0, errors_ = 0;
  // meter
  int levelAcc_ = 0, levelN_ = 0;
  int16_t cols_[kOverviewCols][2];
  int colCur_ = -1;
  std::atomic<uint8_t> state_{0};
  std::atomic<bool> released_{false};
  std::atomic<uint32_t> recorded_{0};
  std::atomic<uint16_t> level_{0}, thr_{0}, peak_{0};
  std::atomic<int> colsUsed_{0};
};

// What the screen shows while a recording runs (MicInput::poll).
struct RecView {
  RecState state = RecState::Off;
  int slot = 0;
  uint32_t samples = 0;        // recorded so far (at `rate`); the sample's length once Done / Full
  uint32_t rate = kPoolRate;   // the stored rate, Hz: what `samples` and barAt count in
  uint32_t capacity = kSlotMaxSamples;   // the slot's size in samples (the picture's width)
  uint16_t level = 0;          // input peak over the last ~46 ms (0..32767)
  uint16_t threshold = 0;      // where the recording starts (0 while settling)
  uint16_t peak = 0;           // of the recording so far
  const int16_t (*cols)[2] = nullptr;   // the growing picture (Recorder::cols)
  int colsUsed = 0;
  const char* why = "";        // Failed: what failed (a short upper-case word)
  // Resampling (resampler.h; SHIFT+L): the source is the engine's output. sync: in
  // time with the groove (Waiting = counting to the bar line); barAt[0..bars-1]: the
  // bar lines inside the recording so far, in samples from its start.
  bool resample = false;
  bool sync = false;
  int bars = 0;
  const uint32_t* barAt = nullptr;
};

// The microphone as the UI sees it (Ui::setMic). UI thread only. The UI's gesture
// (UX.md 10.3 "Recording"): hold L on a sampler instrument's page. Resampling
// (Ui::setResample, SHIFT+L) is the same interface over the engine's output
// (resampler.h): its view sets RecView::resample and has no threshold.
class MicInput {
 public:
  // Can it record now? why: a short upper-case reason for a toast otherwise.
  virtual bool ready(const char** why) = 0;
  // Every byte of the slot is erased (the device; elsewhere always true): recording
  // can start at once, also while the transport plays.
  virtual bool slotErased(int slot) = 0;
  // Empties the slot and erases all of it. Blocking (the device: about 1-2 s with the
  // output faded out and the audio stalled; the UI shows ERASING first).
  virtual bool erase(int slot) = 0;
  // Starts listening for slot `slot` (it must be erased). false: why in poll().
  virtual bool start(int slot) = 0;
  // The key went up.
  virtual void release() = 0;
  // Every UI frame while busy: the state and the picture. When the recording ends
  // (Done / Full) the platform commits it here (the UI thread); the call that
  // reports Done / Full / Cancelled / Failed is the last one of the recording (the
  // next reports Off). Returns false when Off.
  virtual bool poll(RecView* v) = 0;

 protected:
  ~MicInput() = default;
};

}  // namespace sampler
}  // namespace pg
