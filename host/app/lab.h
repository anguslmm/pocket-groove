// The macro lab (docs/MACROLAB.md): a development workbench, off unless the
// app is started with --lab. It is where instruments (macro sets) are tuned:
// .pgm files (pg::macro, lib/pg/src/pg/macro) in a folder are watched and
// reloaded while the app plays, the selected track's instrument is shown in a
// panel right of the screen with a slider per macro and an XY pad for two of
// them, and "good" / "bad" marks append one line each to a notes file.
//
//   one truth  the sliders and the XY pad move the TRACK'S OWN macro positions
//              (Ui::setMacro, the same positions the SOUND page shows and the
//              Project stores); choosing a set in the panel is choosing the
//              track's instrument (Ui::setInstrument; "none" = CLASSIC).
//   overlay    the lab is the instrument library's overlay
//              (pg::macro::LibraryOverlay): a file whose voice and name match a
//              built-in instrument REPLACES it while the app runs (projects keep
//              naming it by its id); a file with no built-in counterpart is an
//              extra instrument for this session (it appears after the built-ins
//              in the SOUND page's list). A file without a `name` line is named
//              after the file. The engine reads the overlay on the audio thread
//              (find(): atomics only); a replaced set's memory is kept until the
//              engine has counted the RefreshInstruments the lab posted for it.
//   files      every *.pgm under the folder (any depth), polled by stat() every
//              kPollMs; changed, added and removed files take effect at once.
//              A file that does not parse keeps its last good version, and the
//              error (file:line: reason) shows in the panel and the terminal.
//              Warnings (pm::Warnings) never stop a load. On a reload the
//              tracks playing it keep their macro positions by macro name.
//   panel      the set's about line, each macro's end labels on its slider and
//              on the XY pad's axes, its description, a tick under a slider
//              wherever an enum parameter jumps, and the readout of what the set
//              drives (the effective values, the mixer rule applied).
//   marks      Cmd+G / Cmd+D (or the GOOD / BAD buttons) take a snapshot, then a
//              one-line note can be typed: Return saves the line with it, Esc
//              without. The line goes to lab-notes.txt (see notesLine()).
//
// Pure C++ except for the drawing target: the panel is drawn into a plain
// kLabW x kCanvasH buffer of palette indices (the instrument's palette and
// font), which display.cpp shows.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "display.h"
#include "pg/macro/macro.h"

namespace desk {

struct LabHost {
  virtual ~LabHost() = default;
  virtual int selectedTrack() = 0;
  // Track values as the UI shows them (pending-aware): 0..31 parameters, 32..35 macro positions 0..255.
  virtual int paramValue(int track, int index) = 0;
  virtual int projectValue(int track, int index) = 0;     // what the Project holds now (the engine's)
  virtual uint16_t instrument(int track) = 0;              // the UI's (pending-aware)
  virtual uint16_t projectInstrument(int track) = 0;
  virtual void setMacro(int track, int m, int value) = 0;  // Ui::setMacro
  virtual void setInstrument(int track, uint16_t id, const uint8_t* positions) = 0;   // Ui::setInstrument
  virtual void refreshInstruments() = 0;                   // post CmdType::RefreshInstruments (every track)
  virtual uint8_t instrumentRefreshes() = 0;               // Engine::instrumentRefreshes()
  // Bit m: macro m is locked on some step of `track` in the pattern being edited.
  virtual uint32_t lockedMask(int track) = 0;
};

class Lab final : public pg::macro::LibraryOverlay {
 public:
  static constexpr uint32_t kPollMs = 250;
  ~Lab();

  // dir: the macro folder; notesPath: where marks go. Scans once and installs
  // itself as the library overlay (pg::macro::setLibraryOverlay).
  void init(const std::string& dir, const std::string& notesPath, const pg::macro::ParamTable& pt, LabHost* host,
            uint32_t nowMs);
  bool enabled() const { return host_ != nullptr; }

  // Every UI frame: polls the folder when due, frees replaced sets the engine let go of.
  void tick(uint32_t nowMs);
  bool changed() const { return dirty_; }        // the panel needs a redraw
  void draw(uint8_t* px);                         // kLabW x kCanvasH palette indices; clears changed()

  // Mouse, panel coordinates (device pixels from the panel's top left).
  void mouseDown(int x, int y);
  void mouseMove(int x, int y);
  void mouseUp();

  // A kActLab* key action (keymap.h). shift: fine nudges.
  void action(int act, bool shift);

  // The note typed after a mark.
  bool typing() const { return typing_; }
  void typeText(const char* text);
  enum TypeKey { kTypeEnter, kTypeEscape, kTypeBackspace };
  void typeKey(TypeKey k);
  void finishTyping(bool withNote);   // also called at quit

