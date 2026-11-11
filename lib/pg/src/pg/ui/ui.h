// Pocket Groove user interface (docs/UX.md). Portable C++17: the same code runs
// on the device and in the host screenshot / test harness.
//
// Threading: everything here runs on the UI thread. The UI changes the
// instrument only through Engine::post(); it reads Engine::status() once per
// update() and reads the Project memory directly to draw (see project.h).
//
// No heap: a Ui is one plain object (a few KB), usually a static.
#pragma once
#include <cstdint>
#include "../engine.h"
#include "../platform.h"
#include "../project.h"
#include "format.h"

namespace pg {
namespace ui {

constexpr uint8_t kBatteryUnknown = 0xFF;

struct UiEnv {
  uint32_t nowMs;       // monotonic milliseconds (wraps; compared by difference)
  uint8_t  batteryPct;  // 0..100, or kBatteryUnknown (0xFF): empty icon, no low-battery save
  bool     sdPresent;
  uint8_t  cpuPct;      // audio CPU load, informational
};

enum class View : uint8_t { Groove, Sound, Mix, Fx, Song, Project };
enum class Overlay : uint8_t { None, Mute, Pattern, Roll, Tempo };
enum class Dialog : uint8_t { None, Confirm, Name };
enum ProjectMount : uint8_t { kPmNone = 0, kPmRom, kPmInt, kPmSd };
enum FxTab : uint8_t { kFxDuck = 0, kFxSpace, kFxMaster, kNumFxTabs };
enum StepField : uint8_t { kFVel = 0, kFGate, kFProb, kFRatch, kFAcc, kFSlide, kFChord, kFLock, kNumFields };
constexpr int kNumMixRows = 5;     // LEVEL DUCK DELAY REVERB PAN
constexpr int kNumRollRates = 4;   // 1/8 1/16 1/16T 1/32
constexpr int kMaxFileRows = 56;   // PROJECT list: ROM demos + up to 24 per mount + a "+N MORE" row each
constexpr int kListPerMount = 24;

class Ui {
 public:
  // Attaches the UI and resets all UI state; does not touch the Project, which
  // must already be valid. fs may be null (no storage: PROJECT lists the ROM demos only).
  void init(Engine& engine, Project& project, FileSystem* fs);
  // Power-on (UX 14): power-loss recovery on each mount, UI settings, resume
  // _last.pgv or else load ROM demo 0, then GROOVE stopped with the boot banner.
  // Call once after init(), before the first update(). Suspends/resumes the engine.
  void boot(uint32_t nowMs);
  // Writes the session file now if it changed (the firmware calls this before
  // power-off). Returns false on a write error. Never call it from the audio thread.
  bool saveSessionNow();

  // Key events, UI thread. The one-argument form uses the clock of the last
  // update(); the firmware should prefer passing millis() so holds are timed
  // from the key itself rather than from the last frame.
  void onKey(const KeyEvent& e) { onKey(e, now_); }
  void onKey(const KeyEvent& e, uint32_t nowMs);

  // Call ~30x/s. Polls the engine, runs timers (repeat, sweeps, erase, toasts),
  // and redraws what changed. Returns true if fb changed.
  bool update(const UiEnv& env, Framebuffer& fb);
  // Rows changed by the last update() that returned true (inclusive): the whole
  // screen after a full redraw, 0..10 when only the status bar changed. Lets the
  // firmware push a partial frame to the LCD.
  void dirtyRows(int* y0, int* y1) const { *y0 = 0; *y1 = dirtyStatusOnly_ ? 10 : kScreenH - 1; }

  // ---- read-only state, for tests and the harness -------------------------
  View view() const { return view_; }
  int selTrack() const { return sel_; }
  ValueRef focus() const { return focus_; }
  int heldStep() const { return heldStep_; }        // absolute step index or -1
  bool stepEditOpen() const;
  bool recOn() const { return recOn_; }
  bool stepWriteActive() const { return recOn_ && !playing_; }
  int stepCursor() const { return stepCursor_; }
  int shownPage() const { return shownPage_; }
  int octave(int t) const { return octave_[t & 7]; }
  int chordMode() const { return chordMode_; }
  int stepField() const { return stepField_; }
  int soloTrack() const { return solo_; }
  int rollRate() const { return rollRate_; }
  int mixRow() const { return mixRow_; }
  int fxTab() const { return fxTab_; }
  int soundPage(int t) const { return soundPage_[t & 7]; }
  int trackFocus(int t) const { return trackFocus_[t & 7]; }
  Overlay overlay() const { return overlay_; }
  bool popupVisible() const { return popUntil_ != 0; }
  const char* toastText() const { return toastUntil_ ? toast_ : ""; }
  Dialog dialog() const { return dialog_; }
  const char* nameText() const { return nameBuf_; }
  int nameMount() const { return nameMount_; }
  bool helpVisible() const;
  bool explainMode() const { return explain_; }
  bool bannerVisible() const { return banner_; }
  bool dirty() const { return dirty_; }               // unsaved changes to the named project
  bool playing() const { return playing_; }          // transport, as of the last status seen
  bool focusLocked() const { return focusLocked_; }  // focused param is p-locked on a step of its track
  const char* projectName() const { return curName_; }
  int projectMount() const { return curMount_; }
  int projectTab() const { return projTab_; }
  int projectCursor() const { return projCursor_; }
  int fileRows() const { return rowCount_; }
  int songCursor() const { return songCursor_; }
  int songLength() const;
  bool undoAvailable() const { return undoValid_; }
  int clipKind() const { return clipKind_; }        // 0 empty, 1 track, 2 pattern
  bool writeInFlight() const { return writeBusy_; }
  uint32_t droppedCommands() const { return dropped_; }
  uint32_t fullRedraws() const { return fullDraws_; }
  uint32_t statusRedraws() const { return statusDraws_; }

