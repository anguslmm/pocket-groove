// The tape's files (docs/MANUAL.md 21 "Files"): beside the project, never inside it,
// so a project stays small and still loads on a device without a tape:
//
//   <mount>/pg/NAME.pgv          the project (pg/io/project_io.h)
//   <mount>/pg/NAME/tape.txt     the tape's settings (below)
//   <mount>/pg/NAME/track1.wav   one mono 16-bit WAV per tape track that holds audio,
//   ...      track4.wav          at the tape's rate, from tape position 0 to its last sound
//
// tape.txt is short text, one setting per line ('#' comments; unknown lines ignored):
//   pgtape 1
//   rate 48000                   the engine rate it was recorded at (samples)
//   bpm 120                      the tape's tempo (0: nothing recorded)
//   loop 4                       the loop length in bars
//   track 1 level 100 pan 64 delay 0 reverb 0 mute 0 samples 384000
//   (one line per track)
//
// Both directions are INCREMENTAL: step() moves at most `budget` samples (a track at a
// time, in pieces of kPiece), so a device's main loop can run it between frames
// without stalling audio. The FileSystem's streaming calls are used (one file open at a
// time); a track is written to trackN.tmp and renamed over trackN.wav when complete.
// Loading a tape recorded at another rate than the engine's is refused (phase 1) with
// a message; the tape is then empty. A project without a tape folder has an empty tape.
// Saving removes the WAV of a track that is empty now. A save can be limited to some
// tracks (the session's autosave writes only those that changed): the others' files
// are left as they are. Main thread only.
#pragma once
#include <cstdint>

#include "../platform.h"
#include "tape.h"

namespace pg {
namespace tape {

// LIFT (docs/MANUAL.md 21 "Lift"): tape audio into a sample pool slot. The platform
// keeps it as it keeps a recording (the desktop: samples/NN-tapNN.wav; the pool's
// rules for the stored rate: recorder.h recRatesFor). Main thread.
class SlotWriter {
 public:
  // The most tape samples (at `rate`) one slot takes.
  virtual uint32_t capacity(uint32_t rate) const = 0;
  // n samples of track t from `start` become slot `slot`'s sample. why: a short reason.
  virtual bool lift(int slot, const Tape& tape, int t, uint32_t start, uint32_t n, const char** why) = 0;

 protected:
  ~SlotWriter() = default;
};

// The session's tape (the desktop: kept with the autosaved session, _last.pgv): its
// folder is /int/pg/_last (a name no project can have: projects are upper case).
constexpr const char* kSessionTape = "_last";

// "/int/pg/NAME" (no slash at the end) into out (at least 32 bytes).
void folderOf(char* out, const char* mount, const char* name);

class TapeFiles {
 public:
  enum class State : uint8_t { Idle = 0, Saving, Loading, Done, Failed };
  static constexpr uint32_t kPiece = 4096;   // samples per read / write

  // folder: "/int/pg/NAME". The tape must be idle (no take). Load: the tape is
  // cleared first; its settings come from tape.txt. false: nothing started (why()).
  // tracks: a mask of the tape tracks whose files are written (bit t = track t + 1);
  // tape.txt is always written.
  bool startSave(FileSystem* fs, const char* folder, Tape* tape, uint8_t tracks = 0xF);
  bool startLoad(FileSystem* fs, const char* folder, Tape* tape);
  // Every frame while busy(): moves up to `budget` samples. Returns the state; Done or
  // Failed once (then Idle again on the next call).
  State step(uint32_t budget = 1u << 16);
  bool busy() const { return st_ == State::Saving || st_ == State::Loading; }
  State state() const { return st_; }
  const char* why() const { return why_; }   // a short upper-case message (Failed / Done)
  int percent() const;                       // of the samples to move
  bool loaded() const { return loaded_; }    // the last load found a tape folder
  uint8_t tracksDone() const { return doneMask_; }   // the job's tracks complete so far (bit t)
  void cancel();

 private:
  bool openTrack();
  void move(uint32_t budget);
  bool fail(const char* why);
  uint32_t contentEnd(int t) const;

  FileSystem* fs_ = nullptr;
  Tape* tape_ = nullptr;
  char dir_[40] = {};
  State st_ = State::Idle;
  const char* why_ = "";
  int track_ = 0;
  int h_ = -1;
  uint32_t pos_ = 0, len_ = 0;
  uint32_t total_ = 0, done_ = 0;
  bool loaded_ = false;
  uint32_t lens_[kTracks] = {};
  uint8_t mask_ = 0xF, doneMask_ = 0;
  int16_t buf_[kPiece];
};

}  // namespace tape
}  // namespace pg
