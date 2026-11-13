// Clipboard and one-level undo/redo (UX.md 13.1).
//
// Buffers (all members of Ui, no heap):
//   snap_[undoIdx_]      the undo snapshot: a whole Pattern, taken BEFORE a destructive action
//   snap_[1 - undoIdx_]  the write buffer for WriteTrack / WritePattern (paste, undo, redo)
//   clip_                the clipboard (a track clip lives in clip_.tracks[0])
// Invariant: a buffer the engine may still read (writeBusy_) is never the undo
// snapshot, and only one write is in flight; it ends when status.writesDone
// reaches writeExpect_.
//
// What is undoable: every change to a pattern's steps and lengths the player
// makes (step edits through setStep, live recording, the bulk actions), and a
// change of a track's instrument (an instrument snapshot: the track's id, macro
// positions and sound parameters, so CLASSIC comes back exactly; a run of
// instrument steps while the list is up is one unit). Macro moves, sound and
// mixer values, the SONG chain and project load/new are not.
//
// One gesture = one undo unit. Each snapshot remembers the key event / frame
// it was taken in (undoEvt_) and the gesture it belongs to (undoGest_); a step
// write takes a NEW snapshot unless the current one is for the same pattern
// AND (was taken in the same key event or frame, OR belongs to the gesture in
// progress). The gestures (gestureNow()):
//   - a step key held down: everything from key-down to key-up (placing it,
//     every nudge of its note / fields / lock, copies to other steps, the
//     delete on a tap's release) is one unit, however many writes;
//   - DEL held while playing (erase): the whole pass;
//   - a recording pass (REC on and playing): from the first recorded note
//     until REC goes off or the transport stops (undoForRecording);
//   - anything else: one key event (e.g. a step-write note plus the slide it
//     sets on the previous step).
// Any other snapshot in between (another step, another pattern, a bulk
// action, an undo/redo) starts a new unit, so the older one is gone (one level).
// A bulk action's own snapshot covers the step writes of the same key event
// only, never a gesture.
// Cost: one copyPatternShadowed (sizeof(Pattern) = 4168 bytes plus the pending
// overlay) on the UI task, at most once per gesture.
#include <cstring>

#include "keys.h"
#include "ui.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {
char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
char* catP2(char* o, int n) {
  *o++ = char('0' + (n / 10) % 10);
  *o++ = char('0' + n % 10);
  *o = 0;
  return o;
}
Command mk(CmdType t, int a, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}
}  // namespace

// The pattern as the UI shows it: Project + the in-flight write + pending edits.
void Ui::copyPatternShadowed(int pat, Pattern& out) const {
  std::memcpy(&out, &proj_->patterns[pat], sizeof(Pattern));
  if (writeBusy_ && writePat_ == pat && &snap_[writeBuf_] != &out) {
    if (writeTrack_ < 0) std::memcpy(&out, &snap_[writeBuf_], sizeof(Pattern));
    else std::memcpy(&out.tracks[writeTrack_], &snap_[writeBuf_].tracks[writeTrack_], sizeof(TrackPattern));
  }
  for (const PendStep& p : pendSteps_)
    if (p.used && p.pat == pat) out.tracks[p.track].steps[p.idx] = p.s;
  if (pendLenPat_ == pat) {
    for (int t = 0; t < kNumTracks; ++t)
      if (pendTrackLen_[t]) out.tracks[t].length = pendTrackLen_[t];
    if (pendPatLen_) out.length = pendPatLen_;
  }
}

// Taken BEFORE the change is sent: the shadowed copy includes the in-flight
// write and every earlier pending edit, but not the one about to be made.
// snap_[undoIdx_] is never the in-flight write buffer, so this is safe while busy.
void Ui::snapshotUndo(int pat, const char* label, uint32_t gest) {
  copyPatternShadowed(pat, snap_[undoIdx_]);
  undoPat_ = uint8_t(pat);
  undoKind_ = 0;
  undoValid_ = true;
  undoIsRedo_ = false;
  undoGest_ = gest;
  undoEvt_ = evtGest_;
  undoQueued_ = false;   // a new edit after a queued CTRL+Z: the queued one would undo the wrong thing
  int i = 0;
  for (; label[i] && i < int(sizeof(undoLabel_)) - 1; ++i) undoLabel_[i] = label[i];
  undoLabel_[i] = 0;
}

