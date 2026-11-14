// PROJECT view, name entry, confirm dialogs, load/save/delete/new, dirty
// tracking, session autosave, UI settings and power-on (UX.md 10.8, 13.2-13.4, 14).
// Storage goes through pg::io (lib/pg/src/pg/io/project_io.h).
#include <cstring>

#include "../demo/demo.h"
#include "../io/project_io.h"
#include "font.h"
#include "gfx.h"
#include "keys.h"
#include "ui.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {
using io::Status;

constexpr uint32_t kAutosaveIdleMs = 10000;   // stopped, dirty and no key for 10 s (13.2)
constexpr uint32_t kAfterStopMs = 2000;       // ... or 2 s after the transport stops
constexpr uint32_t kAutosaveRetryMs = 60000;  // after a failed autosave
constexpr uint8_t kSettingsVersion = 1;

char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
Command mk(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}
const char* mountPath(int m) { return m == kPmSd ? io::kMountSd : io::kMountInt; }
void copyName(char* dst, const char* src) {
  int i = 0;
  for (; src && src[i] && i < 8; ++i) dst[i] = src[i];
  dst[i] = 0;
}

// Every io::Status as a toast (<= 37 characters).
const char* statusText(uint8_t s) {
  switch (Status(s)) {
    case Status::Ok: return "OK";
    case Status::BadName: return "INVALID NAME";
    case Status::NoMount: return "NO SD CARD";
    case Status::NotFound: return "FILE NOT FOUND";
    case Status::NoSpace: return "NOT ENOUGH SPACE";
    case Status::OpenFailed: return "CAN'T OPEN THE FILE";
    case Status::WriteFailed: return "WRITE FAILED";
    case Status::VerifyFailed: return "VERIFY FAILED - NOT SAVED";
    case Status::RenameFailed: return "FAILED - OLD FILE KEPT";
    case Status::RemoveFailed: return "DELETE FAILED";
    case Status::ReadFailed: return "READ ERROR";
    case Status::NotPgFile: return "NOT A POCKET GROOVE FILE";
    case Status::NewerFormat: return "NEEDS NEWER FIRMWARE";
    case Status::ProjectVersion: return "UNSUPPORTED PROJECT VERSION";
    case Status::Corrupt: return "FILE IS DAMAGED";
    case Status::Truncated: return "FILE IS INCOMPLETE";
    case Status::BadProject: return "PROJECT IS INVALID";
    case Status::Busy: return "BUSY - TRY AGAIN";
    case Status::DestClobbered: return "LOAD FAILED - NEW PROJECT";
    case Status::BufferTooSmall: return "INTERNAL ERROR";
  }
  return "ERROR";
}
}  // namespace

void Ui::ioError(const char* what, uint8_t status) {
  char buf[40];
  char* o = cat(buf, what);
  o = cat(o, ": ");
  const char* t = statusText(status);
  if (std::strlen(buf) + std::strlen(t) > 37) o = buf;   // drop the prefix if it would not fit
  cat(o, t);
  toast(buf, 2);
}

// ---- project identity, dirty tracking -------------------------------------------

uint32_t Ui::projectCrc() const {
  return io::crc32(0, reinterpret_cast<const uint8_t*>(proj_), sizeof(Project));
}

void Ui::afterProjectChange(int mount, const char* name, bool keepDirty) {
  curMount_ = uint8_t(mount);
  copyName(curName_, name);
  dirty_ = keepDirty;
  baseCrc_ = projectCrc();
  lastRev_ = st_.revision;
  settingsDirty_ = true;
  ++serial_;
}

// While clean, every revision change is checked against the CRC taken at the
// last load/save, so the engine's own revision bumps (resume, sanitising) and
// edits that were undone by hand don't count as changes.
void Ui::trackDirty() {
  if (dirty_ || st_.revision == lastRev_) return;
  lastRev_ = st_.revision;
  if (projectCrc() != baseCrc_) {
    dirty_ = true;
    settingsDirty_ = true;
    ++serial_;
  }
}

