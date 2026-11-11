// The saved state of the instrument. Every field is a uint8_t, so there is no
// padding or endianness: a Project can be memcpy'd, diffed and serialised as bytes.
//
// THREADING: the audio thread is the only writer once the engine is running
// (edits arrive as Commands, see engine.h). The UI thread reads this memory
// directly to draw. Those reads race with engine writes by design; every field
// is one byte so a read sees either the old or the new value. Don't add
// multi-byte fields or cross-field invariants the UI depends on.
#pragma once
#include "config.h"

namespace pg {

enum StepFlags : uint8_t {
  kStepOn      = 0x01,
  kStepAccent  = 0x02,
  kStepSlide   = 0x04,  // synth tracks: glide into the NEXT step's note, no retrigger (303 style)
  kStepRatchetShift = 4,
  kStepRatchetMask  = 0x30,  // 0..3 => 1..4 evenly spaced hits inside the step
};

struct Step {
  uint8_t note;       // MIDI note. Synths: pitch. Drums: 60 = natural tuning, others transpose in semitones.
  uint8_t flags;      // StepFlags
  uint8_t vel;        // 1..127
  uint8_t gate;       // length in quarter-steps (4 = one step). One-shot drums ignore it.
  uint8_t prob;       // 0..100 percent chance the step fires
  uint8_t chord;      // lead track only: ChordType index, 0 = single note
  uint8_t lockParam;  // parameter lock: 0 = none, else track-param index + 1 (0..31 => 1..32)
  uint8_t lockValue;  // value for the locked param on this step only
};
static_assert(sizeof(Step) == 8, "Step must stay 8 bytes");

inline int stepRatchet(const Step& s) { return ((s.flags & kStepRatchetMask) >> kStepRatchetShift) + 1; }
constexpr Step kDefaultStep = {60, 0, 100, 2, 100, 0, 0, 0};

struct TrackPattern {
  uint8_t length;       // 1..kMaxSteps; tracks may differ (polymeter)
  uint8_t reserved[7];
  Step    steps[kMaxSteps];
};

struct Pattern {
  uint8_t length;       // master length in steps: pattern changes / song advance happen on this boundary
  uint8_t reserved[7];
  TrackPattern tracks[kNumTracks];
};

struct TrackSettings {
  uint8_t params[kParamsPerTrack];  // see params.h; 0..23 voice sound params, 24..31 mixer
  uint8_t mute;
  uint8_t reserved[7];
};

struct SongSlot {
  uint8_t pattern;   // 0..kNumPatterns-1
  uint8_t repeats;   // 0 = end of song (loops back to slot 0)
};

struct Project {
  uint8_t magic[4];     // 'P','G','R','V'
  uint8_t version;      // kProjectVersion
  uint8_t songMode;     // 0 = loop current pattern, 1 = follow song[]
  uint8_t reserved[10];
  char    name[16];     // NUL padded
  uint8_t globals[kNumGlobals];        // GlobalParam ids, see params.h
  TrackSettings tracks[kNumTracks];
  SongSlot song[kSongSlots];
  Pattern patterns[kNumPatterns];
};
constexpr uint8_t kProjectVersion = 1;
static_assert(sizeof(Project) < 72 * 1024, "Project must fit the RAM budget");

}  // namespace pg