uint32_t Ui::gestureNow() const {
  if (heldStep_ >= 0) return heldGest_;
  if (erasing_) return eraseGest_;
  return evtGest_;
}

// Does the current snapshot already cover a write to pat_ made now, in gesture gest?
bool Ui::undoCovers(uint32_t gest) const {
  if (!undoValid_ || undoIsRedo_ || undoKind_ != 0 || undoPat_ != pat_) return false;
  return undoEvt_ == evtGest_ || (gest && undoGest_ == gest);
}

// Before a step write: label "KICK STEP 5" (what = " STEP "), 1-based step.
void Ui::undoForStep(int t, int idx, const char* what) {
  const uint32_t g = gestureNow();
  if (undoCovers(g)) return;
  char buf[24];
  char* o = cat(cat(buf, kTrackNames[t & 7]), what);
  const int n = idx + 1;
  if (n >= 10) *o++ = char('0' + n / 10);
  *o++ = char('0' + n % 10);
  *o = 0;
  snapshotUndo(pat_, buf, g);
}

// Before a NoteOn the engine will record (REC on and playing): the engine
// writes the step itself, so the UI snapshots when it sends the note. The
// snapshot is of the playing pattern; a note quantised onto step 1 of the NEXT
// pattern (pressed just before a pattern change) is not in it.
void Ui::undoForRecording() {
  if (!recGest_) recGest_ = ++gestSeq_;
  const uint32_t g = heldStep_ >= 0 ? heldGest_ : recGest_;
  if (undoCovers(g)) return;
  snapshotUndo(pat_, "RECORDED NOTES", g);
}

bool Ui::postWrite(int pat, int track, int buf) {
  Command c = mk(track < 0 ? CmdType::WritePattern : CmdType::WriteTrack, pat, track < 0 ? 0 : track);
  c.ptr = track < 0 ? static_cast<const void*>(&snap_[buf]) : static_cast<const void*>(&snap_[buf].tracks[track]);
  // Pending edits of this pattern are part of the buffer now.
  for (PendStep& p : pendSteps_)
    if (p.used && p.pat == pat && (track < 0 || p.track == track)) p.used = 0;
  if (pendLenPat_ == pat) {
    if (track < 0) {
      std::memset(pendTrackLen_, 0, sizeof(pendTrackLen_));
      pendPatLen_ = 0;
    } else {
      pendTrackLen_[track] = 0;
    }
  }
  writeBusy_ = true;
  writeBuf_ = uint8_t(buf);
  writePat_ = uint8_t(pat);
  writeTrack_ = int8_t(track);
  writeExpect_ = uint8_t(st_.writesDone + 1);
  writeSinceMs_ = now_;
  send(c);
  ++serial_;
  return true;
}

// Before an instrument change on track t (stepInstrument).
void Ui::snapshotInstrument(int t) {
  undoKind_ = 1;
  undoTrack_ = uint8_t(t);
  undoInst_ = instrumentIdOf(t);
  for (int m = 0; m < kTrackMacros; ++m) undoPos_[m] = uint8_t(macroValue(t, m));
  for (int i = 0; i < kSoundParams; ++i) undoParams_[i] = uint8_t(paramValue(t, i));
  undoValid_ = true;
  undoIsRedo_ = false;
  undoGest_ = undoEvt_ = 0;
  undoQueued_ = false;
  char* o = cat(undoLabel_, kTrackNames[t & 7]);
  cat(o, " INSTRUMENT");
}

