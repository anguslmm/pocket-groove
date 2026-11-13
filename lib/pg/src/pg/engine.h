// The audio engine: sequencer + 8 voices + mixer + sidechain ducker + send FX
// + master bus. Owns all mutation of the Project while running.
//
// Threads:  UI thread  --post(Command)-->  [lock-free SPSC queue]  --> audio thread
//           UI thread  <--status() atomics--                           audio thread
#pragma once
#include <atomic>
#include "project.h"
#include "voice.h"

namespace pg {

enum class CmdType : uint8_t {
  Play,             // start from step 0 (a = 1: continue from current position)
  Stop,             // stop transport, release/choke voices, let FX tails ring
  SetStep,          // a = pattern, b = track, c = step index, step = new contents
  SetTrackParam,    // a = track, b = param index (0..31), c = value
  SetGlobal,        // a = GlobalParam, c = value
  SetTrackLength,   // a = pattern, b = track, c = length
  SetPatternLength, // a = pattern, c = length
  SetMute,          // a = track, c = 0/1
  NoteOn,           // live play: a = track, b = note, c = velocity, step.chord/flags honoured.
                    //   If recording and playing, the engine also writes it to the nearest step.
                    //   A NoteOn that arrives while the previous recorded note on that synth track
                    //   is still held (legato) sets kStepSlide on the PREVIOUS note's step.
  NoteOff,          // a = track, b = note. When recording, sets the recorded step's gate from
                    //   the held duration (min 1 quarter-step).
  QueuePattern,     // a = pattern: switch at the next pattern boundary (immediately if stopped)
  SetSongMode,      // c = 0/1, a = slot to start from when switching on
  SetSongSlot,      // a = slot, b = pattern, c = repeats
  SetRecord,        // c = 0/1
  ClearTrack,       // a = pattern, b = track
  ClearPattern,     // a = pattern
  CopyPattern,      // a = source pattern, b = destination pattern
  CopyTrack,        // a = pattern, b = source track, c = destination track (same type only)
  Roll,             // a = track, c = hits per step x2 (1 = 1/8 notes, 2 = 1/16, 3 = 1/16T, 4 = 1/32), 0 = stop.
                    //   While active the track retriggers on that grid (locked to the transport when
                    //   playing, free-running from the command when stopped) using step.note/vel/chord.
  WriteTrack,       // a = pattern, b = track, ptr = const TrackPattern* to copy in (paste / undo)
  WritePattern,     // a = pattern, ptr = const Pattern* to copy in (paste / undo)
                    //   For both: the engine copies during a later quantum; the buffer must stay
                    //   valid and unmodified until status().writesDone has incremented.
  SetInstrument,    // a = track, instrument id and the four macro positions: build it with
                    //   setInstrumentCommand(). Id 0 = none (CLASSIC: the sound parameters stay
                    //   as they are now and play as stored). See "Instruments" below.
  SetMacro,         // a = track, b = macro 0..3, c = position 0..255; the macro moves there along
                    //   a straight line in a ramp time (step.lockParam | step.lockValue << 8 quanta;
                    //   0 = the default glide, kMacroRampMs; 1 = jump). Build it with
                    //   setMacroCommand(). See "MACRO MOTION" below.
  RefreshInstruments, // a = track (0xFF = every track): look the bound ids up again in the
                    //   library (macro::findInstrument) and re-derive. The host's overlay (the lab)
                    //   sends it after replacing a set; the old set's memory must stay valid until
                    //   instrumentRefreshes() has counted this command (applied or discarded).
};

// INSTRUMENTS (docs/MACROLAB.md): a track whose TrackSettings name an
// instrument (a macro set found by macro::findInstrument) has its sound
// parameters 0..23 derived by the engine: macro::evaluate(set, positions,
// under = the voice's defaults), written into Project params, whenever the id
// or a position changes (the engine compares the Project bytes with what it
// derived from once per quantum, so a project edited or loaded by any route
// is followed; evaluation itself runs only on a change, at a quantum edge).
// The mixer rows stay the player's and combine with the instrument's level and
// sends (macro::combineMixer). An id the library does not know plays the
// stored parameters (as CLASSIC) and is kept. Locks on such a track: a macro
// lock (Step::lockParam 33..36) plays the instrument with that macro at the
// lock's value for the lock's scope (evaluated at the step); a lock of a sound
// parameter (1..24) is ignored (the instrument owns them); mixer locks work.
// On a CLASSIC track macro locks are ignored and everything else is as before.
//
// MACRO MOTION (docs/MACROLAB.md "What a moving parameter sounds like"): the
// Project holds each macro's position as a byte, and the stored sound
// parameters are always the 7-bit evaluation at those bytes. What the voice
// hears follows a trajectory instead: per track and macro the engine keeps a
// current position that moves to a new SetMacro target along a straight line
// in the command's ramp time (default kMacroRampMs, longer than the gap between
// the UI's key repeats on the device, so a held key or a stream of tilt values
// joins into one continuous movement; a new target retargets from wherever the
// macro is). While any macro of a track moves, every kFineQuanta quanta the
// engine evaluates the instrument at the current positions without rounding
// (macro::evaluateFine) and passes the voice fractional parameters
// (Voice::setParamsFine) and the mixer fractional level and sends. When the
// movement ends the values blend into the stored bytes (kMacroSettleMs) and the
// track returns to the 7-bit path, bit for bit what a static project plays; it
// enters the fractional path the same way (the first kMacroSettleMs blend from
// the bytes it was playing). Jumps, never ramps: an instrument change (or a
// project loaded or edited by any other route), a ramp of 1, a macro lock
// starting or ending at a step (the note it fires snaps anyway; a note still
// ringing on a legato slide or another lead voice hears the jump through the
// voice's own de-click smoothing), enum and semitone parameters (they take the
// value the set gives at the current position, so they switch where the set
// says). CLASSIC tracks and tracks whose macros stand still never leave the
// 7-bit path.
constexpr int kMacroRampMs = 72;      // default glide of a SetMacro
constexpr int kMacroSettleMs = 50;    // blend into / out of the fractional path
constexpr uint16_t kMacroJump = 1;    // ramp: arrive at the next quantum


struct Command {
  CmdType type;
  uint8_t a, b, c;
  Step    step;
  const void* ptr;   // WriteTrack / WritePattern only
};

// SetInstrument: a = track; the id in step.lockParam (low byte) / step.lockValue
// (high byte), the positions in step.note, flags, vel, gate (macros 0..3).
Command setInstrumentCommand(int track, uint16_t id, const uint8_t positions[4]);
// SetMacro with an explicit ramp (quanta; 0 = kMacroRampMs). The "motion" stage
// sets ramps that arrive exactly at a note (ramp = quanta until then).
Command setMacroCommand(int track, int macro, uint8_t position, uint16_t rampQuanta = 0);

struct EngineStatus {
  bool     playing;
  bool     recording;
  uint8_t  pattern;            // currently playing pattern
  uint8_t  queuedPattern;      // 0xFF if none
  uint8_t  songSlot;
  uint8_t  songRepeat;         // which play of the current slot we are on, 0-based
  uint8_t  masterStep;         // position within Pattern::length (pattern-boundary countdown)
  uint8_t  writesDone;         // increments (wraps) each time a WriteTrack/WritePattern is applied
  uint8_t  step[kNumTracks];   // current step per track
  uint8_t  trig[kNumTracks];   // increments every time the track fires (UI flashes on change)
  float    peakL, peakR;       // post-master peak since last status() call, 0..1+
  float    duckGain;           // current ducker gain 0..1 (1 = not ducking), for the UI meter
  uint32_t revision;           // bumps whenever the Project is modified
  uint8_t  instrumentRefreshes; // increments (wraps) per RefreshInstruments consumed (applied or discarded)
};

class Engine {
 public:
  Engine();

