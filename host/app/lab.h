// The macro lab (docs/MACROLAB.md): a development workbench, off unless the
// app is started with --lab. It is how macro mappings get tried: .pgm files
// (pg::macro, lib/pg/src/pg/macro) in a folder are watched and reloaded while
// the app plays, the selected track's set is shown in a panel right of the
// screen with a slider per macro and an XY pad for two of them, and "good" /
// "bad" marks append one line each to a notes file.
//
//   files    every *.pgm under the folder (any depth), polled by stat() every
//            kPollMs; changed, added and removed files take effect at once.
//            A file that does not parse keeps its last good version, and the
//            error (file:line: reason) shows in the panel and the terminal.
//            Warnings (pm::Warnings: a plain `set` that wipes out an earlier
//            line, an unused patch) never stop a load; the terminal prints
//            them at every load and the status line shows the active set's.
//   panel    the set's about line under its name, each macro's end labels on
//            its slider and on the XY pad's axes, its description (the
//            keyboard macro's) in the idle status line, and a tick under a
//            slider wherever moving it makes an enum parameter jump.
//   apply    while a set is active on a track, its owned parameters follow
//            the macros: evaluated at most once per UI frame and only values
//            that changed are sent, through LabHost::setParam (Ui::setTrackParam,
//            the UI's own value path). A parameter edited by hand keeps the
//            hand value until the set computes a different value for it (the
//            macro that drives it moves, or the file is reloaded). Choosing
//            "none" (or another set) puts back the values the track had when
//            the set was chosen. Sets and macro positions are remembered per
//            track and file for the session (not in projects, not on disk).
//   marks    Cmd+G / Cmd+D (or the GOOD / BAD buttons) take a snapshot, then a
//            one-line note can be typed: Return saves the line with it, Esc
//            without. The line goes to lab-notes.txt (see notesLine()).
//
// Pure C++ except for the drawing target: the panel is drawn into a plain
// kLabW x kCanvasH buffer of palette indices (the instrument's palette and
// font), which display.cpp shows.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "display.h"
#include "pg/macro/macro.h"

namespace desk {

struct LabHost {
  virtual ~LabHost() = default;
  virtual int selectedTrack() = 0;
  virtual int paramValue(int track, int index) = 0;            // what the UI shows (pending-aware)
  virtual void setParam(int track, int index, int value) = 0;   // Ui::setTrackParam
  // Parameters (bit = index) p-locked on some step of `track` in the pattern being edited.
  virtual uint32_t lockedMask(int track) = 0;
};

class Lab {
 public:
  static constexpr uint32_t kPollMs = 250;

  // dir: the macro folder; notesPath: where marks go. Scans once.
  void init(const std::string& dir, const std::string& notesPath, const pg::macro::ParamTable& pt, LabHost* host,
            uint32_t nowMs);
  bool enabled() const { return host_ != nullptr; }

  // Every UI frame: polls the folder when due, applies the active sets.
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

  // ---- for scripts and tests --------------------------------------------------------
  // Selects a set on `track` by its name, file name or path ("none" = bare parameters).
  bool selectSet(int track, const std::string& which);
  bool setMacro(int track, int m, float pos);
  int reloads() const { return reloads_; }   // files (re)loaded or removed since init, after the first scan
  int errors() const;                         // files currently not parsing
  int warnings() const;                       // warnings of the loaded (good) versions, all files
  // True when every active set's last sent values are what the host reports.
  bool synced();
  std::string describe(int track);            // the notes line's body for `track`
  // Panel coordinates of macro m's slider at pos, of the XY pad at (x, y), of a button.
  void sliderPoint(int m, float pos, int* x, int* y) const;
  void padPoint(float px, float py, int* x, int* y) const;
  enum Button { kBtnPrev, kBtnNext, kBtnGood, kBtnBad, kBtnAxisX, kBtnAxisY };
  void buttonCentre(Button b, int* x, int* y) const;

 private:
  struct File {
    std::string path;           // as found (dir + relative)
    int64_t mtimeNs = 0, size = -1;
    uint32_t hash = 0;          // FNV-1a of the content last read
    uint32_t goodHash = 0;      // ... of the last good version (the marks name it)
    bool good = false;          // set holds a good version
    pg::macro::MacroSet set;
    std::string label;          // set name, or the file name in capitals
    std::string err;            // "" or "line: reason" of the current content
    std::vector<std::string> warns;   // "line: reason" of the good version's warnings (pm::Warnings)
    int warnCount = 0;          // all of them (warns keeps the first kMaxWarnings)
    bool seen = false;
  };
  struct Track {
    std::string path;           // the active set's file, "" = none
    float pos[pg::macro::kMaxMacros] = {};
    uint8_t under[pg::kParamsPerTrack] = {};   // values when the set was chosen
    uint8_t sent[pg::kParamsPerTrack] = {};
    uint32_t sentMask = 0;      // params whose sent[] is valid
    uint32_t owned = 0;
    bool force = false;         // send every owned value on the next apply
    int focus = 0;              // keyboard macro
    int axisX = 0, axisY = 1;
  };
  struct Saved {
    std::string names[pg::macro::kMaxMacros];
    float pos[pg::macro::kMaxMacros] = {};
    int n = 0;
  };

  void scan(uint32_t nowMs, bool first);
  void scanDir(const std::string& dir, int depth);
  void load(File& f, bool first);
  File* find(const std::string& path);
  std::vector<int> setsFor(int track) const;   // indices into files_, sorted by path
  void activate(int track, int fileIdx);       // -1 = none
  void deactivate(int track);
  void apply(int track);
  void savePositions(int track);
  void reconcile(int track, const pg::macro::MacroSet& oldSet, bool hadOld);
  void step(int dir);
  void nudge(float d);
  void mark(bool good);
  void setStatus(const std::string& s, uint8_t colour, uint32_t ms);
  std::string valuesText(int track, bool flagHand);

  std::string dir_, notesPath_;
  pg::macro::ParamTable pt_{};
  LabHost* host_ = nullptr;
  std::vector<File> files_;
  std::vector<std::string> found_;   // scratch for scan
  Track tr_[pg::kNumTracks];
  std::map<std::string, Saved> saved_;
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