void Ui::undo() {
  if (!undoValid_) { toast("NOTHING TO UNDO", 0); return; }
  if (undoKind_ == 1) {
    // swap the track's instrument state with the snapshot
    const int t = undoTrack_;
    const uint16_t id = instrumentIdOf(t);
    uint8_t pos[kTrackMacros], params[kSoundParams];
    for (int m = 0; m < kTrackMacros; ++m) pos[m] = uint8_t(macroValue(t, m));
    for (int i = 0; i < kSoundParams; ++i) params[i] = uint8_t(paramValue(t, i));
    setInstrument(t, undoInst_, undoPos_);
    // CLASSIC (or an id the library lacks) plays its stored values: put them back
    if (!macro::findInstrument(undoInst_))
      for (int i = 0; i < kSoundParams; ++i)
        if (eng_->trackParamInfo(t, i) && paramValue(t, i) != undoParams_[i])
          setValue(ValueRef{false, uint8_t(t), uint8_t(i)}, undoParams_[i]);
    undoInst_ = id;
    std::memcpy(undoPos_, pos, sizeof(pos));
    std::memcpy(undoParams_, params, sizeof(params));
    undoIsRedo_ = !undoIsRedo_;
    listUntil_ = 0;
    const macro::MacroSet* s = instrumentOf(t);
    char buf[40];
    char* o = cat(cat(cat(buf, undoIsRedo_ ? "UNDO: " : "REDO: "), kTrackNames[t & 7]), " ");
    cat(o, s ? s->name : (instrumentIdOf(t) ? "UNKNOWN" : "CLASSIC"));
    toast(buf, 1);
    return;
  }
  if (writeBusy_) {
    // The previous undo/paste is still being copied in (normally under one
    // frame): queue this one; update() runs it when the write lands. Pressing
    // again while queued cancels it (undo + redo = nothing).
    undoQueued_ = !undoQueued_;
    return;
  }
  undoQueued_ = false;
  undoGest_ = undoEvt_ = 0;   // the next edit starts a new unit (and drops the redo)
  const int u = undoIdx_, spare = 1 - u;
  copyPatternShadowed(undoPat_, snap_[spare]);   // what the pattern is now = the redo state
  postWrite(undoPat_, -1, u);
  undoIdx_ = uint8_t(spare);
  undoIsRedo_ = !undoIsRedo_;
  char buf[40];
  cat(cat(buf, undoIsRedo_ ? "UNDO: " : "REDO: "), undoLabel_);
  toast(buf, 1);
}

void Ui::cutCopyTrack(bool cut) {
  TrackPattern& c = clip_.tracks[0];
  c = proj_->patterns[pat_].tracks[sel_];
  c.length = uint8_t(trackLen(sel_));
  for (int i = 0; i < kMaxSteps; ++i) c.steps[i] = stepAt(sel_, i);
  clipKind_ = 1;
  clipTrack_ = sel_;
  clipInst_[0] = instrumentId(proj_->tracks[sel_]);
  char buf[40];
  if (cut) {
    char* o = cat(cat(buf, kTrackNames[sel_]), " CLEARED");
    snapshotUndo(pat_, buf);
    send(mk(CmdType::ClearTrack, pat_, sel_));
    cat(o, "  CTRL+Z UNDO");
    toast(buf, 1);
  } else {
    cat(cat(buf, kTrackNames[sel_]), " COPIED");
    toast(buf, 0);
  }
}

void Ui::cutCopyPattern(bool cut) {
  copyPatternShadowed(pat_, clip_);
  clipKind_ = 2;
  for (int t = 0; t < kNumTracks; ++t) clipInst_[t] = instrumentId(proj_->tracks[t]);
  char buf[40];
  char* o = catP2(cat(buf, "P"), pat_ + 1);
  if (cut) {
    o = cat(o, " CLEARED");
    snapshotUndo(pat_, buf);
    send(mk(CmdType::ClearPattern, pat_));
    // ClearPattern resets the lengths: show 16 until the engine has done it.
    setPatternLength(16);
    for (int t = 0; t < kNumTracks; ++t) pendTrackLen_[t] = 0;
    cat(o, "  CTRL+Z UNDO");
    toast(buf, 1);
  } else {
    cat(o, " COPIED");
    toast(buf, 0);
  }
}

