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
};

struct Command {
  CmdType type;
  uint8_t a, b, c;
  Step    step;
  const void* ptr;   // WriteTrack / WritePattern only
};

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
  // the engine. A valid project is left byte-identical.
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

  // Stop touching the Project (output silence) so the UI thread can load a
  // new one into the same memory. suspend() returns once the audio thread has
  // acknowledged (or immediately if process() has never been called).
  void suspend();
  void resume();

 private:
  struct Impl;
  Impl* impl_;       // statically allocated inside engine.cpp, no heap
};

}  // namespace pg
