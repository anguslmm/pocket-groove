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
#include "../macro/macro.h"
#include "../platform.h"
#include "../project.h"
#include "format.h"
#include "ghost.h"
#include "scope.h"

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
// Track value indices: 0..31 are the track's parameters (params.h), 32..35 its
// instrument's macros (ValueRef::index, trackFocus, and lockParam = index + 1).
constexpr int kMacroIndex0 = kParamsPerTrack;
inline bool isMacroIndex(int i) { return i >= kMacroIndex0 && i < kMacroIndex0 + kTrackMacros; }
// The instrument page's picture (UX.md 10.3): the scope's box, inclusive rows.
constexpr int kScopeX0 = 4, kScopeY0 = 29, kScopeY1 = 72;
static_assert(Scope::kCols == kScreenW - 2 * kScopeX0, "one trace value per column of the box");

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
  // from the key itself rather than from the last frame. Every matrix event
  // first goes through the phantom-key filter (ghost.h, UX.md 16.1); a down it
  // holds back is delivered by the next event or the next update().
  void onKey(const KeyEvent& e) { onKey(e, now_); }
  void onKey(const KeyEvent& e, uint32_t nowMs);
  // The phantom-key filter is on after init(). A host whose keyboard has no
  // matrix (the desktop build) switches it off right after init().
  void setGhostFilter(bool on) { ghost_.setEnabled(on); }

  // Call ~30x/s. Polls the engine, runs timers (repeat, sweeps, erase, toasts),
  // and redraws what changed. Returns true if fb changed.
  bool update(const UiEnv& env, Framebuffer& fb);
  // Rows changed by the last update() that returned true (inclusive): the whole
  // screen after a full redraw, 0..10 when only the status bar changed, the
  // scope's band (with the status bar above it, if that changed too) when only
  // the instrument page's picture moved. Lets the firmware push a partial frame.
  void dirtyRows(int* y0, int* y1) const { *y0 = dirtyY0_; *y1 = dirtyY1_; }
  const Scope& scope() const { return scope_; }    // the instrument page's picture (tests)
  bool macroMoving(int m) const;                   // macro m of the selected track is being moved now

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
  // Instruments (UX.md 10.3). Pending-aware like paramValue().
  uint16_t instrumentIdOf(int t) const;
  const macro::MacroSet* instrumentOf(int t) const;   // nullptr: CLASSIC or an id the library lacks
  int macroValue(int t, int m) const;                  // position 0..255
  // A step's own value of track value `index` (UX.md 7): on a CLASSIC track its
  // parameter lock, on an instrument track the note's value of that macro
  // (index kMacroIndex0 + m); -1 = none.
  int stepOwnValue(int t, const Step& s, int index) const;
  // Motion (UX.md 10.3): bit m = a note of track t in the shown pattern has its own
  // value of macro m. Where the page shows macro m: the engine's heard position
  // while the playing pattern's motion moves it, else macroValue().
  uint8_t motionMask(int t) const;
  int shownMacro(int t, int m) const;
  bool macroLive(int t, int m) const { return playing_ && ((st_.live[t & 7] >> (m & 3)) & 1u); }
  bool instrumentPage() const;     // SOUND shows the player's page (instrument / classic), not parameter pages
  bool listVisible() const { return listUntil_ != 0; }   // the instrument list overlay
  // Developer mode (desktop --dev / --lab): SOUND can also show the bare parameter
  // pages (CLASSIC tracks show them, `l` flips between them and the player's page).
  void setDevMode(bool on) { devMode_ = on; devFlip_ = false; ++serial_; }
  bool devMode() const { return devMode_; }
  int trackFocus(int t) const { return trackFocus_[t & 7]; }
  Overlay overlay() const { return overlay_; }
  bool popupVisible() const { return popUntil_ != 0; }
  const char* toastText() const { return toastUntil_ ? toast_ : ""; }
  Dialog dialog() const { return dialog_; }
  const char* nameText() const { return nameBuf_; }
  int nameMount() const { return nameMount_; }
  bool helpVisible() const;
  bool explainMode() const { return explain_; }
  // The key-map label of key k in the table the help would show now (out[3], "" for none).
  void helpKeyLabel(int k, char* out) const;
  int explainKey() const { return explainKey_; }      // the key explain mode describes, or -1
  // Explain mode's text for that key, as pressed (with the modifier held then):
  // title (title[40], shown up to 30 chars) and two lines. Empty when no key.
  void explainText(char* title, const char** l1, const char** l2) const;
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
  uint64_t heldKeys() const { return held_; }        // keys the UI considers down (bit = key id)
  const GhostFilter& ghostFilter() const { return ghost_; }
  const EngineStatus& engineStatus() const { return st_; }   // as of the last update()

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

  // Sets a track parameter (0..31) the way the UI's own value keys do: clamped to its
  // range, shadowed until the engine has it (so paramValue() and the SOUND page show it at
  // once), coalesced, through Engine::post. Nothing is sent when the value is unchanged.
  // For host tools that drive parameters (the desktop macro lab, docs/MACROLAB.md).
  void setTrackParam(int track, int index, int value);
  // The same for a macro position (0..255) of the track's instrument, and for the
  // instrument itself (id 0 = CLASSIC; positions nullptr = the set's defaults).
  void setMacro(int track, int m, int value);
  void setInstrument(int track, uint16_t id, const uint8_t* positions = nullptr);

  // ---- tilt (UX.md 10.3 "Tilt") --------------------------------------------
  // The host owns G0 (the firmware: volume, diagnostics) and reports the gesture
  // here: tiltPress at G0 down, tiltInput with the two axes (X roll, Y pitch, each
  // -1..1 after the control's response, pg/tilt/tilt.h) as often as it has them,
  // tiltRelease at G0 up, tiltCancel when G0 + a volume key takes the hold. A hold
  // shorter than kTiltEngageMs (a tap) never moves anything; after that the
  // selected track's tilt macros follow the axes as a momentary offset over what
  // plays (engine.h "TILT OFFSET"); release or cancel glides them back.
  struct TiltSpec {
    int8_t track;          // the selected track
    int8_t macro[2];       // per axis (0 = X, 1 = Y): the macro it moves, -1 = none
    uint8_t response[2];   // per axis: pg::tilt::Response (the reader's presets)
    bool active;           // an instrument with at least one axis assigned
  };
  TiltSpec tiltSpec() const;          // what a press now would drive
  void tiltPress(uint32_t nowMs);
  void tiltInput(float x, float y, uint32_t nowMs);
  void tiltRelease(uint32_t nowMs);
  void tiltCancel(uint32_t nowMs);
  bool tiltHeld() const { return tilt_.down && !tilt_.cancelled; }   // the host should read the axes
  bool tiltEngaged() const { return tilt_.engaged; }                  // moving the macros now
  int tiltTrack() const { return tilt_.engaged ? tilt_.track : -1; }
  float tiltAxis(int a) const { return tilt_.in[a & 1]; }            // the last input
  static constexpr uint32_t kTiltEngageMs = 150;   // shorter: a tap (G0 double-tap), never audible
  static constexpr uint32_t kTiltRampMs = 15;      // each new input glides there (inputs every ~10 ms)
  static constexpr uint32_t kTiltReturnMs = 150;   // release / cancel: the glide back
  static constexpr uint32_t kTiltKeepMs = 100;     // an unchanged input is re-sent this often (recording)
  static constexpr int kTiltScale = 160;           // position units (of 255) at an axis's full range

 private:
  // ---- per-key bookkeeping: every key-up goes to whatever owned its key-down
  enum Act : uint8_t {
    kActNone = 0, kActStep, kActPad, kActNote, kActRoll, kActValue, kActSweep, kActErase, kActTab, kActHelp,
    kActMacro,
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
  void keyEvent(const KeyEvent& e, uint32_t nowMs);   // after the ghost filter
  void keyDown(int k, bool shift);
  void keyUp(int k);
  bool isHeld(int k) const { return (held_ >> k) & 1; }

  void home();
  void transport(bool shift);
  void viewKey(View v);
  void selectTrack(int t);
  void stepDown(int k, int idx16, bool shift, bool ctrl);
  void stepUp(int k);
  void padDown(int k, int t, bool shift);
  void noteDown(int k, int n, bool shift);
  void semicolonDot(int dir, bool octave);
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
  // instruments (ui.cpp)
  const ParamInfo* trackInfo(int t, int index) const;   // params and macros
  const ParamInfo* macroInfo(int t, int m) const;
  int focusIndexFor(int t) const;
  void refocus(int t);
  void macroKeyDown(int k, int n, bool shift);
  void macroEvent(int m, int dir, int mag, bool shift);
  void runMacroRepeats();
  void stepInstrument(int dir);
  void snapshotInstrument(int t);
  // tilt (ui_tilt.cpp)
  void tiltEngage();
  void tiltSend(bool force);
  void tiltLetGo(uint32_t nowMs);
  bool tiltShows(int t, int m) const;   // the page shows macro m of track t where the engine has it
  uint8_t tiltMarks(int t, int m) const;   // bit a: tilt axis a moves macro m of track t's instrument

  // clipboard and undo (ui_edit.cpp)
  void ctrlCombo(int k, bool shift);
  // gest: the gesture the snapshot belongs to (0 = only this key event / frame), see ui_edit.cpp.
  void snapshotUndo(int pat, const char* label, uint32_t gest = 0);
  uint32_t gestureNow() const;
  bool undoCovers(uint32_t gest) const;
  void undoForStep(int t, int idx, const char* what);
  void undoForRecording(const char* label = "RECORDED NOTES");
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
  int helpTable(bool layers = true) const;   // layers: a held modifier shows its layer

  // value editing
  enum class Target : uint8_t { None, Focus, Tempo, Field, Lock, Song, Inert };
  Target valueTarget() const;
  bool targetIsSmall(Target t) const;
  void valueEvent(int dir, int mag, bool shift);
  void resetValue();
  void editFocus(const ValueRef& ref, int delta, bool popup);
  void editField(int dir, int mag, bool shift);
  void editLock(int dir, int mag, bool shift);
  void stepMacroEdit(int t, Step& s, int m, int dir, int mag, bool shift);
  void clearMotion(int m);
  bool clearStepOwn(int t, Step& s) const;
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
  void heldEdited(Step& s) { s.addFlags(kStepOn); heldEdited_ = true; }
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
  void drawInstrument(Framebuffer& fb);
  void drawInstrumentList(Framebuffer& fb);
  void drawScope(Framebuffer& fb);
  bool scopeShown() const;
  bool scopeCovered() const;
  void refreshJumps();
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
  uint64_t held_ = 0;
  GhostFilter ghost_;
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

  // instruments
  bool devMode_ = false, devFlip_ = false;
  uint16_t pendInst_[kNumTracks] = {};
  uint8_t pendInstFrame_[kNumTracks] = {};
  uint8_t pendInstMask_ = 0;
  uint8_t pendMac_[kNumTracks][kTrackMacros] = {};
  uint8_t pendMacFrame_[kNumTracks][kTrackMacros] = {};
  uint8_t pendMacMask_[kNumTracks] = {};
  struct MacRep { int8_t key, dir; uint32_t startMs, nextMs; };
  MacRep macRep_[kTrackMacros] = {{-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}};
  struct Memo { uint16_t id; uint8_t track, used; uint8_t pos[kTrackMacros]; };   // positions per instrument, this session
  static constexpr int kMemos = 16;
  Memo memo_[kMemos] = {};
  uint8_t memoNext_ = 0;
  uint32_t listUntil_ = 0;
  uint32_t macMoveMs_[kTrackMacros] = {};   // last move of each macro of the selected track (0 = none)
  Scope scope_;
  // where each macro of the shown instrument makes an enum jump (ticks), 0..255
  static constexpr int kMaxTicks = 6;
  uint8_t tickPos_[kTrackMacros][kMaxTicks] = {};
  uint8_t tickN_[kTrackMacros] = {};
  uint16_t tickId_ = 0;                     // instrument the ticks belong to
  uint8_t tickTrack_ = 0xFF;
  uint8_t tickKey_[kTrackMacros][kTrackMacros] = {};   // the positions they were computed at
  uint8_t tickValid_ = 0;                   // bit per macro
  mutable ParamInfo macInfo_[4] = {};   // macroInfo() results (a ring: a frame needs at most a few at once)
  mutable uint8_t macInfoNext_ = 0;
  // tilt (ui_tilt.cpp)
  struct TiltState {
    bool down, cancelled, engaged, moved, recSnap;
    uint8_t track;
    int8_t macro[2];
    int16_t sent[2];        // the offsets last sent (1/16 units)
    float in[2];            // the last input
    uint32_t downMs, sentMs, showUntil;
  };
  TiltState tilt_ = {};

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
  uint16_t clipInst_[kNumTracks] = {};   // each clipped track's instrument id when it was copied
  bool undoValid_ = false, undoIsRedo_ = false;
  // an instrument change is undone without a pattern snapshot (ui_edit.cpp)
  uint8_t undoKind_ = 0, undoTrack_ = 0;   // 0 pattern, 1 instrument
  uint16_t undoInst_ = 0;
  uint8_t undoPos_[kTrackMacros] = {};
  uint8_t undoParams_[kSoundParams] = {};
  char undoLabel_[20] = {};
  // undo units (ui_edit.cpp): ids from one counter, 0 = none
  uint32_t gestSeq_ = 0, evtGest_ = 0, heldGest_ = 0, eraseGest_ = 0, recGest_ = 0;
  uint32_t undoGest_ = 0, undoEvt_ = 0;   // what the current snapshot covers
  bool undoQueued_ = false;               // CTRL+Z while a write was in flight: runs when it lands
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
  uint8_t explainTable_ = 0;      // helpTable() when explainKey_ went down
  uint32_t helpDownMs_ = 0;

  // redraw bookkeeping
  uint32_t serial_ = 1;           // bumps on every UI state change
  uint32_t lastBodySig_ = 0, lastStatusSig_ = 0;
  bool drawn_ = false;
  // focus lock marker cache (recomputed on focus / pattern / revision / step-edit change)
  bool focusLocked_ = false;
  uint32_t lockKey_ = 0;
  bool dirtyStatusOnly_ = false;
  uint8_t dirtyY0_ = 0, dirtyY1_ = kScreenH - 1;
  uint32_t fullDraws_ = 0, statusDraws_ = 0;
};

}  // namespace ui
}  // namespace pg