  // Not real-time. `project` must outlive the engine. Voices are injected so
  // the engine has no compile-time dependency on concrete synths.
  void init(float sampleRate, Project* project);
  void setVoice(int track, Voice* voice);
  // Device mode: render one bus and mono FX; process() then writes L == R.
  // Call after init/setVoice and before audio starts; forwards to every voice.
  void setMonoOutput(bool mono);

  // Fill `p` with a valid empty project: magic/version, default globals,
  // each track's params from its voice's ParamInfo defaults + mixer defaults,
  // all patterns length 16 with kDefaultStep in every slot.
  void initProject(Project& p) const;

  // Clamp every field of a loaded (untrusted) project into its legal range:
  // magic/version, NUL-terminated name, songMode, song slots, globals and
  // track params within their ParamInfo min/max (unused slots 0), mute 0/1,
  // pattern/track lengths 1..64, and every step (note <= 127, vel 1..127,
  // gate >= 1, prob <= 100, known flag bits only, chord < kChordNum on the
  // lead and 0 elsewhere, lockParam <= 32 naming a param that exists, with
  // lockValue in that param's range). Not real-time; call after setVoice()
  // (param ranges come from the voices) and before handing the project to
  // the engine. A valid project is left byte-identical. A version-1 project
  // becomes version 2 with no instrument on any track. A track whose
  // instrument the library knows gets its sound parameters derived (so after
  // a load they already are what the engine will play).
  void sanitizeProject(Project& p) const;

  // Metadata for the generic UI. index 0..23 -> voice, 24..31 -> mixer table.
  const ParamInfo* trackParamInfo(int track, int index) const;
  static const ParamInfo* globalParamInfo(int id);

  // ---- audio thread -------------------------------------------------------
  // Render interleaved stereo int16. `frames` must be a multiple of kQuantum.
  void process(int16_t* out, int frames);

  // ---- UI thread ----------------------------------------------------------
  bool post(const Command& c);       // false if the queue is full
  EngineStatus status();             // snapshot; resets peak hold
  bool isPlaying() const;            // transport running? No side effects (status() resets the peak hold)
  uint8_t instrumentRefreshes() const;   // as in status(), without its side effects

  // Stop touching the Project (output silence) so the UI thread can load a
  // new one into the same memory. suspend() returns once the audio thread has
  // acknowledged (or immediately if process() has never been called).
  // Commands still queued when the audio thread acknowledges, and any posted
  // while suspended, are DISCARDED, never applied (they were meant for the old
  // project); a discarded WriteTrack/WritePattern still increments
  // status().writesDone, so a buffer the UI is waiting to release is freed.
  // Active rolls stop and held notes are released. Commands posted after
  // resume() returns are kept. After resume() the engine starts clean:
  // stopped at the top, voices choked, FX cleared, the new project sanitised.
  void suspend();
  void resume();

 private:
  struct Impl;
  Impl* impl_;       // statically allocated inside engine.cpp, no heap
};

}  // namespace pg
