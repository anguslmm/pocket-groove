// SamplerVoice: plays a sample pool slot (sample_pool.h) as an instrument, for
// "simple processed vocal chops". The PERC track hosts one besides its native
// percussion voice (voice.h VOICE KINDS; an instrument whose set says
// `voice perc sampler` plays through it). Parameters: sampler_params.h.
//
// One chop at a time (monophonic per track). A note-on latches the slot, the
// slice and the region and starts the chop at its first sample (its last with
// REVERSE, or with the tape running backwards); the gate's end starts the release.
// A new note while one sounds: the old one's last output value becomes an offset
// that fades out over kCutMs (the same de-click the drums use, drums.h), so the
// cut is continuous; the new chop starts from silence with at least a 1 ms attack.
// The region's end (and a loop's wrap) is de-clicked the same way.
//
// Playback: linear interpolation from the int16 data (memory-mapped flash on the
// device), position in 32.32 fixed point, at
//   (sample rate / output rate) * 2^((note - 60 + PITCH + FINE + repeats * RPIT) / 12) * SPEED
// clamped to 4 octaves either way. Never reads outside [region start, region end)
// within the sample: every read index is checked against the region, and the
// region against the sample, before the read.
//
// LATCHED at note-on (they would click if they moved under a sounding chop):
// SLICES, SLICE, START, REVERSE, ATTACK, LOOP, the slot. CONTINUOUS (follow a
// macro while the chop sounds, also through setParamsFine, no libm there):
// LENGTH (the region's end moves), PITCH, FINE, SPEED, RELEASE, the filter,
// BITS, DOWN, DRIVE, RPIT, RFAD; RTRG takes its new rate at the next repeat.
//
// Processing per sample: read -> envelope x velocity -> sample-and-hold (DOWN)
// -> bit crush (BITS) -> drive -> state-variable filter (LP / BP / HP) -> out.
#pragma once
#include <atomic>
#include <cstdint>

#include "../voice.h"
#include "sample_pool.h"
#include "sampler_params.h"

namespace pg {
namespace sampler {

class SamplerVoice final : public Voice {
 public:
  SamplerVoice();

  // Not real-time: the pool every note resolves its slot through (nullptr: the
  // built-in sounds only). Set before audio starts or while the engine is suspended.
  void setPool(const SamplePool* pool) { pool_ = pool; }
  const SamplePool* pool() const { return pool_; }

  void init(float sampleRate) override;
  const ParamInfo* paramInfo(int index) const override { return sampler::paramInfo(index); }
  void setParams(const uint8_t* soundParams) override;
  void setParamsFine(const float* soundParams) override;
  void setTempo(float bpm) override;
  void setSampleSlot(uint8_t slot) override { slot_ = slot < kPoolSlots ? slot : 0; }
  void noteOn(const NoteEvent& e) override;
  void noteOff(uint8_t note) override;
  void choke() override;
  void render(float* outL, float* outR, int frames) override;
  bool active() const override { return active_; }
  bool mono() const override { return true; }

  // For the screen (any thread): where the chop is, in samples of its slot's
  // sample; -1 when silent. Updated once per quantum.
  int32_t playhead() const { return playhead_.load(std::memory_order_relaxed); }
  // The slot the sounding (or last) chop came from.
  uint8_t playingSlot() const { return chopSlot_; }

  // The region a chop with these parameters plays in a sample of `length`
  // samples: [*a, *b), b - a >= min(kMinRegion, length). Shared with the screen.
  static void region(const uint8_t* p, uint32_t length, uint32_t sampleRate, int32_t* a, int32_t* b);

  static constexpr int kMinRegion = 64;
  static constexpr float kCutMs = 3.0f;
  static constexpr float kLevel = 0.7f;   // output at a full-scale sample peak (before velocity)

 private:
  void startChop(bool repeat);
  void updateRegion();
  void updateRate();
  void updateTone();
  bool edge();             // the read index left the region: wrap (loop) or end; false = ended
  void endChop();

  const SamplePool* pool_ = nullptr;
  float fs_ = 44100.0f;
  float stepSamples_ = 5512.5f;   // one 16th at the tempo

  // parameters
  uint8_t raw_[kNumParams];
  float pf_[kNumParams];
  bool fine_ = false;
  uint8_t slot_ = 0;

  // the chop (latched)
  SampleRef ref_;
  uint32_t gen_ = 0;
  uint8_t chopSlot_ = 0;
  int32_t sliceA_ = 0, sliceB_ = 0, startPos_ = 0;   // the slice and the chop's start
  int32_t a_ = 0, b_ = 2;          // region [a_, b_)
  int dir_ = 1;                    // +1 forward, -1 backward (REVERSE x the tape's sign at the start)
  bool loop_ = false;
  int64_t pos_ = 0;                // 32.32
  int64_t inc_ = 0;                // 32.32, signed (the tape runs backwards below SPEED 64)
  float note_ = 0.0f;              // note - 60, semitones
  float velGain_ = 0.0f;
  float tapeGain_ = 1.0f;          // fades the tape out as it stops

  // envelope
  enum Stage : uint8_t { kOff, kAttack, kHold, kRelease };
  Stage stage_ = kOff;
  float env_ = 0.0f, attackInc_ = 1.0f, relCoef_ = 0.999f;
  bool gate_ = false;
  bool choking_ = false;
  float chokeGain_ = 1.0f, chokeStep_ = 0.01f;

  // repeats (RTRG)
  int32_t rtLeft_ = 0;             // samples until the next repeat (0: none due)
  int repeats_ = 0;
  float repGain_ = 1.0f;

  // de-click offset
  float resid_ = 0.0f, residRamp_ = 0.0f, residStep_ = 0.01f, lastOut_ = 0.0f;

  // tone
  float holdPh_ = 0.0f, holdInc_ = 1.0f, holdVal_ = 0.0f;
  bool down_ = false;
  float crushQ_ = 0.0f, crushInv_ = 0.0f;   // q = 0: no crush
  float drive_ = 1.0f, driveNorm_ = 1.0f;
  bool driveOn_ = false;
  float g_ = 0.1f, gk_ = 1.1f, d_ = 0.9f, s1_ = 0.0f, s2_ = 0.0f;
  uint8_t ftype_ = kLP;
  bool filterOn_ = false;

  bool active_ = false;
  std::atomic<int32_t> playhead_{-1};
};

}  // namespace sampler
}  // namespace pg