void Ui::resetForProject(int sel) {
  // Clear everything that referred to the previous project (13.2 Load).
  heldStep_ = -1;
  recOn_ = false;
  erasing_ = false;
  solo_ = -1;
  rollMask_ = 0;
  shownPage_ = 0;
  stepCursor_ = 0;
  songCursor_ = songTop_ = 0;
  undoValid_ = false;
  undoQueued_ = false;
  recGest_ = 0;
  clipKind_ = 0;
  outCount_ = 0;
  std::memset(pendSteps_, 0, sizeof(pendSteps_));
  std::memset(pendTPMask_, 0, sizeof(pendTPMask_));
  pendGMask_ = 0;
  std::memset(pendTrackLen_, 0, sizeof(pendTrackLen_));
  pendPatLen_ = 0;
  std::memset(pendMute_, 0, sizeof(pendMute_));
  std::memset(pendSong_, 0, sizeof(pendSong_));
  pendInstMask_ = 0;
  std::memset(pendMacMask_, 0, sizeof(pendMacMask_));
  std::memset(memo_, 0, sizeof(memo_));
  listUntil_ = 0;
  for (int k = 0; k < kNumKeyIds; ++k)
    if (rec_[k].act == kActStep || rec_[k].act == kActErase || rec_[k].act == kActMacro) rec_[k].act = kActNone;
  for (MacRep& r : macRep_) r.key = -1;
  sel_ = uint8_t(sel);
  focus_ = ValueRef{false, sel_, uint8_t(focusIndexFor(sel_))};
  view_ = View::Groove;
  pat_ = 0;
}

// ---- load / save / delete / new ---------------------------------------------------

void Ui::loadInto(int mount, int romIndex, const char* name) {
  if (writeBusy_) { toast("BUSY - TRY AGAIN", 0); return; }
  if (mount != kPmRom && !fs_) { toast("NO STORAGE", 2); return; }
  // Stop, then take the Project away from the audio thread (13.2).
  outCount_ = 0;                        // queued edits belong to the old project
  send(mk(CmdType::Stop));
  send(mk(CmdType::SetRecord));
  eng_->suspend();
  Status st = Status::Ok;
  if (mount == kPmRom) demo::build(*eng_, *proj_, romIndex);
  else st = io::loadProject(*fs_, mountPath(mount), name, *proj_);
  if (st == Status::DestClobbered) eng_->initProject(*proj_);
  if (st == Status::Ok || st == Status::DestClobbered) eng_->sanitizeProject(*proj_);
  eng_->resume();
  if (st == Status::Ok) {
    resetForProject(kTrackBass);
    curRom_ = uint8_t(romIndex);
    afterProjectChange(mount, mount == kPmRom ? demo::name(romIndex) : name, false);
    playing_ = false;
    char buf[40];
    cat(cat(buf, "LOADED "), curName_);
    toast(buf, 0);
  } else if (st == Status::DestClobbered) {
    resetForProject(0);
    afterProjectChange(kPmNone, "", false);
    toast(statusText(uint8_t(st)), 2);
  } else {
    ioError("LOAD", uint8_t(st));     // the Project is untouched
  }
  sessCrc_ = 0;                        // the session differs from the file just loaded
}

void Ui::doLoadRow(int r) {
  if (r < 0 || r >= rowCount_) return;
  const FileRow& f = rows_[r];
  if (f.status == kRowMore) { toast("LIST SHOWS 24 FILES PER DRIVE", 0); return; }
  if (f.mount != kPmRom && !f.valid) { ioError("LOAD", f.status); return; }
  int rom = 0;
  if (f.mount == kPmRom)
    for (int i = 0; i < demo::count(); ++i)
      if (std::strcmp(demo::name(i), f.name) == 0) rom = i;
  char name[9];
  copyName(name, f.name);
  loadInto(f.mount, rom, name);
}