  // ---- the overlay (pg::macro::LibraryOverlay; find() also from the audio thread) -------
  const pg::macro::MacroSet* find(uint16_t id) const override;
  int extraCount(int track) const override;
  const pg::macro::MacroSet* extra(int track, int i) const override;

  // ---- for scripts and tests --------------------------------------------------------
  // Chooses the instrument of `track` by its name, file name or path ("none" = CLASSIC).
  bool selectSet(int track, const std::string& which);
  bool setMacro(int track, int m, float pos);
  int reloads() const { return reloads_; }   // files (re)loaded or removed since init, after the first scan
  int errors() const;                         // files currently not parsing
  int warnings() const;                       // warnings of the loaded (good) versions, all files
  int retired() const { return int(retired_.size()); }   // replaced sets the engine may still hold
  // True when the engine plays what the lab and the UI asked for: no pending
  // instrument or macro change, every refresh counted, every instrument track's
  // parameters derived from its (possibly replaced) set.
  bool synced();
  std::string describe(int track);            // the notes line's body for `track`
  // Panel coordinates of macro m's slider at pos, of the XY pad at (x, y), of a button.
  void sliderPoint(int m, float pos, int* x, int* y) const;
  void padPoint(float px, float py, int* x, int* y) const;
  enum Button { kBtnPrev, kBtnNext, kBtnGood, kBtnBad, kBtnAxisX, kBtnAxisY };
  void buttonCentre(Button b, int* x, int* y) const;

 private:
  static constexpr int kSlots = 128;
  struct File {
    std::string path;           // as found (dir + relative)
    int64_t mtimeNs = 0, size = -1;
    uint32_t hash = 0;          // FNV-1a of the content last read
    uint32_t goodHash = 0;      // ... of the last good version (the marks name it)
    bool good = false;          // set holds a good version
    std::unique_ptr<pg::macro::MacroSet> set;   // the last good version (what the engine may be playing)
    std::string label;          // set name (from the file name when it has none)
    std::string err;            // "" or "line: reason" of the current content
    std::vector<std::string> warns;   // "line: reason" of the good version's warnings (pm::Warnings)
    int warnCount = 0;          // all of them (warns keeps the first kMaxWarnings)
    int slot = -1;              // in slots_ while good and published
    bool shadowed = false;      // another file (earlier in path order) has the same voice and name
    bool seen = false;
  };
  // What find() reads: id and set pointer, written by the UI thread, read by the audio thread.
  struct Slot {
    std::atomic<uint16_t> id{0};
    std::atomic<const pg::macro::MacroSet*> set{nullptr};
  };
  struct Retired {
    std::unique_ptr<pg::macro::MacroSet> set;
    uint8_t target;             // freed once Engine::instrumentRefreshes() has reached it
  };
  struct Track {
    int focus = 0;              // keyboard macro
    int axisX = 0, axisY = 1;
  };

  void scan(uint32_t nowMs, bool first);
  void scanDir(const std::string& dir, int depth);
  void load(File& f, bool first);
  void publish();                               // slots_ from files_; retire what changed; post a refresh
  File* find(const std::string& path);
  const File* fileFor(uint16_t id) const;       // the published file of that id
  const pg::macro::MacroSet* setOf(int track);  // the track's instrument (the UI's view)
  int listIndex(int track);                     // 0 = none / unknown, 1.. = instrumentAt(track, i - 1)
  void choose(int track, uint16_t id);
  void step(int dir);
  void nudge(float d);
  void mark(bool good);
  void setStatus(const std::string& s, uint8_t colour, uint32_t ms);
  float pos(int track, int m) { return float(host_->paramValue(track, 32 + m)) / 255.0f; }
  std::string valuesText(int track, bool flagHand);

  std::string dir_, notesPath_;
  pg::macro::ParamTable pt_{};
  uint8_t def_[pg::kNumTracks][pg::kParamsPerTrack] = {};
  LabHost* host_ = nullptr;
  std::vector<File> files_;
  std::vector<std::string> found_;   // scratch for scan
  Slot slots_[kSlots];
  std::vector<Retired> retired_;
  uint8_t posted_ = 0;               // refreshes posted (the engine's count when all are consumed)
  bool publishPending_ = false;
  Track tr_[pg::kNumTracks];
  std::map<uint32_t, std::vector<uint8_t>> remembered_;   // (track << 16 | id) -> positions, this session
  uint32_t now_ = 0, nextPoll_ = 0;
  int reloads_ = 0;
  bool dirty_ = true;
  std::string status_;
  uint8_t statusColour_ = 0;
  uint32_t statusUntil_ = 0;
  // mouse capture
  enum Drag { kDragNone, kDragSlider, kDragPad } drag_ = kDragNone;
  int dragSlider_ = 0;
  // marks
  bool typing_ = false;
  std::string pendingLine_, note_;
  int lastShownTrack_ = -1;
  uint32_t lastDrawHash_ = 0;
};

}  // namespace desk
