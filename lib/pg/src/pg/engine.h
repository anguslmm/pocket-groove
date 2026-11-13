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
  NoteOn,           // live play: a = track, b = note, c = velocity, step.chord()/flags honoured.
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
                    //   a straight line in a ramp time (step.ext[0] | step.ext[1] << 8 quanta;
                    //   0 = the default glide, kMacroRampMs; 1 = jump). Build it with
                    //   setMacroCommand(). See "MACRO MOTION" below.
  RefreshInstruments, // a = track (0xFF = every track): look the bound ids up again in the
                    //   library (macro::findInstrument) and re-derive. The host's overlay (the lab)
                    //   sends it after replacing a set; the old set's memory must stay valid until
                    //   instrumentRefreshes() has counted this command (applied or discarded).
  SetMacroOffset,   // a = track, b = macro 0..3 | kOffsetHold: the macro's TILT OFFSET (below) moves to
                    //   a signed target in a ramp. Build it with macroOffsetCommand().
  SetSampleSlot,    // a = track, c = sample pool slot 0..kSampleSlots-1 (TrackSettings::sampleSlot;
                    //   a sampler instrument plays it from the track's next note on)
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
// stored parameters (as CLASSIC) and is kept; so does a set whose voice kind
// (voice.h VOICE KINDS: `voice perc sampler`) the track does not host. The voice
// that renders the track is the bound set's kind: the parameters 0..23 are that
// voice's (derived and sanitised against it), and back on the native voice
// without an instrument they restart from the native defaults. An instrument track has no
// parameter locks: its notes carry their own macro values instead (MOTION,
// below). A CLASSIC track keeps its one parameter lock per step, as before.
//
// MACRO MOTION (docs/MACROLAB.md "What a moving parameter sounds like"): the
// Project holds each macro's position as a byte, and the stored sound
// parameters are always the 7-bit evaluation at those bytes. What the voice
// hears follows a trajectory instead: per track and macro the engine keeps a
// current position that moves to a new SetMacro target along a straight line
// in the command's ramp time (default kMacroRampMs, longer than the gap between
// the UI's key repeats on the device, so a held key or a stream of tilt values
// joins into one continuous movement; a new target retargets from wherever the
// macro is). While any macro of a track moves, the engine evaluates the
// instrument without rounding (macro::evaluateFine) once per kFineEvalQuanta,
// where the macros will be at the end of that interval, and every kFineQuanta
// passes the voice fractional parameters interpolated in between
// (Voice::setParamsFine; drums every kFineEvalQuanta), and the mixer a
// fractional level and sends. When the movement ends the values run into the
// stored bytes (kMacroSettleMs) and the track returns to the 7-bit path, bit for
// bit what a static project plays; it enters the fractional path the same way
// (the first kMacroSettleMs run from the bytes it was playing). Where the macros
// stand still away from the stored bytes (a motion holds them, or they rest
// there after Stop) the 7-bit path plays the instrument evaluated where they
// stand. Jumps, never ramps: an instrument change (or a project loaded or
// edited by any other route), a ramp of 1, Play from the top (to the motion's
// start values), enum and semitone parameters (they take the value the set
// gives at the current position, so they switch where the set says). CLASSIC
// tracks and tracks whose macros stand still never leave the 7-bit path.
//
// MOTION (docs/MACROLAB.md "Motion"): on an instrument track a note (a step
// that is on) can carry its own value of each macro (Step::macroValue, 63
// levels). Per track and macro the KEYFRAMES are the notes with a value of that
// macro, at their grid time including swing, within the pattern's master length
// (a track wraps on its own length); whether a note actually fires (mute,
// probability, a recorded note's suppression, a roll) does not matter: motion is
// deterministic. While playing:
//   - no keyframe in the playing pattern: the macro is at its base (the stored
//     byte, the instrument page's position), as above;
//   - one: it is constant at that value; several: in the quantum a keyframe's
//     step fires the macro is exactly at its value, then it moves along a
//     straight line to the next keyframe's value, arriving in the quantum that
//     one fires; through silence, past notes with no value, across the wrap;
//   - after a pattern's last keyframe the next one is the first keyframe of what
//     plays next (the same pattern looping, a queued pattern, the next song slot:
//     decideNext); none there: the base, arriving at the boundary. A pattern
//     queued late, an edit of the steps or lengths, a tempo or swing change:
//     the motion re-aims from where the macro is at that moment, so it still
//     arrives on the note;
//   - Play from the top starts each macro where the looping pattern has it at
//     step 1 (a keyframe there, or the line from the pattern's last keyframe to
//     its first), so the first pass sounds as the later ones. Stop: the macros
//     rest where the motion had them and the motion lets go (EngineStatus::motion
//     reads 0; the next move of a macro glides from there to its new base);
//   - SetMacro of a macro with motion stores the new base (and the derived bytes)
//     but is not heard until the motion ends: except while recording, when the
//     player's hand is LIVE: from a SetMacro of that macro (record on, transport
//     running) until that macro has had no SetMacro for one bar (16 steps at the
//     tempo then), or record goes off, or Stop. A live macro is heard as moved
//     (stage B's ramps), and every note of the track that starts while it is live
//     (a step that is on, at its grid time; a live-recorded note too) takes its
//     current value, rounded to the 63 levels.
// The engine publishes the heard positions, the motion bits and the live bits
// per track in EngineStatus. Cost: nothing runs per quantum (two compares); at a
// step of a track whose pattern has motion a few compares, and at a keyframe or
// a change a scan of at most two patterns' steps of that track.
// TILT OFFSET (docs/UX.md 10.3 "Tilt"): per track and macro the engine also keeps
// a signed offset, a second trajectory of its own (SetMacroOffset: a straight line
// from where the offset is to the target in the command's ramp). What the voice
// hears, what the page's sliders show (EngineStatus::macroPos) and what motion
// recording writes is
//     clamp(where the macro would otherwise be (its base, or the MOTION) + offset, 0, 255).
// The offset is momentary: it never touches the Project (the base byte, the
// derived parameters, the notes) and is not saved; an instrument change, a
// RefreshInstruments and a new project set it to 0. With every offset at 0 the
// engine plays bit for bit what it played without them (one compare per read).
// Recording: a SetMacroOffset with kOffsetHold (the player holds the gesture and
// has moved it), record on and the transport running makes that macro LIVE as a
// key move does (MOTION, below): every note that starts meanwhile takes the heard
// value. Where the macro had motion it stays where the motion had it at that
// moment (the hand took it), and the offset moves it from there. Live ends when a
// SetMacroOffset without kOffsetHold arrives (the release), unless a key moved the
// macro within the last bar (then the keys' rule ends it), or with record off or Stop.
constexpr uint8_t kOffsetHold = 0x80;
constexpr int kOffsetScale = 16;      // offsets travel in 1/16 of a position unit
constexpr int kMacroRampMs = 72;      // default glide of a SetMacro
constexpr int kMacroSettleMs = 50;    // run into / out of the fractional path
constexpr uint16_t kMacroJump = 1;    // ramp: arrive at the next quantum