void Ui::paste() {
  if (!clipKind_) { toast("CLIPBOARD IS EMPTY", 0); return; }
  if (writeBusy_) { toast("BUSY - TRY AGAIN", 0); return; }
  const int spare = 1 - undoIdx_;
  char buf[40];
  if (clipKind_ == 1) {
    if (isSynth(clipTrack_) != isSynth(sel_)) {
      char* o = cat(buf, "CAN'T PASTE ");
      o = cat(o, isSynth(clipTrack_) ? kTrackNames[clipTrack_] : "DRUMS");
      o = cat(o, " ON ");
      cat(o, kTrackNames[sel_]);
      toast(buf, 2);
      return;
    }
    char* o = cat(cat(buf, kTrackNames[sel_]), " PASTED");
    snapshotUndo(pat_, buf);
    TrackPattern& d = snap_[spare].tracks[sel_];
    d = clip_.tracks[0];
    const uint16_t now = instrumentId(proj_->tracks[sel_]);
    if (clipTrack_ != sel_ || clipInst_[0] != now) {
      // Another voice: its sound-param locks mean something else; only the lead has chords;
      // a note's own macro values mean something only on the instrument they were made on.
      const bool dropAll = clipInst_[0] != 0 || now != 0;
      for (Step& s : d.steps) {
        if (sel_ != kTrackLead) s.setChord(0);
        if (dropAll || (clipTrack_ != sel_ && s.lockParam() && s.lockParam() <= kSoundParams)) s.clearExt();
      }
    }
    postWrite(pat_, sel_, spare);
    cat(o, "  CTRL+Z UNDO");
    toast(buf, 1);
    const int pages = (trackLen(sel_) + 15) / 16;
    if (shownPage_ >= pages) shownPage_ = 0;
    return;
  }
  char* o = cat(catP2(cat(buf, "P"), pat_ + 1), " PASTED");
  snapshotUndo(pat_, buf);
  std::memcpy(&snap_[spare], &clip_, sizeof(Pattern));
  for (int t = 0; t < kNumTracks; ++t)   // a track whose instrument changed since the copy: its values go
    if (clipInst_[t] != instrumentId(proj_->tracks[t]))
      for (Step& s : snap_[spare].tracks[t].steps) s.clearExt();
  postWrite(pat_, -1, spare);
  cat(o, "  CTRL+Z UNDO");
  toast(buf, 1);
  const int pages = (trackLen(sel_) + 15) / 16;
  if (shownPage_ >= pages) shownPage_ = 0;
}

void Ui::eraseFinished() {
  if (!eraseDid_) return;
  char buf[40];
  cat(cat(cat(buf, kTrackNames[sel_]), " ERASED"), "  CTRL+Z UNDO");
  toast(buf, 1);
  eraseDid_ = false;
}

void Ui::ctrlCombo(int k, bool shift) {
  switch (k) {
    case kKNote0: undo(); return;                                   // CTRL+Z
    case kKNote0 + 1: if (shift) cutCopyPattern(true); else cutCopyTrack(true); return;    // CTRL+X
    case kKNote0 + 2: if (shift) cutCopyPattern(false); else cutCopyTrack(false); return;  // CTRL+C
    case kKNote0 + 3: paste(); return;                              // CTRL+V (the clip knows its type)
    case kKPad0 + 1: ctrlSave(); return;                            // CTRL+S
    case kKNote0 + 5:                                               // CTRL+N
      if (dirty_) openConfirm(kConfirmNew, -1);
      else doNew();
      return;
    default: return;
  }
}

}  // namespace ui
}  // namespace pg
