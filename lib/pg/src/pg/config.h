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

// ---- Audio ----------------------------------------------------------------
constexpr int   kQuantum           = 16;       // frames per control block; all events land on quantum edges
constexpr float kDefaultSampleRate = 44100.0f; // engine must work at 32000 too (fallback if CPU is short)
constexpr int   kLeadVoices        = 4;

// ---- Display --------------------------------------------------------------
constexpr int kScreenW = 240;
constexpr int kScreenH = 135;

}  // namespace pg
