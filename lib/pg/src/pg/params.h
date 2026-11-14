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
  Swing,     // 0..50 shown as 50%..75%
  DjFilter,  // 64 = "OFF", below = "LPnn", above = "HPnn"
};

struct ParamInfo {
  const char* name;       // <= 4 chars, upper case, for the tiny screen ("CUT", "RES", "DCY")
  const char* longName;   // <= 12 chars ("Cutoff", "Resonance")
  uint8_t min, max, def;
  ParamFormat fmt;
  const char* const* enumNames;  // only for ParamFormat::Enum, max-min+1 entries, each <= 5 chars
};

// ---- Per-track mixer params (track param indices 24..31), same for all tracks.
enum MixParam : uint8_t {
  kMixLevel = 24,   // 0..127
  kMixPan,          // 0..127, 64 = centre
  kMixDelaySend,    // 0..127
  kMixReverbSend,   // 0..127
  kMixDuck,         // 0..127: how much the sidechain ducker pulls this track down
  // THE TRACK'S TUNE (a sampler instrument only: UX.md 10.3 "Tune"). Signed bytes
  // (int8_t in a uint8_t), 0 = no change, so every older file (these were
  // reserved, sanitised to zero) and every new project plays untuned. No
  // ParamInfo: not a mixer row, not lockable, not a macro target; the macros'
  // derivation and combineMixer never write them. Kept on the tracks that can
  // host a sampler (kSamplerTracks), zero elsewhere; the sampler voices add them
  // to their own PITCH / FINE (Voice::setTune); every other voice ignores them.
  kMixTune,         // semitones, -kTuneMaxSemis..+kTuneMaxSemis
  kMixTuneCents,    // cents, -kTuneMaxCents..+kTuneMaxCents (the gesture steps kTuneCentStep)
  kMixReserved2,
};
constexpr int kTuneMaxSemis = 24;
constexpr int kTuneMaxCents = 50;
constexpr int kTuneCentStep = 5;
inline uint8_t tuneByte(int v) { return uint8_t(int8_t(v)); }
inline int tuneValue(uint8_t b) { return int(int8_t(b)); }
// The track's tune in semitones, from its two stored bytes (clamped to the ranges).
inline float trackTune(const uint8_t* params) {
  int s = tuneValue(params[kMixTune]), c = tuneValue(params[kMixTuneCents]);
  s = s < -kTuneMaxSemis ? -kTuneMaxSemis : (s > kTuneMaxSemis ? kTuneMaxSemis : s);
  c = c < -kTuneMaxCents ? -kTuneMaxCents : (c > kTuneMaxCents ? kTuneMaxCents : c);
  return float(s) + float(c) * 0.01f;
}

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
