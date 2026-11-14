// The sampler voice's parameters, and the voice kinds' parameter tables
// (voice.h VoiceKind). Pure data: the macro module (parsing and evaluating a
// `voice perc sampler` set) and the engine read these without depending on the
// voice itself. docs/MACROLAB.md "The sampler's parameters" lists them with
// what each does.
#pragma once
#include "../params.h"
#include "../voice.h"

namespace pg {

// The tracks that can host a sampler instrument (bit = TrackId): PERC (a
// SamplerVoice: chops, sampler.h) and LEAD (a MelodicSampler: the sample played
// from the keyboard, paraphonic, melodic.h). Instrument wires both through
// Engine::setAltVoice; the kind's parameter list depends on the track.
constexpr uint8_t kSamplerTracks = uint8_t((1u << kTrackPerc) | (1u << kTrackLead));

// "sampler" / "native"; nullptr for an unknown kind.
const char* voiceKindName(int kind);
// The kind a .pgm word names ("sampler"), or -1.
int voiceKindFromName(const char* name);
// May a set of this kind belong to this track (kSamplerTracks)?
bool voiceKindOnTrack(int kind, int track);
// Sound parameter `index` (0..kSoundParams-1) of a voice kind other than the
// native one on `track` (the sampler kind has one list on PERC, sampler::, and
// another on LEAD, mel::); nullptr for the native kind (its table is the track's
// own voice), for a track that does not host the kind and for unused slots.
const ParamInfo* voiceKindParam(int kind, int track, int index);

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
  kRetrig,       // OFF 1/2 1/4 1/8 1/8T 1/16 1/16T 1/32 (steps of the tempo): the chop restarts at this rate while it sounds (gated or releasing)
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

// THE MELODIC SAMPLER's parameters (a `voice lead sampler` set; melodic.h).
// LATCHED at a note's start (they would click if they moved under a sounding
// note): START as the point a note starts from, LOOP, REVERSE, the slot.
// CONTINUOUS (follow a macro while notes sound, also through setParamsFine):
// the rest, and START / LENGTH as the ping-pong loop's window (the playhead is
// never moved: it turns toward a window that moved away from it).
namespace mel {
enum Param : uint8_t {
  kStart = 0,    // 0..127: where in the sample a note starts (0 = its beginning); with LOOP the window's start
  kLength,       // 0..127: how much of the rest plays (127 = to the end; 0 = 20 ms): the one-shot's end / the loop window
  kPitch,        // 16..112 semitones around 64: coarse tune (with the step's note; MIDI 48 = as recorded)
  kFine,         // 0..127, 64 = 0: +-1 semitone
  kCutoff,       // 0..127: filter corner 60 Hz .. 10.5 kHz (127 and LP, no ENV: the filter is off)
  kReso,         // 0..127
  kAttack,       // 0..127: 1 ms .. 2 s (linear)
  kRelease,      // 0..127: after the gate, 5 ms .. 4 s (to -60 dB)
  kDecay,        // 0..127: 20 ms .. 12 s (to -60 dB toward SUSTAIN)
  kSustain,      // 0..127: the level while the gate is held (127 = no decay)
  kLoop,         // ONE / PONG: one-shot to the window's end, or forward-backward over the window while it sounds
  kFilter,       // LP / BP / HP
  kEnvAmt,       // 0..127: the filter envelope's depth, 0 .. +60 semitones of cutoff (shared, from the newest note)
  kFltDecay,     // 0..127: its decay, 30 ms .. 4 s (to -60 dB)
  kGlide,        // 0..127: a slide's pitch glide, time constant 2 ms .. 0.5 s
  kReverse,      // off / on: a note starts at the window's end and plays backwards
  kBits,         // 0..127: 0 = clean, 127 = 2 bits
  kDown,         // 0..127: 0 = off, else sample-and-hold from ~22 kHz down to ~700 Hz
  kDrive,        // 0..127: soft saturation, level-compensated
  kDrift,        // 0..127: slow pitch wander (tape wow), 0 .. +-60 cents, shared by every note
  kDriftRate,    // 0..127: its speed, 0.1 .. 8 Hz
  kNumParams
};
enum LoopMode : uint8_t { kOneShot = 0, kPingPong, kNumLoopModes };
constexpr int kMaxNotes = 4;     // paraphonic: up to four notes into one tone chain
constexpr int kRootNote = 48;    // the step note that plays the sample as recorded (C at the lead's default octave)
const ParamInfo* paramInfo(int index);   // nullptr past kNumParams
}  // namespace mel

}  // namespace sampler
}  // namespace pg