struct Command {
  CmdType type;
  uint8_t a, b, c;
  Step    step;
  const void* ptr;   // WriteTrack / WritePattern only
};

// SetInstrument: a = track; the id in step.ext[0] (low byte) / step.ext[1]
// (high byte), the positions in step.note, flagsRaw, vel, gate (macros 0..3).
Command setInstrumentCommand(int track, uint16_t id, const uint8_t positions[4]);
// SetMacro with an explicit ramp (quanta; 0 = kMacroRampMs). (MOTION's glides are
// the engine's own ramps, aimed at the next keyframe's quantum, of any length.)
Command setMacroCommand(int track, int macro, uint8_t position, uint16_t rampQuanta = 0);
// SetMacroOffset: offset in 1/kOffsetScale position units (clamped to +-255 units),
// ramp in milliseconds (0 = a jump), hold: the player holds the gesture (TILT OFFSET).
Command macroOffsetCommand(int track, int macro, int offset16, uint16_t rampMs, bool hold);

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
  // MOTION (below), per track: where each macro is heard now (rounded; updated at
  // most every 4 quanta while it moves), bit m = macro m has motion in the playing
  // pattern (0 while stopped), bit m = macro m is live (motion recording).
  uint8_t  macroPos[kNumTracks][kTrackMacros];
  uint8_t  motion[kNumTracks];
  uint8_t  live[kNumTracks];
};

