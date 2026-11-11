// Interface every sound source implements. One Voice object per track; the
// lead track handles its own polyphony inside its Voice.
//
// RULES for everything called from the audio thread (all methods except init):
//   * no heap allocation, no locks, no I/O, no exceptions
//   * single-precision float only (the ESP32-S3 FPU has no double; a stray
//     double literal or std::sin(double) costs ~50x). Host build uses
//     -Wdouble-promotion -Wfloat-conversion to catch this.
//   * no per-sample libm calls (sin/exp/pow/tanh): use tables or polynomial
//     approximations, and recompute coefficients once per quantum at most.
#pragma once
#include "params.h"

namespace pg {

struct NoteEvent {
  uint8_t note;     // MIDI note (drums: 60 = natural tuning)
  uint8_t vel;      // 1..127
  bool    accent;
  bool    slide;    // legato: glide from the currently sounding note, don't retrigger envelopes
  uint8_t chord;    // ChordType, lead only
};

class Voice {
 public:
  virtual ~Voice() = default;

  // Called once, not on the audio thread. May precompute tables.
  virtual void init(float sampleRate) = 0;

  // Sound-param metadata for index 0..kSoundParams-1; nullptr for unused slots.
  // Used slots must be contiguous from 0. Order them most-important-first:
  // the UI shows them in pages of 8.
  virtual const ParamInfo* paramInfo(int index) const = 0;

  // Called at the start of every quantum with the track's 24 effective sound
  // params (p-locks already applied). Must be cheap when nothing changed.
  virtual void setParams(const uint8_t* soundParams) = 0;

  virtual void setTempo(float /*bpm*/) {}

  virtual void noteOn(const NoteEvent& e) = 0;
  // End of gate for `note` (for chords: the root passed to noteOn). One-shot drums may ignore.
  virtual void noteOff(uint8_t note) = 0;
  // Hard stop with a short (~2 ms) fade: transport stop, hat choke.
  virtual void choke() = 0;

  // Write (not add) `frames` samples of stereo audio at nominal +-1.0 full
  // scale. frames is always kQuantum. Mono voices write the same signal to
  // both. Only called while active() is true.
  virtual void render(float* outL, float* outR, int frames) = 0;

  // False when the voice is silent, so the engine can skip it.
  virtual bool active() const = 0;

  // True if render() always writes identical L and R, so the mixer may read
  // only outL. (Drums and bass: true.)
  virtual bool mono() const { return false; }

  // The device's codec is mono. When set, a stereo voice should render its
  // mono sum to both channels as cheaply as it can. Called before audio starts.
  virtual void setMonoOutput(bool /*mono*/) {}
};

}  // namespace pg