  // Effective (pending-aware) reads of the edit target, also used by the drawing code.
  Step stepAt(int track, int idx) const;
  int trackLen(int track) const;
  int patternLen() const;
  int paramValue(int track, int index) const;
  int globalValue(int id) const;
  bool muted(int track) const;
  int editPattern() const { return pat_; }
  SongSlot songSlot(int i) const;
  const ParamInfo* paramInfo(int track, int index) const { return eng_->trackParamInfo(track, index); }
  int noteForKey(int n) const { return noteFor(sel_, n); }   // what note key n plays on the selected track

 private:
  // ---- per-key bookkeeping: every key-up goes to whatever owned its key-down
  enum Act : uint8_t {
    kActNone = 0, kActStep, kActPad, kActNote, kActRoll, kActValue, kActSweep, kActErase, kActTab, kActHelp,
  };
  struct KeyRec {
    uint8_t act, a, b, c;   // meaning depends on act (see ui.cpp)
  };
  struct PendStep {
    uint8_t used, pat, track, idx, frame;
    Step s;
  };
  static constexpr int kPendSteps = 24;
  static constexpr int kOutbox = 96;

  // input
  void keyDown(int k, bool shift);
  void keyUp(int k);
  int heldNonModifiers() const;
  bool isHeld(int k) const { return (held_ >> k) & 1; }

  void home();
  void transport(bool shift);
  void viewKey(View v);
  void selectTrack(int t);
  void stepDown(int k, int idx16, bool shift, bool ctrl);
  void stepUp(int k);
  void padDown(int k, int t, bool shift);
  void noteDown(int k, int n, bool shift);
  void semicolonDot(int dir, bool shift);
  void pageKey(bool shift);
  void chordKey(bool shift);
  void recToggle();
  void delDown(int k);
  void valueKeyDown(int k, bool shift);
  void sweepDown(int k, bool shift);
  void fnCombo(int k);
  void optCombo(int k, bool shift);
  void altCombo(int k);
  void tabUp(int k);
  void columnKey(int n);

  // clipboard and undo (ui_edit.cpp)
  void ctrlCombo(int k, bool shift);
  void snapshotUndo(int pat, const char* label);
  void undo();
  void cutCopyTrack(bool cut);
  void cutCopyPattern(bool cut);
  void paste();
  bool postWrite(int pat, int track, int buf);   // track -1 = whole pattern
  const TrackPattern* inflight(int pat, int t) const;
  void copyPatternShadowed(int pat, Pattern& out) const;
  void eraseFinished();

  // SONG (ui_song.cpp)
  void songStepKey(int n, bool shift);
  void songDel();
  void songMove(int dir);
  void songEnter();
  void songRepeats(int delta);
  void setSongSlot(int i, int pattern, int repeats);
  void drawSong(Framebuffer& fb);

  // PROJECT, dialogs, files, boot (ui_project.cpp)
  void refreshFiles();
  void projectMove(int dir);
  void projectEnter();
  void projectDel();
  void openConfirm(uint8_t action, int row);
  void confirmKey(int k);
  void nameKey(int k);
  void openNameDialog();
  void doLoadRow(int row);
  void doSave(int mount, const char* name);
  void doDelete(int row);
  void doNew();
  void ctrlSave();
  void loadInto(int mount, int romIndex, const char* name);
  void afterProjectChange(int mount, const char* name, bool keepDirty);
  void resetForProject(int sel);
  bool patternUsed(int pat) const;
  void ioError(const char* what, uint8_t status);
  uint32_t projectCrc() const;
  void trackDirty();
  void autosave();
  void loadSettings();
  void saveSettings();
  void drawProject(Framebuffer& fb);
  void drawDialog(Framebuffer& fb);
  void drawBanner(Framebuffer& fb);

