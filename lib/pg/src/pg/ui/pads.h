// PADS (docs/UX.md 10.4 "Pads", docs/MACROLAB.md "Velocity and pressure"): the
// platform-neutral input for force-sensing pads. A platform with pads (the Daisy
// prototype: 4 force sensors, later 16; the desktop: keys and the mouse) reads its
// sensors and calls, on the UI thread, with its own pad numbering 0..kMaxPads-1:
//
//     ui.padDown(pad, velocity, nowMs);    // the hit: velocity 0..1 (the platform's
//                                          // calibrated strike force; linear to MIDI 1..127)
//     ui.padPressure(pad, pressure, nowMs);// while held, as often as it reads: 0..1
//     ui.padUp(pad, nowMs);                // the release
//     ui.setPadJob(PadJob::Drums / Notes); // or ui.togglePadJob() (a toast says which)
//
// A pad that is already down gets a padDown as a release and a new hit. A pad
// index out of range is ignored. Nothing else is needed: the Ui turns hits into
// notes (Ui::noteOn: the same path as the note keys, so live record records them
// with their velocity) and pressure into a momentary macro offset (the same engine
// offset tilt uses, engine.h "TILT OFFSET"; the two add when they move one macro).
//
// The two jobs:
//   Drums  pad i plays drum track kDrumPadTrack[i] (KICK SNARE CHH PERC, then CLAP OHH:
//          4 pads are the first four), note 60. The selected track does not change.
//   Notes  pad i plays scale degree i on the track selected at the hit (synths: the
//          octave and the lead's chord mode as the note keys; a drum or a sampler
//          instrument: 60 + the scale's degree i, the drum retuned). A second pad held
//          on a synth plays legato (slide), as a second note key does.
// Pressure: per track, the largest pressure of that track's held pads moves the macro
// the track's instrument names (`press MACRO [response]`, macro::MacroSet::pressAssign)
// by up to kPressRange[response] position units of 255, after a dead zone; when the
// last pad of the track goes up it glides back in kPressReturnMs. CLASSIC tracks and
// `press none`: pressure does nothing. Pressure is performance only: it is not
// recorded into the notes (velocity is, in the note itself).
//
// PG_MAX_PADS: how many pads the build keeps state for (default 16). A build without
// pads (the Cardputer) sets 0: the calls then do nothing and cost no RAM.
#pragma once
#include <cstdint>

#include "../config.h"

#ifndef PG_MAX_PADS
#define PG_MAX_PADS 16
#endif

namespace pg {
namespace ui {

constexpr int kMaxPads = PG_MAX_PADS;
static_assert(kMaxPads >= 0 && kMaxPads <= 32, "PG_MAX_PADS: 0 .. 32");

enum class PadJob : uint8_t { Drums = 0, Notes = 1 };

// Drums job: the track pad i plays (-1 = none). The first four are the 4-pad layout.
constexpr int8_t kDrumPadTrack[6] = {kTrackKick, kTrackSnare, kTrackHatClosed, kTrackPerc, kTrackClap, kTrackHatOpen};
inline int drumPadTrack(int pad) { return pad >= 0 && pad < 6 ? kDrumPadTrack[pad] : -1; }

// A hit's velocity 0..1 as the note's velocity 1..127 (linear; NaN or below 0: 1).
inline int padVelocity(float v) {
  if (!(v > 0.0f)) return 1;
  if (v >= 1.0f) return 127;
  return 1 + int(v * 126.0f + 0.5f);
}

// Pressure, per response (the `press` line's word, macro::PressAssign::response; the
// same names as tilt's): the glide to each new value and the offset at full pressure.
//   fast    10 ms, 160 units   twitchy, immediate
//   smooth  30 ms, 160 units   the default
//   fine    30 ms,  64 units   a subtle squeeze
//   wide    60 ms, 255 units   the whole travel
constexpr uint16_t kPressRampMs[4] = {10, 30, 30, 60};
constexpr uint8_t kPressRange[4] = {160, 160, 64, 255};
constexpr uint32_t kPressReturnMs = 120;   // the last pad of the track up: the glide back
constexpr uint32_t kPressEveryMs = 8;      // at most one offset per track this often (the engine queue)
constexpr float kPressDeadZone = 0.06f;    // the resting touch of a held pad moves nothing

// The offset for pressure p (0..1) in 1/16 position units (engine.h kOffsetScale),
// rounded to whole position units (fewer commands for a sensor that jitters).
inline int pressOffset16(float p, int response) {
  if (!(p > kPressDeadZone)) return 0;
  const float x = p >= 1.0f ? 1.0f : (p - kPressDeadZone) / (1.0f - kPressDeadZone);
  return int(x * float(kPressRange[response & 3]) + 0.5f) * 16;
}

}  // namespace ui
}  // namespace pg
