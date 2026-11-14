// MelodicSampler: a sample pool slot played from the keyboard, in the LEAD slot
// (a `voice lead sampler` instrument; voice.h VOICE KINDS). The lead track hosts
// one besides its synth (Instrument), and the engine switches to it at the
// quantum edge where a sampler instrument is selected. Parameters:
// sampler_params.h namespace mel. docs/MACROLAB.md "The melodic sampler".
//
// PARAPHONIC. Up to kMaxNotes (4) notes at once, each with its own playhead,
// pitch (with glide) and amplitude envelope (ADSR), summed into ONE shared tone
// chain: sample-and-hold (DOWN) -> bit crush (BITS) -> drive -> state-variable
// filter (LP / BP / HP, with a filter envelope that the newest note retriggers)
// -> out. A chord expands here (lead::LeadVoice::chordIntervals, as the synth
// lead does); a chord type with more notes than the cap drops its highest ones
// (none of today's types has more than four).
//
// PITCH. The step's note transposes the sample: note mel::kRootNote (48, C at the
// lead's default octave 3 and root C, the first note key) plays it as recorded;
// PITCH (coarse, semitones) and FINE (+-1 semitone) reach any root. Playback is
// linear interpolation from the int16 data (memory-mapped flash on the device),
// position in 32.32 fixed point, at
//   (sample rate / output rate) * 2^((note - 48 + PITCH + FINE + glide + drift) / 12)
// clamped to 4 octaves either way. Never reads outside the sample: the window is
// always inside it, the playhead never jumps, and every read index is checked.
//
// HELD NOTES. LOOP = ONE: from START (latched) to the window's end, then the note
// ends (de-clicked). LOOP = PONG: the playhead runs forward and backward over the
// window [START, START + LENGTH] for as long as the note sounds, and the release
// ends it: a turn keeps the waveform continuous (no click) and nothing crossfades
// (no level dip). The window follows START and LENGTH continuously: the playhead
// is never moved; when the window moves away from it, it turns toward it and
// travels there at the note's speed (a scan through the sample).
//
// SLIDE (NoteEvent::slide, legato): the notes of the previous note / chord glide
// to the new pitches (GLIDE) without restarting their playheads or envelopes, as
// the synth lead glides. A note whose gate had ended is gated again from where
// its envelope is.
//
// STEALING. A fifth note takes the quietest released note, else the oldest; the
// stolen note's last output becomes an offset that decays within ~3 ms (the
// drums' de-click), so the new note starts on time and nothing jumps. The
// one-shot's end is de-clicked the same way; choke() fades every note in 2 ms.
//
// LATCHED at a note's start: the slot, START (where it starts), LOOP, REVERSE.
// CONTINUOUS (control rate, also through setParamsFine, no libm): the rest.
#pragma once
#include <atomic>
#include <cstdint>

#include "../voice.h"
#include "sample_pool.h"
#include "sampler.h"
#include "sampler_params.h"

namespace pg {
namespace sampler {

class MelodicSampler final : public Voice {
 public:
  MelodicSampler();

  // Not real-time: the pool every note resolves its slot through (nullptr: the
  // built-in sounds only). Set before audio starts or while the engine is suspended.
  void setPool(const SamplePool* pool) { pool_ = pool; }
  const SamplePool* pool() const { return pool_; }
  // RESAMPLING (UX.md 10.3 "Resampling"): the slot being recorded plays nothing on
  // any sampler. The PERC sampler's silent slot (SamplerVoice::setSilentSlot, which
  // the recorders set) also silences it here when linked (Instrument links them);
  // setSilentSlot sets this voice's own. Any thread.
  void followSilent(const SamplerVoice* v) { silentFrom_ = v; }
  void setSilentSlot(int slot) { silent_.store(int8_t(slot >= 0 && slot < kPoolSlots ? slot : -1), std::memory_order_relaxed); }
  int silentSlot() const {
    const int own = silent_.load(std::memory_order_relaxed);
    return own >= 0 ? own : (silentFrom_ ? silentFrom_->silentSlot() : -1);
  }

  void init(float sampleRate) override;
  const ParamInfo* paramInfo(int index) const override { return mel::paramInfo(index); }
  void setParams(const uint8_t* soundParams) override;
  void setParamsFine(const float* soundParams) override;
  void setSampleSlot(uint8_t slot) override { slot_ = slot < kPoolSlots ? slot : 0; }
  void noteOn(const NoteEvent& e) override;
  void noteOff(uint8_t note) override;
  void choke() override;
  void render(float* outL, float* outR, int frames) override;
  bool active() const override { return active_; }
  bool mono() const override { return true; }

