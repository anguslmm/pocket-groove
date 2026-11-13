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

constexpr uint8_t kStepFlagMask = kStepOn | kStepAccent | kStepSlide | kStepRatchetMask;   // 0x37
constexpr int kTrackMacros = 4;                          // macros per instrument (macro::kMaxMacros)

// Per-note macro values (instrument tracks, "MOTION" in engine.h): a 6-bit code
// per macro, 0 = the note has no value of its own for that macro (follow),
// 1..63 = a position on 63 even levels from 0 to 255 (code 1 = 0, 32 = 128,
// 63 = 255). They are glide targets; the glide itself is continuous.
constexpr int kMacroCodeMax = 63;
constexpr uint8_t macroCodeToPos(int code) { return uint8_t(((code - 1) * 255 + 31) / 62); }
constexpr int macroPosToCode(int pos) { return 1 + (pos * 62 + 127) / 255; }

// One step: 8 bytes, packed (project version 3; docs/STATE.md "RAM WALL": the
// pattern data cannot grow). Read and write it through the accessors only:
//   byte 0 note       MIDI note 0..127. Synths: pitch. Drums: 60 = natural tuning, others transpose.
//   byte 1 flagsRaw   bits 0-2, 4-5: StepFlags (flags()); bits 3, 6, 7: chord bits 0, 1, 2
//   byte 2 vel        1..127
//   byte 3 gate       length in quarter-steps (4 = one step), 1..255. One-shot drums ignore it.
//   byte 4 probRaw    bits 0-6: probability 0..100 % (prob()); bit 7: chord bit 3
//   byte 5-7 ext      what the track kind gives the step (the kind is the track's, not the step's:
//                     instrumentId != 0 is an instrument track):
//                       CLASSIC track: the one parameter lock, ext[0] = param index + 1 (0 = none,
//                       1..32), ext[1] = its value (every track parameter is 0..127), ext[2] = 0.
//                       instrument track: four 6-bit macro codes (above), little-endian
//                       24-bit word, macro m in bits 6m..6m+5. No parameter locks.
// The chord (lead only) is a ChordType 0..9. Every older field survives a
// version 1 or 2 load losslessly (Engine::sanitizeProject repacks; a v2
// instrument track's macro lock becomes that macro's value on that note).
struct Step {
  uint8_t note;
  uint8_t flagsRaw;
  uint8_t vel;
  uint8_t gate;
  uint8_t probRaw;
  uint8_t ext[3];

  uint8_t flags() const { return uint8_t(flagsRaw & kStepFlagMask); }
  void setFlags(int f) { flagsRaw = uint8_t((flagsRaw & ~kStepFlagMask) | (f & kStepFlagMask)); }
  void addFlags(int f) { setFlags(flags() | f); }
  void clearFlags(int f) { setFlags(flags() & ~f); }
  void toggleFlags(int f) { setFlags(flags() ^ f); }
  bool on() const { return (flagsRaw & kStepOn) != 0; }

  uint8_t prob() const { return uint8_t(probRaw & 0x7F); }
  void setProb(int p) { probRaw = uint8_t((probRaw & 0x80) | (p & 0x7F)); }

  uint8_t chord() const {
    return uint8_t(((flagsRaw >> 3) & 1) | ((flagsRaw >> 5) & 6) | ((probRaw >> 4) & 8));
  }
  void setChord(int c) {
    flagsRaw = uint8_t((flagsRaw & kStepFlagMask) | ((c & 1) << 3) | ((c & 6) << 5));
    probRaw = uint8_t((probRaw & 0x7F) | ((c & 8) << 4));
  }

  // CLASSIC track: the parameter lock.
  uint8_t lockParam() const { return ext[0]; }   // 0 = none, else param index + 1
  uint8_t lockValue() const { return ext[1]; }
  void setLock(int param1, int value) { ext[0] = uint8_t(param1); ext[1] = uint8_t(value); ext[2] = 0; }
  void clearExt() { ext[0] = ext[1] = ext[2] = 0; }

