// The on-device benchmark of the audio path (">bench", tools/pg-daisy bench;
// docs/DAISY.md "Audio load"): the engine's cost in CPU cycles per frame, scene by
// scene, measured the way the audio interrupt sees it.
//
// HOW IT RUNS. Inside the audio interrupt, one bench block per callback, with the
// live output muted (the callback writes silence while the bench runs): the same
// code, the same optimisation, the same memory placement, the same interrupt
// context (FPSCR, stack in DTCM) and the same 2 ms rhythm as real playing, so the
// caches hold what they hold between two real blocks. What differs, and is said in
// the output: the main loop does only the serial port meanwhile (the UI, the keys,
// the pots, the flash writes are paused: the UI must not see or save the bench's
// projects), so the caches are a little warmer than with the UI running; the "cold"
// column bounds that from the other side (both caches emptied before every block).
// The bench replaces the instrument's project and voices, so the App saves the
// session first and restarts the board at the end (it comes back as after a power
// cycle).
//
// SCENES (deterministic: Instrument::init reseeds the engine, the demos and the busy
// pattern are built in code): idle (the engine stopped), each ROM demo's song, and
// the busy pattern of host/tools/bench_instrument (host/common/grooves.h
// programBusy: all 8 tracks, ratchets, p-locks, every send, the master's filter and
// drive), at 48 kHz stereo through Instrument::processFloat, as the firmware plays.
// Per scene, after kWarm blocks (2 s: the FX fill, every voice has sounded, the
// same warm-up as bench_instrument's):
//   pass 1  kMeasure blocks timed one by one (cycles per block: min, average, max),
//           and the output stage (App's level and clamp) timed apart
//   pass 2  kCold more blocks, each with the I- and D-cache emptied first (the
//           platform's flushCaches; outside the timed span)
//   pass 3  the scene again from the start with every voice wrapped in a timer
//           (TimedVoice: render, note and parameter calls), kSplit blocks after the
//           warm-up: drums, bass, lead, sampler; the engine's own share (sequencer,
//           mixer, FX, master) is the rest. The wrappers cost a little (two cycle
//           reads per call): the split is given in percent of its own pass.
// The host's count for the same scene (x86 instructions per frame, callgrind, the
// same blocks: bench_ref.h, host/daisy/daisy_bench_ref.cpp) gives the ratio cycles
// per x86 instruction that docs/DAISY.md section 11 needed to guess.
//
// Portable: the App drives it (App::serviceBench, App::render) and the host tests
// run it in daisy_sim; benchSetup() is also what the host's reference count runs.
#pragma once
#include <cstdint>

#include "pg/instrument/instrument.h"

namespace pgd {

// The scenes: 0 idle, 1..demos, then busy.
int benchScenes();
const char* benchSceneName(int scene);
// Re-initialises inst at sampleRate, stereo, and sets the scene up (the transport
// posted Play unless idle). Not real-time: the engine must not run meanwhile.
void benchSetup(pg::Instrument& inst, int scene, float sampleRate);
// Blocks of the bench: the warm-up and the measured span, as the host reference uses.
constexpr int kBenchWarm = 1000, kBenchMeasure = 2000, kBenchCold = 250, kBenchSplit = 1000;
constexpr int kBenchQuickWarm = 50, kBenchQuickMeasure = 100, kBenchQuickCold = 20, kBenchQuickSplit = 100;

// One voice behind a timer (pass 3).
class TimedVoice final : public pg::Voice {
 public:
  using Clock = uint32_t (*)(void*);
  void wrap(pg::Voice* v, uint64_t* acc, Clock clock, void* user, const volatile bool* on) {
    v_ = v;
    acc_ = acc;
    clock_ = clock;
    user_ = user;
    on_ = on;
  }
  void init(float sr) override { v_->init(sr); }
  const pg::ParamInfo* paramInfo(int i) const override { return v_->paramInfo(i); }
  void setParams(const uint8_t* p) override {
    const uint32_t t = clock_(user_);
    v_->setParams(p);
    add(t);
  }
  void setParamsFine(const float* p) override {
    const uint32_t t = clock_(user_);
    v_->setParamsFine(p);
    add(t);
  }
  void setTempo(float bpm) override { v_->setTempo(bpm); }
  void setTune(float st) override { v_->setTune(st); }
  void setSampleSlot(uint8_t s) override { v_->setSampleSlot(s); }
  void noteOn(const pg::NoteEvent& e) override {
    const uint32_t t = clock_(user_);
    v_->noteOn(e);
    add(t);
  }
  void noteOff(uint8_t n) override {
    const uint32_t t = clock_(user_);
    v_->noteOff(n);
    add(t);
  }
  void choke() override {
    const uint32_t t = clock_(user_);
    v_->choke();
    add(t);
  }
  void render(float* l, float* r, int frames) override {
    const uint32_t t = clock_(user_);
    v_->render(l, r, frames);
    add(t);
  }
  bool active() const override { return v_->active(); }
  bool mono() const override { return v_->mono(); }
  void setMonoOutput(bool m) override { v_->setMonoOutput(m); }

