// Parameter model. Every tweakable value is a uint8_t with a ParamInfo that
// gives the UI its name, range, default and display format. The UI is generic
// over these tables; voices and effects own the mapping to physical units.
#pragma once
#include "config.h"

namespace pg {

enum class ParamFormat : uint8_t {
  Number,    // show raw value
  Percent,   // min..max shown as 0..100%
  Bipolar,   // centre of range is zero, show -N..+N
  Enum,      // enumNames[value - min]
  OnOff,
  Semitones, // value - centre, in semitones
  Bpm,
};

struct ParamInfo {
  const char* name;       // <= 4 chars, upper case, for the tiny screen ("CUT", "RES", "DCY")
  const char* longName;   // <= 12 chars ("Cutoff", "Resonance")
  uint8_t min, max, def;
  ParamFormat fmt;
  const char* const* enumNames;  // only for ParamFormat::Enum, max-min+1 entries
};

// ---- Per-track mixer params (track param indices 24..31), same for all tracks.
enum MixParam : uint8_t {
  kMixLevel = 24,   // 0..127
  kMixPan,          // 0..127, 64 = centre
  kMixDelaySend,    // 0..127
  kMixReverbSend,   // 0..127
  kMixDuck,         // 0..127: how much the sidechain ducker pulls this track down
  kMixReserved0, kMixReserved1, kMixReserved2,
};

// ---- Global params (Project::globals indices). Table lives in engine/globals.cpp.
enum GlobalParam : uint8_t {
  kGTempo = 0,       // BPM, raw value (60..200)
  kGSwing,           // 0..50 => 50%..75% swing on off-16ths
  kGMasterVolume,
  kGDuckSource,      // enum: Off, Kick track, Every beat (4-on-floor, works with kick muted)
  kGDuckDepth,       // how far the ducked tracks are pulled down at the trigger
  kGDuckHold,        // time held at full depth before recovery
  kGDuckRelease,     // recovery time
  kGDuckCurve,       // recovery curve shape (log .. linear .. exp)
  kGDuckFx,          // how much the delay/reverb returns are ducked
  kGDelayTime,       // enum of tempo-synced divisions
  kGDelayFeedback,
  kGDelayTone,       // filtering in the feedback loop
  kGDelayWidth,      // mono .. ping-pong
  kGReverbSize,
  kGReverbDamp,
  kGMasterFilter,    // DJ filter: 64 = off, below = low-pass closing, above = high-pass opening
  kGMasterReso,
  kGMasterDrive,     // bus saturation
  kGScaleRoot,       // 0..11, used by the UI for note entry
  kGScaleType,       // enum, used by the UI for note entry
  kGNumUsed
};
static_assert(kGNumUsed <= kNumGlobals, "too many globals");

// ---- Lead-track chord types for Step::chord. Lead voice owns the interval table.
enum ChordType : uint8_t {
  kChordNone = 0, kChordOct, kChordFifth, kChordMaj, kChordMin, kChordMaj7, kChordMin7,
  kChordDom7, kChordSus4, kChordMin9, kChordNum
};

}  // namespace pg