  // For the screen (any thread): note k's playhead in samples of its slot, and the
  // slot, packed as slot << 24 | position; -1 when note k is silent. Once per quantum.
  int32_t playhead(int k) const {
    return k >= 0 && k < mel::kMaxNotes ? playhead_[k].load(std::memory_order_relaxed) : -1;
  }
  static int playheadPos(int32_t packed) { return packed < 0 ? -1 : int(packed & 0xFFFFFF); }
  static int playheadSlot(int32_t packed) { return packed < 0 ? -1 : int(packed >> 24); }

  // The window a note with these parameters plays in a sample of `length`
  // samples: [*a, *b), b - a >= min(the window's minimum, length). Shared with the screen.
  static void region(const uint8_t* p, uint32_t length, uint32_t sampleRate, int32_t* a, int32_t* b);

  // ---- introspection for tests ----
  struct NoteView {
    bool on, gate;
    uint8_t note, slot;
    float semis;      // the note's pitch now (glide included), semitones from kRootNote
    float env;
    int32_t pos;      // playhead, samples
    int dir;          // +1 forward, -1 backward
  };
  NoteView view(int k) const;
  int soundingNotes() const;

  static constexpr float kLevel = 0.40f;    // a single note's output at a full-scale sample peak (before velocity)
  static constexpr float kMinWindowMs = 20.0f;

 private:
  enum Stage : uint8_t { sOff = 0, sAttack, sDecay, sRelease, sKill };
  struct Note {
    const int16_t* data;
    uint32_t len;          // samples
    uint32_t gen;          // the pool's generation at the start
    uint32_t rate;         // the sample's rate
    float rateRatio;       // sample rate / output rate
    float gain;            // velocity x chord x slot gain x kLevel / 32768
    int64_t pos;           // 32.32
    int64_t step;          // 32.32, signed: the direction it runs
    int32_t a, b;          // the window [a, b)
    int32_t startPos;      // the latched start (ONE: the window's start)
    float semis, target;   // pitch (from kRootNote), its glide target
    float env;             // amplitude envelope at the start of the next quantum
    float killStep;
    float last;            // the last output contribution (a steal's offset)
    uint32_t group, age;
    uint8_t stage, note, root, slot;
    bool gate, loop, reverse, builtin;
  };

  void updateTone();
  void startNote(Note& n, uint8_t note, uint8_t root, float gain, uint32_t group);
  int allocNote(uint8_t note, uint32_t claimed) const;
  void releaseNote(Note& n);
  void killNote(Note& n);
  void window(const Note& n, int32_t* a, int32_t* b) const;
  void renderNote(Note& n, float* acc);
  void publish();

  const SamplePool* pool_ = nullptr;
  const SamplerVoice* silentFrom_ = nullptr;
  float fs_ = 44100.0f;
  float invFs_ = 1.0f / 44100.0f;

  // parameters
  uint8_t raw_[mel::kNumParams];
  float pf_[mel::kNumParams];
  bool fine_ = false;
  uint8_t slot_ = 0;

  // derived (updateTone)
  float atkStepQ_ = 1.0f, decCoefQ_ = 0.9f, sus_ = 1.0f, relCoefQ_ = 0.9f;
  float glideCoef_ = 1.0f, fdecCoefQ_ = 0.9f, envOct_ = 0.0f;
  float tune_ = 0.0f;                 // PITCH + FINE, semitones
  float cutOct_ = 7.45f, k_ = 1.0f / 0.6f;
  float driftDepth_ = 0.0f, driftInc_ = 0.0f;
  uint8_t ftype_ = kLP;
  bool filterOn_ = false, toneDirty_ = true;

  // shared state
  Note n_[mel::kMaxNotes];
  uint32_t groupCounter_ = 0, ageCounter_ = 0, lastGroup_ = 0;
  float fenv_ = 0.0f;                 // filter envelope 0..1
  float g_ = 0.1f, gk_ = 1.1f, d_ = 0.9f, s1_ = 0.0f, s2_ = 0.0f;
  float holdPh_ = 0.0f, holdInc_ = 1.0f, holdVal_ = 0.0f;
  bool down_ = false;
  float crushQ_ = 0.0f, crushInv_ = 0.0f;
  float drive_ = 1.0f, driveNorm_ = 1.0f;
  bool driveOn_ = false;
  float resid_ = 0.0f, residK_ = 0.99f;   // the de-click offset, decaying per sample
  float residNext_ = 0.0f;                // offsets of notes that ended in this quantum (from the next on)
  float driftPh_ = 0.0f, driftWander_ = 0.0f, driftTarget_ = 0.0f, drift_ = 0.0f;
  uint32_t rng_ = 0x9E3779B9u;
  int killQuanta_ = 6;
  bool active_ = false;
  std::atomic<int8_t> silent_{-1};
  std::atomic<int32_t> playhead_[mel::kMaxNotes];
};

}  // namespace sampler
}  // namespace pg