// SCOPE TAP (the instrument page's picture, docs/UX.md 10.3): one track's voice
// output (its left channel, before level, pan, mute and the mix), averaged over
// `decimation` samples (one ring sample per `decimation` frames: a box filter, so
// a bright wave decimated for a long window keeps a steady shape), written by
// the audio thread into a ring of kScopeRing floats. Single writer (audio thread), single reader (UI thread), no locks: the
// write position is published once per quantum, and readScope() checks after
// copying that the engine did not overwrite what it copied. Off by default; with
// no track tapped the audio thread does nothing for it but one load per quantum
// and a compare per sounding track. The tap only observes: the audio is
// bit-identical with it on or off.
constexpr int kScopeRing = 256;      // samples in the ring
constexpr int kScopeReadMax = 192;   // the most readScope() copies (the rest is the margin against overwrites)
struct ScopeInfo {
  uint32_t end;       // samples written in total (the ring's end): advances only while the voice sounds
  uint32_t since;     // the first sample written with the current track and decimation
  uint32_t mark;      // `end` at the tapped track's last note-on (notes start at quantum edges)
  uint32_t hits;      // note-ons of the tapped track since it was chosen (wraps)
  uint8_t  note;      // the tapped track's last note-on (also before it was tapped; lead: the chord's
                      // root; 0 = none yet)
  uint8_t  track;     // the track the audio thread is tapping, 0xFF = none (yet)
  uint8_t  decimation;
  float    sampleRate;
};

class Engine {
 public:
  Engine();

  // Not real-time. `project` must outlive the engine. Voices are injected so
  // the engine has no compile-time dependency on concrete synths.
  void init(float sampleRate, Project* project);
  void setVoice(int track, Voice* voice);
  // VOICE KINDS (voice.h): track also hosts a voice of `kind` (not kVoiceNative); an
  // instrument of that kind on the track plays through it. The engine switches at the
  // quantum edge where the bound instrument's kind changes: the notes the old voice
  // holds are released, it is choked and rendered until its fade ends, and the
  // parameter ranges follow the voice that renders. Same rules as setVoice (not while
  // process() runs; the caller has run Voice::init). nullptr removes it.
  void setAltVoice(int track, int kind, Voice* voice);
  bool hostsKind(int track, int kind) const;   // kVoiceNative: the track has a voice at all
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
  // lead and 0 elsewhere; on a CLASSIC track a lock naming a param that
  // exists, with its value in that param's range; on an instrument track any
  // macro values). Not real-time; call after setVoice()
  // (param ranges come from the voices) and before handing the project to
  // the engine. A valid project is left byte-identical. A version-1 or 2
  // project becomes version 3: its steps are repacked (project.h Step,
  // repackStepV2); version 1 has no instrument on any track. A track whose
  // instrument the library knows gets its sound parameters derived (so after
  // a load they already are what the engine will play).
  void sanitizeProject(Project& p) const;

  // Metadata for the generic UI. index 0..23 -> voice, 24..31 -> mixer table.
  // The native voice's (what CLASSIC tracks play and the parameter pages show);
  // the second form gives a hosted kind's (nullptr if the track hosts none).
  const ParamInfo* trackParamInfo(int track, int index) const;
  const ParamInfo* trackParamInfo(int track, int kind, int index) const;
  static const ParamInfo* globalParamInfo(int id);

  // ---- audio thread -------------------------------------------------------
  // Render interleaved stereo int16. `frames` must be a multiple of kQuantum.
  void process(int16_t* out, int frames);

  // ---- UI thread ----------------------------------------------------------
  bool post(const Command& c);       // false if the queue is full
  // Diagnostics: the most commands that were ever waiting in the queue at once
  // (seen by post(); the queue holds 256), so its size can be judged on the device.
  uint32_t queuePeak() const;
  EngineStatus status();             // snapshot; resets peak hold
  bool isPlaying() const;            // transport running? No side effects (status() resets the peak hold)
  uint8_t instrumentRefreshes() const;   // as in status(), without its side effects

  // Scope tap (above). track -1 = off; decimation 1..255 (clamped). Takes effect
  // at the next quantum; samples written before it are not reported by readScope.
  void setScope(int track, int decimation);
  // Copies the newest min(n, kScopeReadMax, end - since) samples, oldest first,
  // into out and fills *info. Returns the count, or 0 when nothing consistent
  // could be read (nothing written yet, or the engine overwrote the copy or
  // changed the tap while copying: try again at the next frame).
  int readScope(float* out, int n, ScopeInfo* info) const;

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