 private:
  void add(uint32_t t0) {
    if (*on_) *acc_ += clock_(user_) - t0;
  }
  pg::Voice* v_ = nullptr;
  uint64_t* acc_ = nullptr;
  Clock clock_ = nullptr;
  void* user_ = nullptr;
  const volatile bool* on_ = nullptr;
};

class Bench {
 public:
  static constexpr int kMaxScenes = 8;
  enum Group : uint8_t { kDrums, kBass, kLead, kSampler, kGroups };
  struct Result {
    bool done;
    uint32_t blocks;                 // pass 1
    uint64_t sum;                    // cycles, pass 1
    uint32_t minC, maxC;             // cycles per block, pass 1
    uint64_t outSum;                 // the output stage's cycles, pass 1
    uint32_t coldBlocks;
    uint64_t coldSum;
    uint32_t coldMax;
    uint32_t splitBlocks;
    uint64_t splitSum;               // pass 3's whole blocks
    uint64_t group[kGroups];         // pass 3's voices
  };
  enum class Pass : uint8_t { None, Plain, Cold, Split };

  using Clock = uint32_t (*)(void*);
  using Flush = void (*)(void*);
  // Main loop. quick: short spans (the host tests); only: one scene, -1 all.
  void start(pg::Instrument* inst, float sampleRate, int frames, Clock clock, Flush flush, void* user, bool quick,
             int only);
  bool active() const { return phase_ != Phase::Off; }
  // Main loop, every pass while active(): sets up the next pass when the interrupt
  // has finished one. Returns true once everything is measured (then active() is
  // false and results() hold it all).
  bool step();
  // The audio interrupt: one bench block (out gets silence). false: not running.
  bool audioBlock(float* out, int frames);

  int scenes() const { return nScenes_; }
  int firstScene() const { return first_; }
  const Result& result(int scene) const { return res_[scene]; }
  int frames() const { return frames_; }
  int warmBlocks() const { return warm_; }
  int measureBlocks() const { return measure_; }
  int coldBlocks() const { return cold_; }
  int splitBlocks() const { return split_; }
  int scene() const { return scene_; }
  Pass pass() const { return pass_; }

 private:
  enum class Phase : uint8_t { Off, Setup, Run, Done };
  void setupPass();
  pg::Instrument* inst_ = nullptr;
  float rate_ = 48000.0f;
  int frames_ = 96;
  Clock clock_ = nullptr;
  Flush flush_ = nullptr;
  void* user_ = nullptr;
  int warm_ = 0, measure_ = 0, cold_ = 0, split_ = 0;
  int first_ = 0, nScenes_ = 0, last_ = 0;
  int scene_ = 0;
  Pass pass_ = Pass::None;
  volatile Phase phase_ = Phase::Off;
  volatile int block_ = 0;           // blocks run in this pass
  int passWarm_ = 0, passBlocks_ = 0;
  volatile bool timing_ = false;     // pass 3's voice timers count
  Result res_[kMaxScenes] = {};
  TimedVoice wrap_[pg::kNumTracks + 2];
  float scratch_[2 * 256] = {};
};

}  // namespace pgd