void Ui::doSave(int mount, const char* name) {
  dialog_ = Dialog::None;
  if (!fs_) { toast("NO STORAGE", 2); return; }
  if (micBusy()) { toast("RECORDING: SAVE AFTER", 2); return; }   // the mic streams to flash now
  if (mount == kPmSd && !fs_->mounted(io::kMountSd)) { toast("NO SD CARD", 2); return; }
  // The name field mirrors the file name (13.2). The audio thread never reads it.
  char old[16];
  std::memcpy(old, proj_->name, sizeof(old));
  std::memset(proj_->name, 0, sizeof(proj_->name));
  copyName(proj_->name, name);
  const Status st = io::saveProject(*fs_, mountPath(mount), name, *proj_);
  if (st != Status::Ok) {
    std::memcpy(proj_->name, old, sizeof(old));
    ioError("SAVE", uint8_t(st));
    refreshFiles();
    return;
  }
  afterProjectChange(mount, name, false);
  char buf[40];
  cat(cat(buf, "SAVED "), name);
  toast(buf, 0);
  refreshFiles();
}

void Ui::doDelete(int r) {
  if (r < 0 || r >= rowCount_ || !fs_) return;
  const FileRow f = rows_[r];
  const Status st = io::deleteProject(*fs_, mountPath(f.mount), f.name);
  if (st != Status::Ok) {
    ioError("DELETE", uint8_t(st));
  } else {
    char buf[40];
    cat(cat(buf, "DELETED "), f.name);
    toast(buf, 0);
    if (curMount_ == f.mount && std::strcmp(curName_, f.name) == 0) {
      // The open project lost its file: it is unsaved now.
      curMount_ = kPmNone;
      dirty_ = true;
      settingsDirty_ = true;
    }
  }
  refreshFiles();
}

void Ui::doNew() {
  dialog_ = Dialog::None;
  if (writeBusy_) { toast("BUSY - TRY AGAIN", 0); return; }
  outCount_ = 0;
  send(mk(CmdType::Stop));
  send(mk(CmdType::SetRecord));
  eng_->suspend();
  eng_->initProject(*proj_);
  std::memset(proj_->name, 0, sizeof(proj_->name));
  // A new project starts with the first instrument of every track at its default
  // positions (macros/builtin.txt). The sanitiser derives their sound parameters.
  for (int t = 0; t < kNumTracks; ++t) {
    const macro::MacroSet* s = macro::instrumentAt(t, 0);
    if (!s) continue;
    setInstrumentId(proj_->tracks[t], s->id);
    for (int m = 0; m < kTrackMacros; ++m)
      proj_->tracks[t].macro[m] = m < s->numMacros ? macro::startByte(s->macros[m]) : 0;
  }
  eng_->sanitizeProject(*proj_);
  eng_->resume();
  resetForProject(0);                   // selTrack 0 on a new project (2)
  afterProjectChange(kPmNone, "", false);
  playing_ = false;
  sessCrc_ = 0;
  toast("NEW PROJECT", 0);
}

void Ui::ctrlSave() {
  if ((curMount_ == kPmInt || curMount_ == kPmSd) && io::isValidName(curName_)) {
    doSave(curMount_, curName_);
    return;
  }
  // No file yet, or the ROM demo: SAVE AS (13.2)
  view_ = View::Project;
  projTab_ = 1;
  projCursor_ = 0;
  refreshFiles();
  openNameDialog();
}

// ---- PROJECT list -------------------------------------------------------------------