  // HELP (ui_help.cpp)
  void drawHelp(Framebuffer& fb);
  int helpTable() const;

  // value editing
  enum class Target : uint8_t { None, Focus, Tempo, Field, Lock, Song, Inert };
  Target valueTarget() const;
  bool targetIsSmall(Target t) const;
  void valueEvent(int dir, int mag, bool shift);
  void resetValue();
  void editFocus(const ValueRef& ref, int delta, bool popup);
  void editField(int dir, int mag, bool shift);
  void editLock(int dir, int mag, bool shift);
  void setValue(const ValueRef& ref, int v);
  const ParamInfo* infoFor(const ValueRef& ref) const;
  int valueOf(const ValueRef& ref) const;
  void showPopup(const ValueRef& ref);
  bool valueOnScreen(const ValueRef& ref) const;
  void toast(const char* s, uint8_t kind);   // kind 0 info, 1 undoable, 2 error

  // step helpers
  bool isSynth(int t) const { return t >= kNumDrumTracks; }
  Step newStep(int t) const;
  int noteFor(int t, int degree) const;
  uint8_t chordForDegree(int degree) const;
  void setStep(int t, int idx, const Step& s);
  void setTrackLength(int t, int len);
  void setPatternLength(int len);
  void setMute(int t, bool m);
  void heldEdited(Step& s) { s.flags |= kStepOn; heldEdited_ = true; }
  bool fieldAvailable(int t, int f) const;
  void stepEditNote(int k, int n);
  void stepWriteNote(int k, int n, bool shift);
  void playNote(int k, int n, bool shift);
  void addPage();
  void noteOn(int t, int note, int vel, uint8_t flags, uint8_t chord);
  void noteOff(int t, int note);
  void sendRoll(int t, bool on);

  // engine plumbing
  void send(const Command& c);
  void flushOutbox();
  void prunePending();
  void pollStatus();
  void runTimers();
  void runRepeat();
  void updateFocusLock();

  // drawing (ui_draw.cpp)
  void drawAll(Framebuffer& fb);
  void drawStatus(Framebuffer& fb);
  void drawGroove(Framebuffer& fb);
  void drawStepEdit(Framebuffer& fb);
  void drawSound(Framebuffer& fb);
  void drawMix(Framebuffer& fb);
  void drawFx(Framebuffer& fb);
  void drawFxDuck(Framebuffer& fb);
  void drawOverlay(Framebuffer& fb);
  void drawPopup(Framebuffer& fb);
  void drawToast(Framebuffer& fb);
  uint32_t statusSignature() const;
  uint32_t bodySignature() const;
  int playStep(int t) const;   // playhead step of track t, or -1 when stopped

  // ---- members (all plain data) --------------------------------------------
  Engine* eng_ = nullptr;
  Project* proj_ = nullptr;
  FileSystem* fs_ = nullptr;
  UiEnv env_{};
  EngineStatus st_{};
  uint32_t now_ = 0;
  uint8_t frame_ = 0;
  uint8_t pat_ = 0;
  bool playing_ = false;

  View view_ = View::Groove;
  uint8_t sel_ = 0;
  ValueRef focus_{};
  uint8_t trackFocus_[kNumTracks] = {};
  uint8_t soundPage_[kNumTracks] = {};
  uint8_t octave_[kNumTracks] = {};
  uint8_t mixRow_ = 0, fxTab_ = 0;
  uint8_t fxFocus_[kNumFxTabs] = {};
  uint8_t shownPage_ = 0;
  uint8_t chordMode_ = 0;
  uint8_t rollRate_ = 1;
  uint8_t rollMask_ = 0;
  uint8_t stepField_ = 0;
  int8_t solo_ = -1;
  uint8_t soloSaved_ = 0;

  // held step
  int8_t heldStep_ = -1;
  uint8_t heldKey_ = 0, heldTrack_ = 0, heldPattern_ = 0;
  bool heldWasOn_ = false, heldEdited_ = false, heldOther_ = false;
  uint32_t heldDownMs_ = 0;

  // record
  bool recOn_ = false;
  uint8_t stepCursor_ = 0;

  // keys
  uint64_t held_ = 0, ignored_ = 0;
  uint32_t downMs_[57] = {};
  KeyRec rec_[57] = {};
  uint8_t comboMods_ = 0;      // bit per modifier (0 fn,1 opt,2 alt,3 tab) that has been used in a combo
  uint32_t tabDownUs_ = 0;
  uint32_t taps_[5] = {};
  uint8_t tapCount_ = 0;
  Overlay overlay_ = Overlay::None;

  // -/= repeat
  int8_t repKey_ = -1;
  int8_t repDir_ = 0;
  uint32_t repStartMs_ = 0, repNextMs_ = 0;

