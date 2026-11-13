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
  uint8_t lockParam;  // lock: 0 = none; 1..32 = track param index + 1 (0..31);
                      //   33..36 = macro 0..3 of the track's instrument (kLockMacro0 + m)
  uint8_t lockValue;  // value for the locked param (or macro position 0..255) on this step only
};
static_assert(sizeof(Step) == 8, "Step must stay 8 bytes");

constexpr int kTrackMacros = 4;                          // macros per instrument (macro::kMaxMacros)
constexpr uint8_t kLockMacro0 = kParamsPerTrack + 1;     // Step::lockParam of macro 0
inline bool isMacroLock(uint8_t lockParam) { return lockParam >= kLockMacro0 && lockParam < kLockMacro0 + kTrackMacros; }

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

// The instrument (version 2): a macro set (docs/MACROLAB.md) named by its
// 16-bit id (macro::setId; 0 = none: CLASSIC, the stored sound parameters play
// as they are) and its four macro positions, 0..255. With an instrument the
// engine DERIVES params[0..23] from the set at those positions whenever either
// changes (they are still stored, so a project whose instrument this firmware
// does not know plays the sound it was saved with). params[24..31] stay the
// player's mixer values (the instrument's level and sends combine with them,
// macro::combineMixer).
struct TrackSettings {
  uint8_t params[kParamsPerTrack];  // see params.h; 0..23 voice sound params, 24..31 mixer
  uint8_t mute;
  uint8_t instrument[2];            // id, little-endian (two bytes: see THREADING above)
  uint8_t macro[kTrackMacros];      // macro positions 0..255
  uint8_t reserved[1];
};
inline uint16_t instrumentId(const TrackSettings& ts) { return uint16_t(ts.instrument[0] | (ts.instrument[1] << 8)); }
inline void setInstrumentId(TrackSettings& ts, uint16_t id) {
  ts.instrument[0] = uint8_t(id & 0xFF);
  ts.instrument[1] = uint8_t(id >> 8);
}

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
// 1: no instruments (the bytes after mute were reserved, zero). 2: TrackSettings
// carries the instrument. A version-1 project loads with no instrument on any
// track (CLASSIC) and sounds exactly as before.
constexpr uint8_t kProjectVersion = 2;
constexpr uint8_t kProjectVersionMin = 1;
static_assert(sizeof(TrackSettings) == 40, "TrackSettings must not grow");
static_assert(sizeof(Project) < 72 * 1024, "Project must fit the RAM budget");

}  // namespace pg