void Ui::refreshFiles() {
  rowCount_ = 0;
  for (int i = 0; i < demo::count() && rowCount_ < kMaxFileRows; ++i) {
    FileRow& r = rows_[rowCount_++];
    r.mount = kPmRom;
    r.valid = 1;
    r.status = 0;
    r.tempo = uint8_t(demo::tempo(i));
    copyName(r.name, demo::name(i));
  }
  listedSd_ = env_.sdPresent;
  if (fs_) {
    io::ProjectEntry e[kListPerMount];
    for (int m = kPmInt; m <= kPmSd; ++m) {
      if (!fs_->mounted(mountPath(m))) continue;
      const int n = io::listProjects(*fs_, mountPath(m), e, kListPerMount);
      for (int i = 0; i < n && rowCount_ < kMaxFileRows; ++i) {
        FileRow& r = rows_[rowCount_++];
        r.mount = uint8_t(m);
        r.valid = e[i].valid;
        r.status = uint8_t(e[i].status);
        r.tempo = e[i].tempo;
        copyName(r.name, e[i].name);
      }
      if (n < kListPerMount) continue;
      // More files than the list holds: say so instead of truncating silently.
      char dir[12];
      cat(cat(dir, mountPath(m)), "/pg");
      struct Count { int n; };
      Count c{0};
      fs_->list(dir, [](const char* name, size_t, void* u) {
        const size_t l = std::strlen(name);
        if (l > 4 && std::strcmp(name + l - 4, ".pgv") == 0 && name[0] != '_') ++static_cast<Count*>(u)->n;
      }, &c);
      if (c.n > n && rowCount_ < kMaxFileRows) {
        FileRow& r = rows_[rowCount_++];
        r.mount = uint8_t(m);
        r.valid = 0;
        r.status = kRowMore;
        r.tempo = 0;
        char* o = cat(r.name, "+");
        o += itoa10(c.n - n > 999 ? 999 : c.n - n, o);
      }
    }
  }
  // ROM rows show the demo's tempo too
  listValid_ = true;
  const int shown = rowCount_ + (projTab_ == 1 ? 1 : 0);
  if (projCursor_ >= shown) projCursor_ = uint8_t(shown > 0 ? shown - 1 : 0);
  ++serial_;
}

void Ui::projectMove(int dir) {
  const int shown = rowCount_ + (projTab_ == 1 ? 1 : 0);
  int c = projCursor_ + (dir < 0 ? -1 : 1);
  projCursor_ = uint8_t(c < 0 ? 0 : (c >= shown ? (shown > 0 ? shown - 1 : 0) : c));
}

void Ui::projectEnter() {
  if (projTab_ == 0) {
    if (projCursor_ >= rowCount_) return;
    if (dirty_) openConfirm(kConfirmLoad, projCursor_);
    else doLoadRow(projCursor_);
    return;
  }
  if (projCursor_ == 0) { openNameDialog(); return; }
  const int r = projCursor_ - 1;
  if (r >= rowCount_) return;
  if (rows_[r].mount == kPmRom) { toast("DEMOS ARE READ-ONLY", 2); return; }
  if (rows_[r].status == kRowMore) { toast("LIST SHOWS 24 FILES PER DRIVE", 0); return; }
  openConfirm(kConfirmOverwrite, r);
}

void Ui::projectDel() {
  const int r = projTab_ == 0 ? projCursor_ : projCursor_ - 1;
  if (r < 0 || r >= rowCount_) return;
  if (rows_[r].mount == kPmRom) { toast("DEMOS CAN'T BE DELETED", 2); return; }
  if (rows_[r].status == kRowMore) return;
  openConfirm(kConfirmDelete, r);
}

// ---- dialogs ------------------------------------------------------------------------

void Ui::openConfirm(uint8_t action, int row) {
  dialog_ = Dialog::Confirm;
  confirmAction_ = action;
  confirmRow_ = int8_t(row);
  ++serial_;
}

