// The sampler voice's parameters, and the voice kinds' parameter tables
// (voice.h VoiceKind). Pure data: the macro module (parsing and evaluating a
// `voice perc sampler` set) and the engine read these without depending on the
// voice itself. docs/MACROLAB.md "The sampler's parameters" lists them with
// what each does.
#pragma once
#include "../params.h"
#include "../voice.h"

namespace pg {

// The tracks that can host a sampler instrument (bit = TrackId). Only PERC is
// wired today (Instrument gives the PERC track a SamplerVoice); the mechanism
// (Engine::setAltVoice) is per track.
constexpr uint8_t kSamplerTracks = uint8_t(1u << kTrackPerc);

// "sampler" / "native"; nullptr for an unknown kind.
const char* voiceKindName(int kind);
// The kind a .pgm word names ("sampler"), or -1.
int voiceKindFromName(const char* name);
// May a set of this kind belong to this track (kSamplerTracks)?
bool voiceKindOnTrack(int kind, int track);
// Sound parameter `index` (0..kSoundParams-1) of a voice kind other than the
// native one; nullptr for the native kind (its table is the track's own voice)
// and for unused slots.
const ParamInfo* voiceKindParam(int kind, int index);

namespace sampler {

// Ranges 0..127 at most, like every other voice. LATCHED at note-on (they would
// click if they moved under a sounding chop): SLICES, SLICE, START, REVERSE,
// ATTACK, LOOP. CONTINUOUS (follow a macro while the chop sounds, through
// setParamsFine): the rest. Enums and semitone intervals jump (engine.h).
enum Param : uint8_t {
  kSlices = 0,   // 1..16: the sample (or the region START..LENGTH would give) cut into this many equal slices
  kSlice,        // 0..15: which slice plays (modulo SLICES)
  kStart,        // 0..127: where in the slice the chop starts (0 = its beginning)
  kLength,       // 0..127: how much of the rest of the slice plays (127 = to its end; 0 = a 10 ms blip)
  kPitch,        // semitones around 64 (16..112 = -48..+48): with the step's note (60 = as recorded)
  kFine,         // 0..127, 64 = 0: +-1 semitone
  kSpeed,        // 0..127: tape speed, 96 = x1, 64 = stopped, 32 = x1 backwards, 0 = x2 backwards, 127 = ~x2
  kReverse,      // off / on: the chop plays from its end to its start
  kAttack,       // 0..127: 0 = 1 ms .. ~400 ms fade in
  kRelease,      // 0..127: after the gate, 5 ms .. ~2 s
  kCutoff,       // 0..127: filter corner 60 Hz .. 10.5 kHz (127 and LP: the filter is off)
  kReso,         // 0..127
  kFilter,       // LP / BP / HP
  kBits,         // 0..127: 0 = clean, 127 = 2 bits (a bit-depth crush)
  kDown,         // 0..127: 0 = off, else a sample-and-hold rate from ~22 kHz down to ~700 Hz
  kDrive,        // 0..127: soft saturation, level-compensated
  kRetrig,       // OFF 1/2 1/4 1/8 1/8T 1/16 1/16T 1/32 (steps of the tempo): the chop restarts at this rate while gated
  kRepPitch,     // 0..127, 64 = 0: each restart is this much higher (above 64) or lower, +-12 semitones per repeat at the ends
  kRepFade,      // 0..127: each restart is quieter by this much (0 = all equal, 127 = -6 dB per repeat)
  kLoop,         // off / on: the region loops while the gate is held
  kNumParams
};

enum FilterType : uint8_t { kLP = 0, kBP, kHP, kNumFilters };
constexpr int kNumRetrig = 8;
// The retrigger periods in 16th steps, per kRetrig value (0 = off).
constexpr float kRetrigSteps[kNumRetrig] = {0.0f, 8.0f, 4.0f, 2.0f, 4.0f / 3.0f, 1.0f, 2.0f / 3.0f, 0.5f};

const ParamInfo* paramInfo(int index);   // nullptr past kNumParams

}  // namespace sampler
}  // namespace pg