  // Instrument track: the note's own macro values.
  uint32_t extWord() const { return uint32_t(ext[0]) | uint32_t(ext[1]) << 8 | uint32_t(ext[2]) << 16; }
  void setExtWord(uint32_t w) { ext[0] = uint8_t(w); ext[1] = uint8_t(w >> 8); ext[2] = uint8_t(w >> 16); }
  int macroCode(int m) const { return int((extWord() >> (6 * m)) & 63u); }
  void setMacroCode(int m, int code) {
    setExtWord((extWord() & ~(63u << (6 * m))) | (uint32_t(code & 63) << (6 * m)));
  }
  bool hasMacro(int m) const { return macroCode(m) != 0; }
  uint8_t macroValue(int m) const { return macroCodeToPos(macroCode(m) ? macroCode(m) : 1); }   // valid if hasMacro
  void setMacroValue(int m, int pos) { setMacroCode(m, macroPosToCode(pos < 0 ? 0 : (pos > 255 ? 255 : pos))); }
  void clearMacro(int m) { setMacroCode(m, 0); }
  bool hasExt() const { return (ext[0] | ext[1] | ext[2]) != 0; }   // a lock (CLASSIC) / any own value (instrument)
};
static_assert(sizeof(Step) == 8, "Step must stay 8 bytes");
static_assert(macroCodeToPos(1) == 0 && macroCodeToPos(32) == 128 && macroCodeToPos(63) == 255, "macro codes");
static_assert(macroPosToCode(0) == 1 && macroPosToCode(128) == 32 && macroPosToCode(255) == 63, "macro codes");

// Version 2's lock numbering, kept for loading old files: lockParam 33..36 was
// a lock of macro 0..3 of the track's instrument.
constexpr uint8_t kLockMacro0 = kParamsPerTrack + 1;
inline bool isMacroLock(uint8_t lockParam) { return lockParam >= kLockMacro0 && lockParam < kLockMacro0 + kTrackMacros; }

inline int stepRatchet(const Step& s) { return ((s.flagsRaw & kStepRatchetMask) >> kStepRatchetShift) + 1; }
constexpr Step kDefaultStep = {60, 0, 100, 2, 100, {0, 0, 0}};

// A version 1 / 2 step (the layout before the repack), for loading old files.
struct StepV2 {
  uint8_t note, flags, vel, gate, prob, chord, lockParam, lockValue;
};
static_assert(sizeof(StepV2) == 8, "StepV2");
// Repacks one old step. `instrument`: the track names an instrument (v2 only).
// A macro lock becomes that macro's value (instrument tracks); every other lock
// on an instrument track is dropped (sound-parameter locks were already ignored;
// mixer locks on instrument tracks are no longer supported); on a CLASSIC track
// a parameter lock stays and a macro lock (ignored there) is dropped.
inline Step repackStepV2(const StepV2& o, bool instrument) {
  Step s;
  s.note = o.note;
  s.flagsRaw = uint8_t(o.flags & kStepFlagMask);
  s.vel = o.vel;
  s.gate = o.gate;
  s.probRaw = uint8_t(o.prob > 127 ? 127 : o.prob);
  s.ext[0] = s.ext[1] = s.ext[2] = 0;
  s.setChord(o.chord < 16 ? o.chord : 0);
  if (instrument) {
    if (isMacroLock(o.lockParam)) s.setMacroValue(o.lockParam - kLockMacro0, o.lockValue);
  } else if (o.lockParam && o.lockParam <= kParamsPerTrack) {
    s.setLock(o.lockParam, o.lockValue);
  }
  return s;
}

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
// track (CLASSIC) and sounds exactly as before. 3: the packed Step (above):
// chord in spare bits, the step's last three bytes are a CLASSIC lock or four
// per-note macro values. Versions 1 and 2 are repacked when loaded
// (Engine::sanitizeProject).
constexpr uint8_t kProjectVersion = 3;
constexpr uint8_t kProjectVersionMin = 1;
static_assert(sizeof(TrackSettings) == 40, "TrackSettings must not grow");
static_assert(sizeof(Project) == 67136, "Project must not grow (docs/STATE.md RAM WALL)");
static_assert(sizeof(Project) < 72 * 1024, "Project must fit the RAM budget");

}  // namespace pg