void Ui::confirmKey(int k) {
  if (k == kKSpace) { transport(isHeld(kKShift)); return; }   // SPACE still plays/stops
  if (k == kKBacktick) { dialog_ = Dialog::None; return; }
  if (k != kKEnter) return;
  dialog_ = Dialog::None;
  switch (confirmAction_) {
    case kConfirmLoad: doLoadRow(confirmRow_); break;
    case kConfirmOverwrite:
      if (confirmRow_ >= 0 && confirmRow_ < rowCount_) {
        char name[9];
        copyName(name, rows_[confirmRow_].name);
        doSave(rows_[confirmRow_].mount, name);
      }
      break;
    case kConfirmDelete: doDelete(confirmRow_); break;
    case kConfirmNew: doNew(); break;
    case kConfirmOverwriteName: doSave(nameMount_, nameBuf_); break;
    case kConfirmReplaceSample: micConfirmed(); break;
    default: break;
  }
}

void Ui::openNameDialog() {
  dialog_ = Dialog::Name;
  nameMount_ = (curMount_ == kPmSd && fs_ && fs_->mounted(io::kMountSd)) ? uint8_t(kPmSd) : uint8_t(kPmInt);
  char sug[io::kNameBuf] = "GROOVE01";
  if (fs_) io::nextFreeName(*fs_, sug);
  copyName(nameBuf_, sug);
  ++serial_;
}

void Ui::nameKey(int k) {
  int len = int(std::strlen(nameBuf_));
  switch (k) {
    case kKBacktick: dialog_ = Dialog::None; return;
    case kKDel:
      if (len) nameBuf_[len - 1] = 0;
      return;
    case kKTab: nameMount_ = nameMount_ == kPmSd ? uint8_t(kPmInt) : uint8_t(kPmSd); return;
    case kKEnter: {
      if (!io::isValidName(nameBuf_)) { toast("TYPE A NAME", 2); return; }
      if (!fs_) { dialog_ = Dialog::None; toast("NO STORAGE", 2); return; }
      if (nameMount_ == kPmSd && !fs_->mounted(io::kMountSd)) { toast("NO SD CARD", 2); return; }
      for (int i = 0; i < demo::count(); ++i)
        if (std::strcmp(demo::name(i), nameBuf_) == 0) { toast("THAT NAME IS A DEMO", 2); return; }
      if (io::projectExists(*fs_, mountPath(nameMount_), nameBuf_)) {
        openConfirm(kConfirmOverwriteName, -1);
        return;
      }
      doSave(nameMount_, nameBuf_);
      return;
    }
    default: break;
  }
  char c = 0;
  if (k == kKSpace) c = '_';
  else if (k == kKMinus) c = '-';
  else {
    const char* l = keyLegend(k);
    if (l[0] && !l[1]) {
      if (l[0] >= 'a' && l[0] <= 'z') c = char(l[0] - 32);
      else if (l[0] >= '0' && l[0] <= '9') c = l[0];
    }
  }
  if (c && len < 8) {
    nameBuf_[len] = c;
    nameBuf_[len + 1] = 0;
  }
}

// ---- session, settings, power-on ------------------------------------------------------

void Ui::loadSettings() {
  if (!fs_) return;
  uint8_t ver = 0, b[io::kUiSettingsMax];
  size_t len = 0;
  if (io::loadUiSettings(*fs_, &ver, b, sizeof(b), &len) != Status::Ok || ver != kSettingsVersion || len < 15) return;
  if (b[0] <= 4) octave_[kTrackBass] = b[0];
  if (b[1] >= 1 && b[1] <= 6) octave_[kTrackLead] = b[1];
  if (b[2] < kNumChordModes) chordMode_ = b[2];
  if (b[3] < kNumRollRates) rollRate_ = b[3];
  if (b[4] <= kPmSd) curMount_ = b[4];
  dirty_ = b[5] != 0;
  curRom_ = b[6] < demo::count() ? b[6] : 0;
  char name[9];
  std::memcpy(name, b + 7, 8);
  name[8] = 0;
  copyName(curName_, name);
  if (curName_[0] && !io::isValidName(curName_)) curName_[0] = 0;
}

