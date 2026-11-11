// Offline stem tap: lets a desktop renderer (host/tools/bounce) read the
// engine's float buses in the same pass that produces the normal int16 output.
//
// COMPILE-TIME OPTION. Everything here exists only when PG_STEM_TAP is 1. The
// host build defines it (host/CMakeLists.txt); the firmware does not, so on
// the device the preprocessor removes every trace of the tap: the audio path
// compiles to the same machine code as without this file, with the same RAM
// and CPU cost. With PG_STEM_TAP = 1 and no tap installed (the default) the
// engine's output is bit-identical too (host/itests/itest_golden.cpp).
//
// What is tapped (stereo mode only; in mono device mode the tap is ignored),
// per frame, all in the engine's float bus units (1.0 = int16 full scale of
// the master output at master volume 100 / drive 0):
//   track[t]   track t's contribution to the dry bus: the voice after the
//              track's level, pan, mute and sidechain duck, i.e. exactly
//              what the mixer adds to the bus for that track.
//   delayRet   the delay return as added to the bus (after the FX duck).
//   reverbRet  the reverb return as added to the bus (after the FX duck).
//   preMaster  the bus handed to the master section (DC/rumble filter, DJ
//              filter, drive, volume, limiter, int16).
// sum(track[0..7]) + delayRet + reverbRet == preMaster to float rounding
// (host/itests/itest_bounce.cpp). The master bus is nonlinear and is not split.
//
// The tap only observes; the one thing it can change is the send mask:
// a track whose bit is clear sends nothing to the delay and reverb (its dry
// signal is unchanged). The bounce tool uses that for wet stems (one pass per
// track, each pass's FX return is that track's share).
//
// Also host-only: setEndAfterPatternPlays() makes the sequencer stop firing
// steps at a pattern boundary (after N pattern plays since Play) without
// choking anything, so the last notes and the FX tails ring out naturally.
#pragma once
#include <cstdint>

#include "../config.h"

#ifndef PG_STEM_TAP
#define PG_STEM_TAP 0
#endif

#if PG_STEM_TAP
namespace pg {
namespace engine_detail {

struct StemTap {
  // Caller-owned buffers of `capacity` frames each (any may be nullptr = not
  // wanted). Quantum by quantum the engine writes frames [pos, pos + 16) and
  // advances pos; it stops writing (and sets overflow) when pos reaches
  // capacity. The caller resets pos between process() calls as it likes.
  float* track[kNumTracks][2] = {};
  float* delayRet[2] = {};
  float* reverbRet[2] = {};
  float* preMaster[2] = {};
  long capacity = 0;
  long pos = 0;
  bool overflow = false;
  uint32_t sendMask = 0xFFu;   // bit t set: track t feeds the delay / reverb sends
};

// Installs (or with nullptr removes) the tap. Not real-time: call while the
// engine is not inside process(). Engine::init() keeps the tap installed.
void setStemTap(StemTap* tap);

// After `plays` pattern plays since the last Play (song slots count each
// repeat), the sequencer fires no further steps: pending note-offs, gates and
// ratchets of the last step still run, nothing is choked. -1 = off (default).
// Takes effect for the next Play. endedAtFrame() is the engine frame (counted
// from Engine::init) of the boundary where it stopped, or -1.
void setEndAfterPatternPlays(int plays);
long endedAtFrame();

}  // namespace engine_detail
}  // namespace pg
#endif
