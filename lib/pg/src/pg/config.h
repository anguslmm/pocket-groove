// Pocket Groove — compile-time configuration shared by every module.
// Portable C++17. No Arduino / ESP-IDF includes anywhere under lib/pg.
#pragma once
#include <cstddef>
#include <cstdint>

namespace pg {

// ---- Tracks ---------------------------------------------------------------
constexpr int kNumTracks      = 8;
constexpr int kNumDrumTracks  = 6;
enum TrackId : uint8_t {
  kTrackKick = 0, kTrackSnare, kTrackClap, kTrackHatClosed, kTrackHatOpen, kTrackPerc,
  kTrackBass,   // monophonic bass synth (acid / bass-house)
  kTrackLead,   // 4-voice poly synth (stabs, chords, leads)
};

// ---- Sequencer ------------------------------------------------------------
constexpr int kMaxSteps       = 64;   // per track, 16th notes
constexpr int kStepsPerPage   = 16;
constexpr int kNumPatterns    = 16;
constexpr int kSongSlots      = 32;
constexpr int kGateUnitsPerStep = 4;  // Step::gate is in quarter-steps

// ---- Parameters -----------------------------------------------------------
constexpr int kSoundParams    = 24;   // per-track sound params, indices 0..23 (voice-defined)
constexpr int kParamsPerTrack = 32;   // 24 sound + 8 mixer (indices 24..31)
constexpr int kNumGlobals     = 32;
constexpr int kSampleSlots    = 12;   // the sample pool (pg/sampler/sample_format.h kPoolSlots)

// ---- Audio ----------------------------------------------------------------
constexpr int   kQuantum           = 16;       // frames per control block; all events land on quantum edges
constexpr float kDefaultSampleRate = 44100.0f; // engine must work at 32000 too (fallback if CPU is short)
// SAMPLE RATES (docs/ARCHITECTURE.md "Sample rates"): the engine runs at any rate
// Engine::init is given; 32, 44.1 and 48 kHz are tested (48 kHz: the Daisy). The FX
// buffers (the delay line, the reverb pool) are sized at compile time for the
// highest rate the platform runs: PG_MAX_SAMPLE_RATE (default 48000; the Cardputer
// builds with 44100, which keeps its RAM what it was). Above it the engine still
// works, with a delay line and a reverb shorter in time than at the rates below.
#ifndef PG_MAX_SAMPLE_RATE
#define PG_MAX_SAMPLE_RATE 48000
#endif
constexpr int   kMaxSampleRate     = PG_MAX_SAMPLE_RATE;
static_assert(kMaxSampleRate >= 44100 && kMaxSampleRate <= 192000, "PG_MAX_SAMPLE_RATE: 44100 .. 192000");
constexpr int   kLeadVoices        = 4;
// Control rate of macro motion: while a macro moves, the engine evaluates the
// track's instrument and hands the voice fractional parameters every this many
// quanta (64 frames: 1.45 ms at 44.1 kHz, 2 ms at 32 kHz). Power of two.
constexpr int   kFineQuanta        = 4;
// ... and evaluates the instrument once per this many quanta (11.6 ms at 44.1 kHz),
// where the macros will be at the end of that interval; the values in between
// are interpolated linearly. A multiple of kFineQuanta.
constexpr int   kFineEvalQuanta    = 32;

// ---- Display --------------------------------------------------------------
constexpr int kScreenW = 240;
constexpr int kScreenH = 135;

}  // namespace pg