void Ui::saveSettings() {
  if (!fs_) return;
  uint8_t b[15] = {octave_[kTrackBass], octave_[kTrackLead], chordMode_, rollRate_, curMount_,
                   uint8_t(dirty_ ? 1 : 0), curRom_};
  std::memcpy(b + 7, curName_, 8);
  if (io::saveUiSettings(*fs_, kSettingsVersion, b, sizeof(b)) == Status::Ok) settingsDirty_ = false;
}

bool Ui::saveSessionNow() {
  if (!fs_ || !proj_) return false;
  if (settingsDirty_) saveSettings();
  const uint32_t crc = projectCrc();
  if (crc == sessCrc_) return true;
  const Status st = io::saveSession(*fs_, *proj_);
  if (st != Status::Ok) return false;
  sessCrc_ = crc;
  return true;
}

// Autosave: never while playing (a flash write stalls the audio task on the
// device); 2 s after a stop, or after 10 s without a key while stopped. The
// battery warning (16.8) saves once whatever the transport does.
void Ui::autosave() {
  if (!fs_) return;
  if (wasPlaying_ && !playing_) stopMs_ = now_ | 1u;
  wasPlaying_ = playing_;
  if (!battSaved_ && env_.batteryPct <= 5) {   // kBatteryUnknown (0xFF) never triggers
    battSaved_ = true;
    if (saveSessionNow()) toast("BATTERY LOW - SAVED", 2);
    return;
  }
  if (playing_ || dialog_ != Dialog::None || micBusy()) return;   // a recording streams to flash: no erase now
  if (int32_t(now_ - autosaveRetryMs_) < 0) return;
  const bool afterStop = stopMs_ && int32_t(now_ - (stopMs_ + kAfterStopMs)) >= 0 &&
                         int32_t(lastKeyMs_ - stopMs_) <= 0;
  const bool idle = int32_t(now_ - (lastKeyMs_ + kAutosaveIdleMs)) >= 0;
  if (!afterStop && !idle) return;
  if (afterStop) stopMs_ = 0;
  if (st_.revision == sessRev_ && !settingsDirty_) return;   // nothing new since the last check
  sessRev_ = st_.revision;
  if (!saveSessionNow()) {
    autosaveRetryMs_ = now_ + kAutosaveRetryMs;
    toast("AUTOSAVE FAILED", 2);
  }
}

void Ui::boot(uint32_t nowMs) {
  now_ = nowMs;
  lastKeyMs_ = nowMs;
  if (fs_) {
    io::recoverAfterPowerLoss(*fs_, io::kMountInt);
    if (fs_->mounted(io::kMountSd)) io::recoverAfterPowerLoss(*fs_, io::kMountSd);
  }
  loadSettings();
  const uint8_t mount = curMount_;
  char name[9];
  copyName(name, curName_);
  const bool wasDirty = dirty_;
  eng_->suspend();
  Status st = fs_ ? io::loadSession(*fs_, *proj_) : Status::NotFound;
  if (st != Status::Ok) demo::build(*eng_, *proj_, 0);   // first boot or a bad session: the demo
  eng_->sanitizeProject(*proj_);
  eng_->resume();
  // Stopped, BASS selected, the focus on its macro 1 (UX.md 14.2): in the demo
  // ACID's OPEN, which pattern 1 leaves to the player while BITE moves by
  // itself; on a CLASSIC bass (an old session) its CUT, as before instruments.
  resetForProject(kTrackBass);
  if (st == Status::Ok) {
    if (!name[0] && io::isValidName(proj_->name)) copyName(name, proj_->name);
    afterProjectChange(mount, name, wasDirty);
    sessCrc_ = projectCrc();
  } else {
    curRom_ = 0;
    afterProjectChange(kPmRom, demo::name(0), false);
    sessCrc_ = 0;
  }
  settingsDirty_ = false;                               // just loaded them
  sessRev_ = 0xFFFFFFFFu;
  banner_ = true;
  ++serial_;
}