  // [ ] sweep
  int8_t swKey_ = -1;
  int8_t swDir_ = 0;
  bool swBass_ = false;
  float swPos_ = 0.0f;
  uint32_t swNextMs_ = 0, swPauseUntil_ = 0;
  uint8_t bassReturn_ = 0;

  // erase
  bool erasing_ = false;
  uint8_t eraseLast_ = 0;

  // feedback
  char toast_[40] = {};
  uint8_t toastKind_ = 0;
  uint32_t toastUntil_ = 0;
  ValueRef popRef_{};
  uint32_t popUntil_ = 0;
  uint8_t lastTrig_[kNumTracks] = {};
  uint32_t flashUntil_[kNumTracks] = {};
  uint32_t lastTrigMs_[kNumTracks] = {};
  uint8_t lastMaster_ = 0xFF;
  uint32_t beatUntil_ = 0, lastBeatMs_ = 0;
  uint32_t clipUntil_ = 0;
  float meter_ = 0.0f;

  // pending values (shown before the engine has applied them)
  uint8_t pendTP_[kNumTracks][kParamsPerTrack] = {};
  uint8_t pendTPFrame_[kNumTracks][kParamsPerTrack] = {};
  uint32_t pendTPMask_[kNumTracks] = {};
  uint8_t pendG_[kNumGlobals] = {};
  uint8_t pendGFrame_[kNumGlobals] = {};
  uint32_t pendGMask_ = 0;
  PendStep pendSteps_[kPendSteps] = {};
  uint8_t pendTrackLen_[kNumTracks] = {};    // 0 = none
  uint8_t pendTrackLenFrame_[kNumTracks] = {};
  uint8_t pendPatLen_ = 0, pendPatLenFrame_ = 0, pendLenPat_ = 0;
  uint8_t pendMute_[kNumTracks] = {};        // 0 none, 1 unmuted, 2 muted
  uint8_t pendMuteFrame_[kNumTracks] = {};

  // outbox: commands the engine queue had no room for, retried in order
  Command outbox_[kOutbox] = {};
  uint8_t outHead_ = 0, outCount_ = 0;
  uint32_t dropped_ = 0;

  // ---- phase 2 ----
  // file identity and dirty tracking
  uint8_t curMount_ = kPmNone;
  uint8_t curRom_ = 0;
  char curName_[9] = {};
  bool dirty_ = false, sessDirty_ = false;
  uint32_t baseCrc_ = 0, sessCrc_ = 0, lastRev_ = 0;
  bool revSeen_ = false;
  uint32_t lastKeyMs_ = 0, stopMs_ = 0;
  bool wasPlaying_ = false, battSaved_ = false, settingsDirty_ = false;
  uint32_t sessRev_ = 0, autosaveRetryMs_ = 0;
  // PROJECT list
  struct FileRow { uint8_t mount, tempo, valid, status; char name[9]; };
  FileRow rows_[kMaxFileRows] = {};
  uint8_t rowCount_ = 0, projTab_ = 0, projCursor_ = 0, projTop_ = 0;
  bool listedSd_ = false, listValid_ = false;
  // dialogs
  Dialog dialog_ = Dialog::None;
  uint8_t confirmAction_ = 0;
  int8_t confirmRow_ = -1;
  char nameBuf_[9] = {};
  uint8_t nameMount_ = kPmInt;
  // clipboard / undo: snap_[undoIdx_] is the undo snapshot, the other one the write buffer
  Pattern snap_[2] = {};
  Pattern clip_ = {};               // a track clip uses clip_.tracks[0]
  uint8_t undoIdx_ = 0, undoPat_ = 0, clipKind_ = 0, clipTrack_ = 0;
  bool undoValid_ = false, undoIsRedo_ = false;
  char undoLabel_[20] = {};
  bool writeBusy_ = false;
  uint32_t writeSinceMs_ = 0;
  uint8_t writeBuf_ = 0, writePat_ = 0, writeExpect_ = 0;
  int8_t writeTrack_ = -1;
  bool eraseSnapped_ = false, eraseDid_ = false;
  // song
  struct PendSlot { uint8_t pattern, repeats, used, frame; };
  PendSlot pendSong_[kSongSlots] = {};
  uint8_t songCursor_ = 0, songTop_ = 0;
  // help / banner
  bool explain_ = false, banner_ = false;
  int8_t explainKey_ = -1;
  uint32_t helpDownMs_ = 0;

  // redraw bookkeeping
  uint32_t serial_ = 1;           // bumps on every UI state change
  uint32_t lastBodySig_ = 0, lastStatusSig_ = 0;
  bool drawn_ = false;
  // focus lock marker cache (recomputed on focus / pattern / revision / step-edit change)
  bool focusLocked_ = false;
  uint32_t lockKey_ = 0;
  bool dirtyStatusOnly_ = false;
  uint32_t fullDraws_ = 0, statusDraws_ = 0;
};

}  // namespace ui
}  // namespace pg
