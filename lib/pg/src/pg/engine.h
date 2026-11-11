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
  NoteOff,          // a = track, b = note
  QueuePattern,     // a = pattern: switch at the next pattern boundary (immediately if stopped)
  SetSongMode,      // c = 0/1
  SetSongSlot,      // a = slot, b = pattern, c = repeats
  SetRecord,        // c = 0/1
  ClearTrack,       // a = pattern, b = track
  ClearPattern,     // a = pattern
  CopyPattern,      // a = source pattern, b = destination pattern
  CopyTrack,        // a = pattern, b = source track, c = destination track (same type only)
};

struct Command {
  CmdType type;
  uint8_t a, b, c;
  Step    step;
};

struct EngineStatus {
  bool     playing;
  bool     recording;
  uint8_t  pattern;            // currently playing pattern
  uint8_t  queuedPattern;      // 0xFF if none
  uint8_t  songSlot;
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

  // Fill `p` with a valid empty project: magic/version, default globals,
  // each track's params from its voice's ParamInfo defaults + mixer defaults,
  // all patterns length 16 with kDefaultStep in every slot.
  void initProject(Project& p) const;

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