// ---- drawing ------------------------------------------------------------------------

void Ui::drawProject(Framebuffer& fb) {
  Gfx g(fb);
  if (!listValid_) refreshFiles();
  int x = g.chip(4, 14, "PROJECT", TEXT);
  static const char* const kTabs[2] = {"LOAD", "SAVE AS"};
  for (int i = 0; i < 2; ++i) {
    if (i == projTab_) {
      x = g.chip(x + 6, 14, kTabs[i], WHITE);
    } else {
      g.text(x + 8, 15, kTabs[i], DIM);
      x += int(std::strlen(kTabs[i])) * 6 + 9;
    }
  }
  if (env_.sdPresent) g.textR(235, 15, "SD " PG_ICON_CHECK, CYAN);
  else g.textR(235, 15, "NO SD", DIM);
  const int shown = rowCount_ + (projTab_ == 1 ? 1 : 0);
  if (projCursor_ < projTop_) projTop_ = projCursor_;
  if (projCursor_ >= projTop_ + 9) projTop_ = uint8_t(projCursor_ - 8);
  for (int i = 0; i < 9 && projTop_ + i < shown; ++i) {
    const int row = projTop_ + i;
    const int y = 28 + i * 10;
    const bool cur = row == projCursor_;
    if (cur) g.rrect(2, y - 1, 236, 9, WHITE);
    if (projTab_ == 1 && row == 0) {
      g.text(30, y, "+ NEW NAME...", cur ? BG : TEXT);
      continue;
    }
    const FileRow& f = rows_[row - (projTab_ == 1 ? 1 : 0)];
    static const char* const kTag[4] = {"", "ROM", "INT", "SD"};
    static const uint8_t kCol[4] = {DIM, GREEN, AMBER, CYAN};
    static const uint8_t kColD[4] = {LINE, GREEN_D, AMBER_D, CYAN_D};
    g.text(6, y, kTag[f.mount & 3], cur ? kColD[f.mount & 3] : kCol[f.mount & 3]);
    if (f.status == kRowMore) {
      char more[32];
      cat(cat(more, f.name), " MORE NOT SHOWN");
      g.text(30, y, more, cur ? BG : DIM);
      continue;
    }
    g.text(30, y, f.name, cur ? BG : (f.valid ? TEXT : DIM));
    char t[8];
    if (f.valid) itoa10(f.tempo, t);
    else cat(t, "BAD");
    g.textR(214, y, t, cur ? LINE : DIM);
    g.textR(232, y, "BPM", cur ? LINE : RAISED);
  }
  if (projTab_ == 0) {
    x = g.text(4, 127, "ENTER", TEXT);
    x = g.text(x, 127, " LOAD  ", DIM);
    x = g.text(x, 127, "DEL", TEXT);
    x = g.text(x, 127, " DELETE  ", DIM);
    x = g.text(x, 127, "\\", TEXT);
    g.text(x, 127, " SAVE TAB", DIM);
  } else {
    x = g.text(4, 127, "ENTER", TEXT);
    x = g.text(x, 127, " SAVE / OVERWRITE  ", DIM);
    x = g.text(x, 127, "\\", TEXT);
    g.text(x, 127, " LOAD TAB", DIM);
  }
}

void Ui::drawDialog(Framebuffer& fb) {
  Gfx g(fb);
  const int x = 30, y = 34, w = 180, h = 67;
  auto box = [&](const char* title) {
    g.rect(x - 2, y - 2, w + 4, h + 4, BG);
    g.rrect(x, y, w, h, PANEL);
    g.rframe(x, y, w, h, AMBER);
    g.rect(x + 1, y + 1, w - 2, 11, AMBER);
    g.text(x + 6, y + 3, title, BG);
  };
  if (dialog_ == Dialog::Name) {
    box("SAVE AS");
    g.rect(x + 8, y + 18, w - 16, 20, BG);
    const int xx = g.text(x + 13, y + 21, nameBuf_, WHITE, 2);
    g.rect(xx, y + 21, 10, 14, AMBER);   // steady cursor block (11: no blink)
    g.text(x + 8, y + 43, "TAB", TEXT);
    g.text(x + 30, y + 43, "SAVE TO", DIM);
    static const char* const kM[2] = {"INT", "SD"};
    for (int i = 0; i < 2; ++i) {
      if (nameMount_ == (i ? kPmSd : kPmInt)) g.chip(x + 78 + i * 26, y + 42, kM[i], i ? CYAN : AMBER);
      else g.text(x + 80 + i * 26, y + 43, kM[i], DIM);
    }
    g.text(x + 8, y + 55, "ENTER", TEXT);
    g.text(x + 44, y + 55, "SAVE", DIM);
    g.text(x + 86, y + 55, "ESC", TEXT);
    g.text(x + 110, y + 55, "CANCEL", DIM);
    return;
  }
  char title[32], l1[40];
  const char* cur = curName_[0] ? curName_ : "THIS PROJECT";
  const char* row = (confirmRow_ >= 0 && confirmRow_ < rowCount_) ? rows_[confirmRow_].name : nameBuf_;
  const char* yes = "";
  l1[0] = 0;
  switch (confirmAction_) {
    case kConfirmLoad:
      cat(cat(cat(title, "LOAD "), row), "?");
      cat(cat(l1, cur), " HAS UNSAVED CHANGES");
      yes = "ENTER  DISCARD AND LOAD";
      break;
    case kConfirmOverwrite:
    case kConfirmOverwriteName:
      cat(cat(cat(title, "OVERWRITE "), row), "?");
      cat(l1, "THE OLD FILE IS REPLACED");
      yes = "ENTER  OVERWRITE";
      break;
    case kConfirmDelete:
      cat(cat(cat(title, "DELETE "), row), "?");
      cat(l1, "THIS CAN'T BE UNDONE");
      yes = "ENTER  DELETE";
      break;
    case kConfirmReplaceSample: {
      char* o = cat(title, "REPLACE SLOT ");
      o += itoa10(confirmRow_ + 1, o);
      cat(o, "?");
      sampler::SampleRef r;
      sampler::resolve(samplePool(), confirmRow_, &r);
      cat(cat(l1, r.name), " IS ERASED");
      yes = "ENTER  ERASE";
      break;
    }
    default:
      cat(title, "NEW PROJECT?");
      cat(cat(l1, cur), " HAS UNSAVED CHANGES");
      yes = "ENTER  DISCARD";
      break;
  }
  box(title);
  g.text(x + 8, y + 18, l1, TEXT);
  g.text(x + 8, y + 18 + 22, yes, WHITE);
  g.text(x + 8, y + 18 + 33, "ESC    CANCEL", DIM);
}

void Ui::drawBanner(Framebuffer& fb) {
  Gfx g(fb);
  const int x = 30, y = 30, w = 180, h = 44;
  g.rect(x - 2, y - 2, w + 4, h + 4, BG);
  g.rrect(x, y, w, h, PANEL);
  g.rframe(x, y, w, h, GREEN);
  g.textC(x + w / 2, y + 6, "POCKET GROOVE", WHITE, 2);
  g.text(x + 16, y + 26, "SPACE", GREEN);
  g.text(x + 52, y + 26, "PLAY", TEXT);
  g.text(x + 100, y + 26, "/", GREEN);
  g.text(x + 112, y + 26, "ALL KEYS", TEXT);
  g.textC(x + w / 2, y + 35, "1-8 Q-I STEPS   A-K PADS", DIM);
}

}  // namespace ui
}  // namespace pg
