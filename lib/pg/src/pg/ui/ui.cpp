// Pocket Groove UI: input handling, value editing, engine plumbing, timers.
// Drawing lives in ui_draw.cpp. Section numbers refer to docs/UX.md.
#include "ui.h"

#include <cstring>
#include <initializer_list>
#include <new>
#include "keys.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {
constexpr uint32_t kHoldMs = 250;          // step / TAB hold threshold (11)
constexpr uint32_t kModOverlayMs = 200;    // modifier overlay delay
constexpr uint32_t kRepeatDelayMs = 300;   // -/= (5.2)
constexpr uint32_t kRepeatMs = 40;
constexpr uint32_t kRepeatSmallMs = 200;
constexpr uint32_t kSweepTapMs = 120;      // [ ] (5.3)
constexpr uint32_t kSweepTickMs = 40;
constexpr float kSweepPerTick = 48.0f * 0.040f;
constexpr uint32_t kDetentMs = 150;
constexpr uint32_t kPopupMs = 1200;
constexpr uint32_t kToastMs[3] = {1500, 2500, 3000};
constexpr uint32_t kFlashMs = 80;
constexpr uint8_t kPendFrames = 10;        // a pending value outlives the engine round trip by this much
constexpr uint32_t kListMs = 1500;         // the instrument list after a change
constexpr uint32_t kMoveShowMs = 350;      // a macro reads as moving this long after its last step
constexpr int kMacroTap = 2;               // macro units (0..255) per tap; a repeat moves 2 / 4 / 8
constexpr int kMacroCoarse = 16;           // with SHIFT
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool after(uint32_t now, uint32_t t) { return int32_t(now - t) >= 0; }

Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}

// Append helpers for toast text (no printf on the device path).
char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
char* catInt(char* o, int v) { return o + itoa10(v, o); }
char* catP2(char* o, int n) {   // "05"
  *o++ = char('0' + (n / 10) % 10);
  *o++ = char('0' + n % 10);
  *o = 0;
  return o;
}
}  // namespace

// ============================================================== lifecycle ====

void Ui::init(Engine& engine, Project& project, FileSystem* fs) {
  // Reset every member to its default (Ui is plain data). Placement-new, not
  // `*this = Ui()`, so no multi-KB temporary lands on the UI task's stack.
  this->~Ui();
  new (this) Ui();
  eng_ = &engine;
  proj_ = &project;
  fs_ = fs;
  sel_ = kTrackBass;                       // power-on state (UX 2, 14)
  focus_ = ValueRef{false, kTrackBass, 0};
  trackFocus_[kTrackPerc] = 1;             // PERC focuses TUNE, not MODL
  fxFocus_[kFxDuck] = 1;                   // DDEP
  fxFocus_[kFxSpace] = 1;                  // DFB
  fxFocus_[kFxMaster] = 3;                 // FILT
  octave_[kTrackBass] = 1;
  octave_[kTrackLead] = 3;
  chordMode_ = kModeAuto;
  rollRate_ = 1;                           // 1/16
  std::memset(&st_, 0, sizeof(st_));
  st_.queuedPattern = 0xFF;
  baseCrc_ = projectCrc();            // the attached project counts as clean
  sessCrc_ = baseCrc_;
}

// ============================================================ effective reads ====

Step Ui::stepAt(int t, int idx) const {
  t &= 7;
  idx = clampi(idx, 0, kMaxSteps - 1);
  for (const PendStep& p : pendSteps_)
    if (p.used && p.pat == pat_ && p.track == t && p.idx == idx) return p.s;
  if (const TrackPattern* w = inflight(pat_, t)) return w->steps[idx];
  return proj_->patterns[pat_].tracks[t].steps[idx];
}

const TrackPattern* Ui::inflight(int pat, int t) const {
  if (!writeBusy_ || writePat_ != pat || (writeTrack_ >= 0 && writeTrack_ != t)) return nullptr;
  return &snap_[writeBuf_].tracks[t];
}

int Ui::trackLen(int t) const {
  t &= 7;
  const TrackPattern* w = inflight(pat_, t);
  int v = (pendTrackLen_[t] && pendLenPat_ == pat_) ? pendTrackLen_[t]
                                                     : (w ? w->length : proj_->patterns[pat_].tracks[t].length);
  return clampi(v, 1, kMaxSteps);
}

int Ui::patternLen() const {
  int v = (pendPatLen_ && pendLenPat_ == pat_) ? pendPatLen_ : proj_->patterns[pat_].length;
  if (!(pendPatLen_ && pendLenPat_ == pat_) && writeBusy_ && writePat_ == pat_ && writeTrack_ < 0)
    v = snap_[writeBuf_].length;
  return clampi(v, 1, kMaxSteps);
}

int Ui::paramValue(int t, int i) const {
  t &= 7;
  if (isMacroIndex(i)) return macroValue(t, i - kMacroIndex0);
  i &= 31;
  if ((pendTPMask_[t] >> i) & 1) return pendTP_[t][i];
  return proj_->tracks[t].params[i];
}

int Ui::globalValue(int g) const {
  g &= 31;
  if ((pendGMask_ >> g) & 1) return pendG_[g];
  return proj_->globals[g];
}

bool Ui::muted(int t) const {
  t &= 7;
  if (pendMute_[t]) return pendMute_[t] == 2;
  return proj_->tracks[t].mute != 0;
}

// Is the focused track param p-locked on any step of its track in the playing
// pattern? Cached: rescanned only when its inputs change (64 steps at most).
void Ui::updateFocusLock() {
  uint32_t key = 2166136261u;
  for (uint32_t v : {uint32_t(focus_.global), uint32_t(focus_.track), uint32_t(focus_.index), uint32_t(pat_),
                     st_.revision, uint32_t(writeBusy_)}) key = (key ^ v) * 16777619u;
  for (const PendStep& p : pendSteps_) key = (key ^ (p.used ? p.s.extWord() << 8 | p.idx : 0u)) * 16777619u;
  if (key == lockKey_) return;
  lockKey_ = key;
  bool locked = false;
  if (!focus_.global) {
    const int t = focus_.track & 7, len = trackLen(t);
    for (int i = 0; i < len && !locked; ++i) {
      const Step s = stepAt(t, i);
      locked = s.on() && stepOwnValue(t, s, focus_.index) >= 0;
    }
  }
  if (locked != focusLocked_) { focusLocked_ = locked; ++serial_; }
}

int Ui::stepOwnValue(int t, const Step& s, int index) const {
  if (instrumentIdOf(t)) {
    const int m = index - kMacroIndex0;
    return (isMacroIndex(index) && s.hasMacro(m)) ? s.macroValue(m) : -1;
  }
  return (s.lockParam() && s.lockParam() == index + 1) ? s.lockValue() : -1;
}

// DECISION (del on a held step): a CLASSIC step loses its lock; a note with own
// macro values loses the focused macro's value if it has one, else all of them.
bool Ui::clearStepOwn(int t, Step& s) const {
  if (!s.hasExt()) return false;
  const int f = focusIndexFor(t);
  if (instrumentIdOf(t) && isMacroIndex(f) && s.hasMacro(f - kMacroIndex0)) s.clearMacro(f - kMacroIndex0);
  else s.clearExt();
  return true;
}

uint8_t Ui::motionMask(int t) const {
  if (!instrumentIdOf(t)) return 0;
  const int len = trackLen(t) < patternLen() ? trackLen(t) : patternLen();
  uint32_t w = 0;
  for (int i = 0; i < len; ++i) {
    const Step s = stepAt(t, i);
    if (s.on()) w |= s.extWord();
  }
  uint8_t mask = 0;
  for (int m = 0; m < kTrackMacros; ++m)
    if ((w >> (6 * m)) & 63u) mask = uint8_t(mask | (1u << m));
  return mask;
}

int Ui::shownMacro(int t, int m) const {
  t &= 7;
  m &= 3;
  if (playing_ && ((st_.motion[t] >> m) & 1u) && !((st_.live[t] >> m) & 1u)) return st_.macroPos[t][m];
  return macroValue(t, m);
}

bool Ui::patternUsed(int pat) const {
  const Pattern& P = proj_->patterns[pat & 15];
  for (int t = 0; t < kNumTracks; ++t) {
    const int len = P.tracks[t].length > kMaxSteps ? kMaxSteps : P.tracks[t].length;
    for (int s = 0; s < len; ++s)
      if (P.tracks[t].steps[s].flags() & kStepOn) return true;
  }
  return false;
}

bool Ui::stepEditOpen() const {
  return heldStep_ >= 0 && (heldOther_ || after(now_, heldDownMs_ + kHoldMs));
}

int Ui::playStep(int t) const {
  if (!playing_) return -1;
  return st_.step[t & 7];
}

const ParamInfo* Ui::infoFor(const ValueRef& r) const {
  return r.global ? Engine::globalParamInfo(r.index) : trackInfo(r.track, r.index);
}

// ============================================================ instruments ====

uint16_t Ui::instrumentIdOf(int t) const {
  t &= 7;
  if ((pendInstMask_ >> t) & 1) return pendInst_[t];
  return instrumentId(proj_->tracks[t]);
}

const macro::MacroSet* Ui::instrumentOf(int t) const {
  const macro::MacroSet* s = macro::findInstrument(instrumentIdOf(t));
  return s && s->track == (t & 7) ? s : nullptr;
}

int Ui::macroValue(int t, int m) const {
  t &= 7;
  m &= 3;
  if ((pendMacMask_[t] >> m) & 1) return pendMac_[t][m];
  return proj_->tracks[t].macro[m];
}

bool Ui::instrumentPage() const {
  if (!devMode_) return true;
  return (instrumentOf(sel_) != nullptr) != devFlip_;   // dev mode: CLASSIC shows the parameters, `l` flips
}

const ParamInfo* Ui::macroInfo(int t, int m) const {
  const macro::MacroSet* s = instrumentOf(t);
  if (!s || m < 0 || m >= s->numMacros) return nullptr;
  ParamInfo& in = macInfo_[macInfoNext_++ & 3];
  in.name = s->macros[m].name;
  in.longName = s->macros[m].name;
  in.min = 0;
  in.max = uint8_t(macro::kPosMax);
  in.def = macro::startByte(s->macros[m]);
  in.fmt = ParamFormat::Percent;
  in.enumNames = nullptr;
  return &in;
}

const ParamInfo* Ui::trackInfo(int t, int i) const {
  if (isMacroIndex(i)) return macroInfo(t, i - kMacroIndex0);
  return eng_->trackParamInfo(t, i);
}

// What -/= edit for track t when it gets the focus: its remembered focus if that
// still exists; on a track with an instrument a macro (the bare sound parameters
// are not the player's, except in developer mode); on CLASSIC a parameter.
int Ui::focusIndexFor(int t) const {
  const int f = trackFocus_[t & 7];
  const macro::MacroSet* s = instrumentOf(t);
  if (isMacroIndex(f)) {
    if (s && f - kMacroIndex0 < s->numMacros) return f;
    if (s) return kMacroIndex0;
    return t == kTrackPerc ? 1 : 0;
  }
  if (s && f < kSoundParams && !devMode_) return kMacroIndex0;
  return f;
}

// After the instrument of track t changed: a focus on its sound follows (a mixer row stays).
void Ui::refocus(int t) {
  if (!focus_.global && focus_.track == t && !(focus_.index >= kMixLevel && focus_.index < kMacroIndex0))
    focus_ = ValueRef{false, uint8_t(t), uint8_t(focusIndexFor(t))};
}

void Ui::setMacro(int t, int m, int v) {
  if (t < 0 || t >= kNumTracks || m < 0 || m >= kTrackMacros) return;
  v = clampi(v, 0, macro::kPosMax);
  if (v == macroValue(t, m)) return;
  pendMac_[t][m] = uint8_t(v);
  pendMacFrame_[t][m] = frame_;
  pendMacMask_[t] |= uint8_t(1u << m);
  // Record on and playing: the move is recorded into the notes it passes (the engine
  // writes them; one undo unit with the pass's notes).
  if (recOn_ && playing_ && instrumentOf(t)) undoForRecording("RECORDED MOTION");
  send(cmd(CmdType::SetMacro, t, m, v));
  ++serial_;
}

void Ui::setInstrument(int t, uint16_t id, const uint8_t* positions) {
  if (t < 0 || t >= kNumTracks) return;
  // CLASSIC <-> instrument changes what a step's last three bytes mean (project.h),
  // so a pattern snapshot taken before the change would restore them in the wrong
  // format: it is dropped. (The UI's own instrument changes already hold an
  // instrument snapshot here; this is for callers such as the macro lab.)
  if (undoValid_ && undoKind_ == 0 && (id != 0) != (instrumentIdOf(t) != 0)) {
    undoValid_ = false;
    undoQueued_ = false;
  }
  uint8_t pos[kTrackMacros] = {0, 0, 0, 0};
  const macro::MacroSet* s = macro::findInstrument(id);
  for (int m = 0; m < kTrackMacros; ++m)
    pos[m] = positions ? positions[m] : (s && m < s->numMacros ? macro::startByte(s->macros[m]) : 0);
  pendInst_[t] = id;
  pendInstFrame_[t] = frame_;
  pendInstMask_ |= uint8_t(1u << t);
  for (int m = 0; m < kTrackMacros; ++m) {
    pendMac_[t][m] = pos[m];
    pendMacFrame_[t][m] = frame_;
  }
  pendMacMask_[t] = 0x0F;
  send(setInstrumentCommand(t, id, pos));
  refocus(t);
  ++serial_;
}

// The instrument before / after this one in the track's list (UX.md 10.3). CLASSIC
// (and an id the library does not know) is not in the list: from there `.` goes to
// the first instrument and `;` to the last. A run of steps while the list is up is
// one undo unit, so CTRL+Z goes back to what the track had before the run.
void Ui::stepInstrument(int dir) {
  const int t = sel_;
  const int n = macro::instrumentCount(t);
  if (n <= 0) return;
  const uint16_t cur = instrumentIdOf(t);
  const int idx = macro::instrumentIndex(t, cur);
  const int next = idx < 0 ? (dir > 0 ? 0 : n - 1) : ((idx + dir) % n + n) % n;
  const macro::MacroSet* s = macro::instrumentAt(t, next);
  if (!s) return;
  if (!(undoValid_ && undoKind_ == 1 && undoTrack_ == t && !undoIsRedo_ && listUntil_)) snapshotInstrument(t);
  // remember the positions of the one we leave, find those of the one we go to
  if (idx >= 0) {
    Memo* slot = nullptr;
    for (Memo& mm : memo_)
      if (mm.used && mm.id == cur && mm.track == t) slot = &mm;
    if (!slot) slot = &memo_[memoNext_++ % kMemos];
    slot->used = 1;
    slot->id = cur;
    slot->track = uint8_t(t);
    for (int m = 0; m < kTrackMacros; ++m) slot->pos[m] = uint8_t(macroValue(t, m));
  }
  const uint8_t* pos = nullptr;
  for (const Memo& mm : memo_)
    if (mm.used && mm.id == s->id && mm.track == t) pos = mm.pos;
  setInstrument(t, s->id, pos);
  listUntil_ = (now_ + kListMs) | 1u;
  toastUntil_ = popUntil_ = 0;
}

// ---- macro keys (UX.md 10.3): z/x c/v b/n m/, = macro 1..4 down/up -----------------

void Ui::macroKeyDown(int k, int n, bool shift) {
  const macro::MacroSet* s = instrumentOf(sel_);
  if (!s) {
    toast(heldStep_ >= 0 ? "NO INSTRUMENT ON THIS TRACK" : ". OR 9: PICK AN INSTRUMENT", 0);
    return;
  }
  const int m = n / 2, dir = (n & 1) ? 1 : -1;
  if (m >= s->numMacros) return;
  if (heldStep_ < 0 && isHeld(kKDel)) {   // DEL held + a macro key: clear that macro's motion
    rec_[k] = KeyRec{kActNone, 0, 0, 0};
    clearMotion(m);
    return;
  }
  trackFocus_[sel_] = uint8_t(kMacroIndex0 + m);
  focus_ = ValueRef{false, sel_, uint8_t(kMacroIndex0 + m)};
  rec_[k] = KeyRec{kActMacro, uint8_t(m), 0, 0};
  const int partner = kKNote0 + (n ^ 1);
  if (isHeld(partner) && rec_[partner].act == kActMacro) {
    // both keys of a pair: the set's default (a lock: the default on that step)
    const int def = macro::startByte(s->macros[m]);
    if (heldStep_ >= 0) {
      Step st = stepAt(heldTrack_, heldStep_);
      st.setMacroValue(m, def);
      heldEdited(st);
      setStep(heldTrack_, heldStep_, st);
    } else {
      setMacro(sel_, m, def);
    }
    macMoveMs_[m] = now_ | 1u;
    char buf[24];
    cat(cat(buf, s->macros[m].name), " DEFAULT");
    toast(buf, 0);
    macRep_[m].key = -1;
    rec_[partner].act = kActNone;
    rec_[k].act = kActNone;
    return;
  }
  macroEvent(m, dir, 1, shift);
  macRep_[m] = MacRep{int8_t(k), int8_t(dir), now_, now_ + kRepeatDelayMs};
}

// One move of macro m: on the held step's lock while a step is held (SOUND), else the macro.
void Ui::macroEvent(int m, int dir, int mag, bool shift) {
  const int d = dir * (shift ? kMacroCoarse : kMacroTap * mag);
  macMoveMs_[m & 3] = now_ | 1u;
  if (heldStep_ >= 0 && view_ != View::Groove) {
    focus_ = ValueRef{false, heldTrack_, uint8_t(kMacroIndex0 + m)};
    editLock(dir, mag, shift);
    return;
  }
  setMacro(sel_, m, macroValue(sel_, m) + d);
  showPopup(ValueRef{false, sel_, uint8_t(kMacroIndex0 + m)});
}

// Moving now: its key repeats, or it moved within the last moment (a tap reads as a move too).
bool Ui::macroMoving(int m) const {
  m &= 3;
  if (macRep_[m].key >= 0) return true;
  return macMoveMs_[m] != 0 && int32_t(now_ - macMoveMs_[m]) < int32_t(kMoveShowMs);
}

// Each macro key repeats on its own, so two macros sweep at once (two fingers).
void Ui::runMacroRepeats() {
  const bool shift = isHeld(kKShift);
  for (int m = 0; m < kTrackMacros; ++m) {
    MacRep& r = macRep_[m];
    if (r.key < 0) continue;
    if (!isHeld(r.key) || rec_[r.key].act != kActMacro || view_ != View::Sound) { r.key = -1; continue; }
    for (int guard = 0; guard < 8 && after(now_, r.nextMs); ++guard) {
      const uint32_t rep = r.nextMs - (r.startMs + kRepeatDelayMs);
      const int mag = shift ? 1 : (rep < 500 ? 1 : (rep < 1000 ? 2 : 4));
      macroEvent(m, r.dir, mag, shift);
      r.nextMs += kRepeatMs;
    }
    if (!after(r.nextMs, now_ - 1000)) r.nextMs = now_;
  }
}

int Ui::valueOf(const ValueRef& r) const {
  return r.global ? globalValue(r.index) : paramValue(r.track, r.index);
}

// ============================================================ engine plumbing ====

void Ui::send(const Command& c) {
  if (outCount_ == 0 && eng_->post(c)) return;
  // Coalesce value repeats: only the latest value per parameter waits (UX 16.7).
  if (c.type == CmdType::SetTrackParam || c.type == CmdType::SetGlobal || c.type == CmdType::SetMacro) {
    for (int i = 0; i < outCount_; ++i) {
      Command& o = outbox_[(outHead_ + i) % kOutbox];
      if (o.type == c.type && o.a == c.a && (c.type == CmdType::SetGlobal || o.b == c.b)) {
        o.c = c.c;
        return;
      }
    }
  }
  if (outCount_ >= kOutbox) { ++dropped_; return; }
  outbox_[(outHead_ + outCount_) % kOutbox] = c;
  ++outCount_;
}

void Ui::flushOutbox() {
  while (outCount_) {
    if (!eng_->post(outbox_[outHead_])) return;
    outHead_ = uint8_t((outHead_ + 1) % kOutbox);
    --outCount_;
  }
}

void Ui::prunePending() {
  const bool idle = outCount_ == 0;
  auto stale = [&](uint8_t f) { return idle && uint8_t(frame_ - f) > kPendFrames; };
  for (int t = 0; t < kNumTracks; ++t) {
    uint32_t m = pendTPMask_[t];
    while (m) {
      const int i = __builtin_ctz(m);
      m &= m - 1;
      if (proj_->tracks[t].params[i] == pendTP_[t][i] || stale(pendTPFrame_[t][i])) pendTPMask_[t] &= ~(1u << i);
    }
    if (pendTrackLen_[t] && (proj_->patterns[pendLenPat_].tracks[t].length == pendTrackLen_[t] ||
                             stale(pendTrackLenFrame_[t])))
      pendTrackLen_[t] = 0;
    if (pendMute_[t] && ((proj_->tracks[t].mute != 0) == (pendMute_[t] == 2) || stale(pendMuteFrame_[t])))
      pendMute_[t] = 0;
    if (((pendInstMask_ >> t) & 1) &&
        (instrumentId(proj_->tracks[t]) == pendInst_[t] || stale(pendInstFrame_[t])))
      pendInstMask_ &= uint8_t(~(1u << t));
    for (int m = 0; m < kTrackMacros; ++m)
      if (((pendMacMask_[t] >> m) & 1) && !((pendInstMask_ >> t) & 1) &&
          (proj_->tracks[t].macro[m] == pendMac_[t][m] || stale(pendMacFrame_[t][m])))
        pendMacMask_[t] &= uint8_t(~(1u << m));
  }
  uint32_t m = pendGMask_;
  while (m) {
    const int i = __builtin_ctz(m);
    m &= m - 1;
    if (proj_->globals[i] == pendG_[i] || stale(pendGFrame_[i])) pendGMask_ &= ~(1u << i);
  }
  if (pendPatLen_ && (proj_->patterns[pendLenPat_].length == pendPatLen_ || stale(pendPatLenFrame_))) pendPatLen_ = 0;
  for (int i = 0; i < kSongSlots; ++i) {
    PendSlot& p = pendSong_[i];
    if (p.used && ((proj_->song[i].pattern == p.pattern && proj_->song[i].repeats == p.repeats) || stale(p.frame)))
      p.used = 0;
  }
  for (PendStep& p : pendSteps_) {
    if (!p.used) continue;
    const Step& cur = proj_->patterns[p.pat].tracks[p.track].steps[p.idx];
    if (std::memcmp(&cur, &p.s, sizeof(Step)) == 0 || stale(p.frame)) p.used = 0;
  }
}

void Ui::setStep(int t, int idx, const Step& s) {
  if (idx < 0 || idx >= kMaxSteps) return;
  // Every step write is undoable; writes of one gesture share a snapshot (ui_edit.cpp).
  const Step cur = stepAt(t, idx);
  if (std::memcmp(&cur, &s, sizeof(Step)) != 0) undoForStep(t, idx, " STEP ");
  // Shadow it so the next read (e.g. the key-up of a tap) sees it before the engine does.
  PendStep* slot = nullptr;
  for (PendStep& p : pendSteps_)
    if (p.used && p.pat == pat_ && p.track == t && p.idx == idx) { slot = &p; break; }
  if (!slot)
    for (PendStep& p : pendSteps_)
      if (!p.used) { slot = &p; break; }
  if (!slot) {   // full: evict the oldest
    slot = &pendSteps_[0];
    for (PendStep& p : pendSteps_)
      if (uint8_t(frame_ - p.frame) > uint8_t(frame_ - slot->frame)) slot = &p;
  }
  slot->used = 1;
  slot->pat = pat_;
  slot->track = uint8_t(t);
  slot->idx = uint8_t(idx);
  slot->frame = frame_;
  slot->s = s;
  Command c = cmd(CmdType::SetStep, pat_, t, idx);
  c.step = s;
  send(c);
  ++serial_;
}

void Ui::setTrackLength(int t, int len) {
  len = clampi(len, 1, kMaxSteps);
  if (pendLenPat_ != pat_) { std::memset(pendTrackLen_, 0, sizeof(pendTrackLen_)); pendPatLen_ = 0; }
  pendLenPat_ = pat_;
  pendTrackLen_[t] = uint8_t(len);
  pendTrackLenFrame_[t] = frame_;
  send(cmd(CmdType::SetTrackLength, pat_, t, len));
  ++serial_;
}

void Ui::setPatternLength(int len) {
  len = clampi(len, 1, kMaxSteps);
  if (pendLenPat_ != pat_) { std::memset(pendTrackLen_, 0, sizeof(pendTrackLen_)); }
  pendLenPat_ = pat_;
  pendPatLen_ = uint8_t(len);
  pendPatLenFrame_ = frame_;
  send(cmd(CmdType::SetPatternLength, pat_, 0, len));
  ++serial_;
}

void Ui::setMute(int t, bool m) {
  pendMute_[t] = m ? 2 : 1;
  pendMuteFrame_[t] = frame_;
  send(cmd(CmdType::SetMute, t, 0, m ? 1 : 0));
  ++serial_;
}

void Ui::setValue(const ValueRef& r, int v) {
  if (r.global) {
    pendG_[r.index & 31] = uint8_t(v);
    pendGFrame_[r.index & 31] = frame_;
    pendGMask_ |= 1u << (r.index & 31);
    send(cmd(CmdType::SetGlobal, r.index, 0, v));
  } else if (isMacroIndex(r.index)) {
    setMacro(r.track & 7, r.index - kMacroIndex0, v);
    return;
  } else {
    pendTP_[r.track & 7][r.index & 31] = uint8_t(v);
    pendTPFrame_[r.track & 7][r.index & 31] = frame_;
    pendTPMask_[r.track & 7] |= 1u << (r.index & 31);
    send(cmd(CmdType::SetTrackParam, r.track, r.index, v));
  }
  ++serial_;
}

void Ui::setTrackParam(int track, int index, int value) {
  if (track < 0 || track >= kNumTracks || index < 0 || index >= kParamsPerTrack) return;
  const ParamInfo* info = eng_->trackParamInfo(track, index);
  if (!info) return;
  const int v = clampi(value, info->min, info->max);
  if (v != paramValue(track, index)) setValue(ValueRef{false, uint8_t(track), uint8_t(index)}, v);
}

void Ui::noteOn(int t, int note, int vel, uint8_t flags, uint8_t chord) {
  if (recOn_ && playing_) undoForRecording();   // the engine records this note into the pattern
  Command c = cmd(CmdType::NoteOn, t, clampi(note, 0, 127), clampi(vel, 1, 127));
  c.step = kDefaultStep;
  c.step.note = uint8_t(c.b);
  c.step.vel = uint8_t(c.c);
  c.step.setFlags(flags);
  c.step.setChord(chord);
  send(c);
}

void Ui::noteOff(int t, int note) { send(cmd(CmdType::NoteOff, t, clampi(note, 0, 127))); }

// ============================================================== helpers ====

int Ui::noteFor(int t, int degree) const {
  return degreeNote(globalValue(kGScaleRoot) % 12, globalValue(kGScaleType), octave_[t & 7], degree);
}

uint8_t Ui::chordForDegree(int degree) const { return chordFor(chordMode_, globalValue(kGScaleType), degree); }

Step Ui::newStep(int t) const {
  Step s = kDefaultStep;
  s.setFlags(kStepOn);
  if (isSynth(t)) {
    s.note = uint8_t(noteFor(t, 0));
    if (t == kTrackLead) s.setChord(chordForDegree(0));
  }
  return s;
}

bool Ui::fieldAvailable(int t, int f) const {
  if (f == kFSlide) return isSynth(t);
  if (f == kFChord) return t == kTrackLead;
  return f >= 0 && f < kNumFields;
}

void Ui::toast(const char* s, uint8_t kind) {
  int i = 0;
  for (; s[i] && i < int(sizeof(toast_)) - 1; ++i) toast_[i] = s[i];
  toast_[i] = 0;
  toastKind_ = kind > 2 ? 2 : kind;
  toastUntil_ = (now_ + kToastMs[toastKind_]) | 1u;
  popUntil_ = 0;
  ++serial_;
}

bool Ui::valueOnScreen(const ValueRef& r) const {
  switch (view_) {
    case View::Sound:
      if (r.global || r.track != sel_) return false;
      if (instrumentPage()) return isMacroIndex(r.index);
      return r.index < kSoundParams && r.index / 8 == soundPage_[sel_];
    case View::Mix:
      return !r.global && r.index == kMixRowParam[mixRow_];
    case View::Fx:
      if (!r.global) return fxTab_ == kFxDuck && r.index == kMixDuck;
      for (int c = 0; c < 8; ++c)
        if (kFxPage[fxTab_][c] == r.index) return true;
      return false;
    default:
      return false;
  }
}

void Ui::showPopup(const ValueRef& r) {
  if (isHeld(kKTab) && r.global && (r.index == kGTempo || r.index == kGSwing)) return;
  if (valueOnScreen(r)) return;
  popRef_ = r;
  popUntil_ = (now_ + kPopupMs) | 1u;
  toastUntil_ = 0;
  ++serial_;
}

// ============================================================== key input ====

void Ui::onKey(const KeyEvent& e, uint32_t nowMs) {
  if (e.key >= kNumKeyIds || !proj_) return;
  // Phantom keys of the diode-less matrix are dropped here (16.1, ghost.h).
  GhostFilter::Out out[GhostFilter::kMaxOut];
  const int n = ghost_.push(e, nowMs, out);
  for (int i = 0; i < n; ++i) keyEvent(out[i].ev, out[i].ms);
}

void Ui::keyEvent(const KeyEvent& e, uint32_t nowMs) {
  const int k = e.key;
  now_ = nowMs;
  evtGest_ = ++gestSeq_;   // a new key event (undo units, ui_edit.cpp)
  const uint64_t bit = 1ull << k;
  ++serial_;
  if (e.down) {
    if (held_ & bit) return;                       // duplicate down
    held_ |= bit;
    downMs_[k] = nowMs;
    rec_[k] = KeyRec{kActNone, 0, 0, 0};
    lastKeyMs_ = nowMs;
    if (banner_) banner_ = false;                  // the first key dismisses the banner AND acts (14.2)
    // Explain mode (10.10): keys describe themselves; only SPACE, ` and / act.
    if (explain_) {
      if (k == kKSlash || k == kKBacktick) {
        explain_ = false;
        explainKey_ = -1;
        if (k == kKBacktick) home();
      } else if (k == kKSpace) {
        transport(isHeld(kKShift));
      } else {
        explainKey_ = int8_t(k);
        explainTable_ = uint8_t(helpTable());   // FN+pad explains the mute, not the pad
      }
      return;
    }
    if (k == kKG0) return;                         // the firmware owns G0 (volume, diagnostics)
    // Modal dialogs swallow every key (16.15); confirm still lets SPACE play/stop.
    if (dialog_ != Dialog::None) {
      if (isModifierKey(k)) return;
      if (dialog_ == Dialog::Name) nameKey(k);
      else confirmKey(k);
      return;
    }
    if (heldStep_ >= 0 && k != heldKey_ && !isModifierKey(k)) heldOther_ = true;
    if (isModifierKey(k)) {
      if (k == kKFn) comboMods_ &= ~1u;
      if (k == kKOpt) comboMods_ &= ~2u;
      if (k == kKAlt) comboMods_ &= ~4u;
      return;
    }
    if (k == kKTab) {
      comboMods_ &= ~8u;
      tabDownUs_ = e.timeUs;
      rec_[k].act = kActTab;
      return;
    }
    // Any key pressed while a modifier is held is a combo: the overlay shows at once.
    if (isHeld(kKFn)) comboMods_ |= 1;
    if (isHeld(kKOpt)) comboMods_ |= 2;
    if (isHeld(kKAlt)) comboMods_ |= 4;
    if (isHeld(kKTab)) comboMods_ |= 8;
    keyDown(k, isHeld(kKShift));
  } else {
    if (!(held_ & bit)) return;
    held_ &= ~bit;
    keyUp(k);
  }
}

void Ui::keyDown(int k, bool shift) {
  const bool fn = isHeld(kKFn), opt = isHeld(kKOpt), alt = isHeld(kKAlt), ctrl = isHeld(kKCtrl);
  const bool tab = isHeld(kKTab);
  // ---- keys that never change meaning
  switch (k) {
    case kKBacktick: home(); return;
    case kKSpace: transport(shift); return;
    case kK9: viewKey(View::Sound); return;
    case kK0: viewKey(View::Mix); return;
    case kKO: viewKey(View::Fx); return;
    case kKP: viewKey(View::Song); return;
    case kKBackslash: viewKey(View::Project); return;
    case kKSlash:                                  // hold = key map, tap = explain mode
      rec_[k].act = kActHelp;
      helpDownMs_ = now_;
      return;
    case kKLBracket: case kKRBracket: sweepDown(k, shift); return;
    case kKMinus: case kKEquals: valueKeyDown(k, shift); return;
    default: break;
  }
  const int step = stepKeyIndex(k), pad = padKeyIndex(k), note = noteKeyIndex(k);
  // ---- modifier layers (6.2)
  if (fn) { fnCombo(k); return; }
  if (opt) { optCombo(k, shift); return; }
  if (alt) { altCombo(k); return; }
  if (ctrl) {
    if (shift && step >= 0 && view_ != View::Song && view_ != View::Project) stepDown(k, step, true, true);
    else if (heldStep_ >= 0 && view_ == View::Groove && (k == kKSemicolon || k == kKDot))
      semicolonDot(k == kKSemicolon ? 1 : -1, true);   // CTRL+;/. = note +-12 (was SHIFT: ghosting, 16.1)
    else ctrlCombo(k, shift);
    return;
  }
  if (tab && (k == kKSemicolon || k == kKDot)) {
    const ValueRef r{true, 0, kGSwing};
    editFocus(r, k == kKSemicolon ? 1 : -1, true);
    return;
  }
  if (view_ == View::Song) {
    if (step >= 0) { songStepKey(step, shift); return; }
    switch (k) {
      case kKDel: songDel(); return;
      case kKSemicolon: songMove(-1); return;
      case kKDot: songMove(1); return;
      case kKEnter: songEnter(); return;
      case kKQuote: case kKL: return;
      default: break;
    }
  } else if (view_ == View::Project) {
    if (step >= 0) return;
    switch (k) {
      case kKDel: projectDel(); return;
      case kKSemicolon: projectMove(-1); return;
      case kKDot: projectMove(1); return;
      case kKEnter: projectEnter(); return;
      case kKQuote: case kKL: return;
      default: break;
    }
  }
  if (step >= 0) { stepDown(k, step, shift, false); return; }
  if (pad >= 0) { padDown(k, pad, shift); return; }
  if (note >= 0) { noteDown(k, note, shift); return; }
  switch (k) {
    case kKSemicolon: semicolonDot(1, false); return;
    case kKDot: semicolonDot(-1, false); return;
    case kKQuote: pageKey(shift); return;
    case kKL: chordKey(shift); return;
    case kKEnter: recToggle(); return;
    case kKDel: delDown(k); return;
    default: return;
  }
}

void Ui::keyUp(int k) {
  KeyRec& r = rec_[k];
  switch (r.act) {
    case kActStep: stepUp(k); break;
    case kActPad:
    case kActNote: noteOff(r.a, r.b); break;
    case kActRoll:
      rollMask_ &= uint8_t(~(1u << r.a));
      sendRoll(r.a, false);
      break;
    case kActValue:
      if (repKey_ == k) {
        runRepeat();     // repeats due between the last frame and the release still count
        repKey_ = -1;
      }
      break;
    case kActSweep:
      if (swKey_ == k) swKey_ = -1;
      break;
    case kActMacro:
      if (macRep_[r.a & 3].key == k) {
        runMacroRepeats();   // repeats due since the last frame still count
        macRep_[r.a & 3].key = -1;
      }
      break;
    case kActErase:
      erasing_ = false;
      eraseFinished();
      break;
    case kActTab: tabUp(k); break;
    case kActHelp:
      if (!after(now_, helpDownMs_ + 400)) {      // tap: explain mode
        explain_ = true;
        explainKey_ = -1;
      }
      break;
    default: break;
  }
  r.act = kActNone;
}

// ---- global keys -------------------------------------------------------------

void Ui::home() {
  view_ = View::Groove;
  if (recOn_) {
    recOn_ = false;
    send(cmd(CmdType::SetRecord, 0, 0, 0));
  }
  if (heldStep_ >= 0) {                  // the step is released without toggling
    rec_[heldKey_].act = kActNone;
    heldStep_ = -1;
  }
  popUntil_ = toastUntil_ = 0;
}

void Ui::transport(bool shift) {
  if (playing_) {
    send(shift ? cmd(CmdType::Play, 0) : cmd(CmdType::Stop));
    playing_ = shift;
  } else {
    send(cmd(CmdType::Play, shift ? 1 : 0));
    playing_ = true;
  }
  erasing_ = false;
  recGest_ = 0;   // a recording pass ends with the transport (and a restart starts a new one)
}

void Ui::viewKey(View v) {
  if (view_ != v) {
    view_ = v;
    if (v == View::Project) refreshFiles();
    return;
  }
  switch (v) {   // the view key again = next page / row / tab
    case View::Sound: {
      if (instrumentPage()) { stepInstrument(1); break; }   // 9 again: the next instrument
      int n = 0;
      while (n < kSoundParams && eng_->trackParamInfo(sel_, n)) ++n;
      const int pages = n ? (n + 7) / 8 : 1;
      soundPage_[sel_] = uint8_t((soundPage_[sel_] + 1) % pages);
      break;
    }
    case View::Mix:
      mixRow_ = uint8_t((mixRow_ + 1) % kNumMixRows);
      focus_ = ValueRef{false, sel_, kMixRowParam[mixRow_]};
      break;
    case View::Fx: fxTab_ = uint8_t((fxTab_ + 1) % kNumFxTabs); break;
    case View::Project:                             // LOAD <-> SAVE AS
      projTab_ ^= 1;
      projCursor_ = 0;
      projTop_ = 0;
      break;
    default: break;
  }
}

void Ui::selectTrack(int t) {
  sel_ = uint8_t(t);
  if (view_ == View::Mix) {
    focus_ = ValueRef{false, sel_, kMixRowParam[mixRow_]};
  } else if (view_ == View::Fx) {
    if (fxTab_ == kFxDuck) focus_ = ValueRef{false, sel_, kMixDuck};
  } else {
    focus_ = ValueRef{false, sel_, uint8_t(focusIndexFor(t))};
  }
  listUntil_ = 0;
  for (uint32_t& ms : macMoveMs_) ms = 0;
  const int pages = (trackLen(t) + 15) / 16;
  if (shownPage_ >= pages) shownPage_ = uint8_t(pages - 1);
}

// ---- step keys (4.1, 4.2, 7, 9.2) --------------------------------------------

void Ui::stepDown(int k, int idx16, bool shift, bool ctrl) {
  const int abs = shownPage_ * 16 + idx16;
  if (heldStep_ >= 0) {
    // Tap another step = copy the held step there (7). Works across pages via '.
    if (abs < trackLen(heldTrack_) && abs != heldStep_) {
      const Step src = stepAt(heldTrack_, heldStep_), dst = stepAt(heldTrack_, abs);
      if (std::memcmp(&src, &dst, sizeof(Step)) != 0) undoForStep(heldTrack_, heldStep_, " COPY STEP ");
      setStep(heldTrack_, abs, src);
    }
    return;
  }
  char buf[32];
  if (shift && ctrl) {        // CTRL+SHIFT+step: selected track length (polymeter)
    char* o = cat(buf, kTrackNames[sel_]);
    o = cat(o, " LENGTH ");
    catInt(o, abs + 1);
    snapshotUndo(pat_, buf);
    setTrackLength(sel_, abs + 1);
    cat(o + std::strlen(o), "  CTRL+Z UNDO");
    toast(buf, 1);
    return;
  }
  if (shift) {                // SHIFT+step: pattern length = every track's length
    catInt(cat(buf, "LENGTH "), abs + 1);
    snapshotUndo(pat_, buf);
    setPatternLength(abs + 1);
    for (int t = 0; t < kNumTracks; ++t) setTrackLength(t, abs + 1);
    cat(buf + std::strlen(buf), "  CTRL+Z UNDO");
    toast(buf, 1);
    return;
  }
  if (abs >= trackLen(sel_)) {
    toast("BEYOND LENGTH - SHIFT+STEP SETS LENGTH", 0);
    return;
  }
  heldStep_ = int8_t(abs);
  heldGest_ = ++gestSeq_;   // everything until this key is released is one undo unit
  heldKey_ = uint8_t(k);
  heldTrack_ = sel_;
  heldPattern_ = pat_;
  heldDownMs_ = now_;
  heldOther_ = heldEdited_ = false;
  const Step s = stepAt(sel_, abs);
  heldWasOn_ = (s.flags() & kStepOn) != 0;
  if (!stepWriteActive() && !heldWasOn_) setStep(sel_, abs, newStep(sel_));   // on at key-down
  rec_[k].act = kActStep;
}

void Ui::stepUp(int k) {
  if (heldStep_ < 0 || heldKey_ != k) return;
  const bool tap = !heldOther_ && !after(now_, heldDownMs_ + kHoldMs);
  if (stepWriteActive()) {
    if (tap) stepCursor_ = uint8_t(heldStep_);
  } else if (heldWasOn_ && tap && !heldEdited_) {
    Step s = stepAt(heldTrack_, heldStep_);
    s.clearFlags(kStepOn);
    setStep(heldTrack_, heldStep_, s);
  }
  heldStep_ = -1;
}

// ---- pads (4.3) and step-edit field selection ---------------------------------

void Ui::padDown(int k, int t, bool shift) {
  if (heldStep_ >= 0) {
    if (view_ == View::Groove && fieldAvailable(heldTrack_, t)) {
      // The MOTION field again on an instrument track: the next macro (its value is the one edited).
      const macro::MacroSet* set = instrumentOf(heldTrack_);
      if (t == kFLock && stepField_ == kFLock && set && set->numMacros > 0) {
        const int f = focusIndexFor(heldTrack_) - kMacroIndex0;
        trackFocus_[heldTrack_] = uint8_t(kMacroIndex0 + (f + 1) % set->numMacros);
        ++serial_;
      }
      stepField_ = uint8_t(t);
    }
    return;   // other views: pads are inert while a step is held
  }
  selectTrack(t);
  if (shift) return;                         // SHIFT+pad: select silently
  const int note = isSynth(t) ? noteFor(t, 0) : 60;
  const uint8_t chord = t == kTrackLead ? chordForDegree(0) : 0;
  noteOn(t, note, 100, 0, chord);
  rec_[k] = KeyRec{kActPad, uint8_t(t), uint8_t(note), 0};
}

// ---- note row (4.4, 7, 9.2) / column keys ------------------------------------

void Ui::columnKey(int n) {
  if (view_ == View::Sound) {
    const int idx = soundPage_[sel_] * 8 + n;
    if (idx >= kSoundParams || !eng_->trackParamInfo(sel_, idx)) return;
    trackFocus_[sel_] = uint8_t(idx);
    focus_ = ValueRef{false, sel_, uint8_t(idx)};
  } else if (view_ == View::Mix) {
    if (n >= kNumMixRows) return;
    mixRow_ = uint8_t(n);
    focus_ = ValueRef{false, sel_, kMixRowParam[n]};
  } else if (view_ == View::Fx) {
    const uint8_t g = kFxPage[fxTab_][n];
    if (g == 0xFF) return;
    fxFocus_[fxTab_] = uint8_t(n);
    focus_ = ValueRef{true, 0, g};
  }
}

void Ui::noteDown(int k, int n, bool shift) {
  if (view_ == View::Sound && instrumentPage()) { macroKeyDown(k, n, shift); return; }
  if (view_ == View::Sound || view_ == View::Mix || view_ == View::Fx) { columnKey(n); return; }
  if (heldStep_ >= 0) { stepEditNote(k, n); return; }
  if (stepWriteActive() && view_ == View::Groove) { stepWriteNote(k, n, shift); return; }
  playNote(k, n, shift);
}

void Ui::playNote(int k, int n, bool shift) {
  const int t = sel_;
  int note = 60, vel = 100;
  uint8_t chord = 0, flags = shift ? uint8_t(kStepAccent) : uint8_t(0);
  if (isSynth(t)) {
    note = noteFor(t, n);
    if (t == kTrackLead) chord = chordForDegree(n);
    // Legato = slide: another note key is still held (4.4).
    for (int o = 0; o < 8; ++o)
      if (kKNote0 + o != k && isHeld(kKNote0 + o) && rec_[kKNote0 + o].act == kActNote) flags |= kStepSlide;
  } else {
    vel = kVelRow[n];
  }
  noteOn(t, note, vel, flags, chord);
  rec_[k] = KeyRec{kActNote, uint8_t(t), uint8_t(note), 0};
}

void Ui::stepEditNote(int k, int n) {
  if (view_ != View::Groove) return;
  const int t = heldTrack_;
  Step s = stepAt(t, heldStep_);
  if (isSynth(t)) {
    s.note = uint8_t(noteFor(t, n));
    if (t == kTrackLead) s.setChord(chordForDegree(n));
  } else {
    s.vel = kVelRow[n];
  }
  heldEdited(s);
  setStep(t, heldStep_, s);
  noteOn(t, s.note, s.vel, 0, s.chord());           // audition
  rec_[k] = KeyRec{kActNote, uint8_t(t), s.note, 0};
}

void Ui::stepWriteNote(int k, int n, bool shift) {
  const int t = sel_;
  const int len = trackLen(t);
  const int cur = stepCursor_ % len;
  // Previous note key still held = slide on the previous written step (303 style).
  if (isSynth(t)) {
    for (int o = 0; o < 8; ++o) {
      const int ok = kKNote0 + o;
      if (ok == k || !isHeld(ok) || rec_[ok].act != kActNote || !rec_[ok].c) continue;
      const int prev = rec_[ok].c - 1;
      Step p = stepAt(t, prev);
      if (p.flags() & kStepOn) {
        p.addFlags(kStepSlide);
        setStep(t, prev, p);
      }
    }
  }
  Step s = kDefaultStep;
  s.setFlags(uint8_t(kStepOn | (shift ? kStepAccent : 0)));
  if (isSynth(t)) {
    s.note = uint8_t(noteFor(t, n));
    s.vel = 100;
    s.gate = 2;
    if (t == kTrackLead) s.setChord(chordForDegree(n));
  } else {
    s.vel = kVelRow[n];
  }
  setStep(t, cur, s);
  noteOn(t, s.note, s.vel, s.flags() & kStepAccent, s.chord());
  rec_[k] = KeyRec{kActNote, uint8_t(t), s.note, uint8_t(cur + 1)};
  stepCursor_ = uint8_t((cur + 1) % len);
  shownPage_ = uint8_t(stepCursor_ / 16);
}

void Ui::semicolonDot(int dir, bool octave) {
  if (heldStep_ >= 0) {
    if (view_ != View::Groove) {
      // fall through to the view's page/row/tab keys below
    } else {
      const int t = heldTrack_;
      Step s = stepAt(t, heldStep_);
      const int lo = isSynth(t) ? 0 : 36, hi = isSynth(t) ? 127 : 84;
      const int nn = clampi(s.note + dir * (octave ? 12 : 1), lo, hi);
      if (nn == s.note) return;
      s.note = uint8_t(nn);
      heldEdited(s);
      setStep(t, heldStep_, s);
      return;
    }
  }
  switch (view_) {
    case View::Sound: {
      if (instrumentPage()) { stepInstrument(-dir); return; }   // ; = previous, . = next instrument
      int n = 0;
      while (n < kSoundParams && eng_->trackParamInfo(sel_, n)) ++n;
      const int pages = n ? (n + 7) / 8 : 1;
      soundPage_[sel_] = uint8_t((soundPage_[sel_] + pages - dir) % pages);   // ; = prev, . = next
      return;
    }
    case View::Mix:
      mixRow_ = uint8_t((mixRow_ + kNumMixRows - dir) % kNumMixRows);
      focus_ = ValueRef{false, sel_, kMixRowParam[mixRow_]};
      return;
    case View::Fx: fxTab_ = uint8_t((fxTab_ + kNumFxTabs - dir) % kNumFxTabs); return;
    case View::Groove: {
      if (!isSynth(sel_)) return;
      const int lo = sel_ == kTrackBass ? 0 : 1, hi = sel_ == kTrackBass ? 4 : 6;
      octave_[sel_] = uint8_t(clampi(octave_[sel_] + dir, lo, hi));
      settingsDirty_ = true;
      char buf[12];
      catInt(cat(buf, "OCT "), octave_[sel_]);
      toast(buf, 0);
      return;
    }
    default: return;
  }
}

void Ui::pageKey(bool shift) {
  if (view_ != View::Groove && view_ != View::Sound && view_ != View::Mix) return;
  if (shift) { addPage(); return; }
  const int pages = (trackLen(sel_) + 15) / 16;
  shownPage_ = uint8_t((shownPage_ + 1) % (pages < 1 ? 1 : pages));
}

void Ui::addPage() {
  const int plen = patternLen();
  if (plen >= kMaxSteps) { toast("64 STEPS IS THE MAXIMUM", 2); return; }
  snapshotUndo(pat_, "PAGE ADDED");
  for (int t = 0; t < kNumTracks; ++t) {
    const int len = trackLen(t);
    const int nl = len + 16 > kMaxSteps ? kMaxSteps : len + 16;
    for (int j = len; j < nl; ++j) {
      const int src = len >= 16 ? j - 16 : j % len;
      const Step s = stepAt(t, src);
      const Step d = stepAt(t, j);
      if (std::memcmp(&s, &d, sizeof(Step)) != 0) setStep(t, j, s);
    }
    setTrackLength(t, nl);
  }
  setPatternLength(plen + 16 > kMaxSteps ? kMaxSteps : plen + 16);
  shownPage_ = uint8_t((trackLen(sel_) - 1) / 16);
  char buf[32];
  cat(catInt(cat(buf, "LENGTH "), patternLen()), "  CTRL+Z UNDO");
  toast(buf, 1);
}

void Ui::chordKey(bool shift) {
  if (view_ == View::Sound && devMode_ && heldStep_ < 0) {   // developer mode: player page <> parameters
    devFlip_ = !devFlip_;
    toast(instrumentPage() ? "DEV: PLAYER PAGE" : "DEV: PARAMETER PAGES", 0);
    return;
  }
  if (heldStep_ >= 0) {
    if (view_ != View::Groove || heldTrack_ != kTrackLead) return;
    Step s = stepAt(heldTrack_, heldStep_);
    s.setChord(uint8_t((s.chord() + (shift ? kChordNum - 1 : 1)) % kChordNum));
    heldEdited(s);
    setStep(heldTrack_, heldStep_, s);
    return;
  }
  if (view_ != View::Groove || sel_ != kTrackLead) return;
  chordMode_ = uint8_t((chordMode_ + (shift ? kNumChordModes - 1 : 1)) % kNumChordModes);
  settingsDirty_ = true;
  char buf[16];
  cat(cat(buf, "CHORD "), chordModeName(chordMode_));
  toast(buf, 0);
}

void Ui::recToggle() {
  recOn_ = !recOn_;
  recGest_ = 0;   // a recording pass ends (or a new one starts)
  send(cmd(CmdType::SetRecord, 0, 0, recOn_ ? 1 : 0));
  if (recOn_ && !playing_) {
    // Step-write starts at step 1 of the shown page, or where it was left (9.2).
    if (stepCursor_ / 16 != shownPage_ || stepCursor_ >= trackLen(sel_)) stepCursor_ = uint8_t(shownPage_ * 16);
    if (stepCursor_ >= trackLen(sel_)) stepCursor_ = 0;
  }
}

void Ui::delDown(int k) {
  if (heldStep_ >= 0) {                       // remove the held step's p-lock / own macro value(s)
    Step s = stepAt(heldTrack_, heldStep_);
    if (!clearStepOwn(heldTrack_, s)) return;
    heldEdited(s);
    setStep(heldTrack_, heldStep_, s);
    return;
  }
  if (view_ != View::Groove) return;
  if (stepWriteActive()) {                    // rest: clear the cursor step and advance
    const int len = trackLen(sel_);
    const int cur = stepCursor_ % len;
    setStep(sel_, cur, kDefaultStep);
    stepCursor_ = uint8_t((cur + 1) % len);
    shownPage_ = uint8_t(stepCursor_ / 16);
    return;
  }
  if (!playing_) return;
  erasing_ = true;                            // erase as the playhead passes (8.5)
  eraseGest_ = ++gestSeq_;                    // the whole pass is one undo unit
  eraseSnapped_ = eraseDid_ = false;
  eraseLast_ = st_.step[sel_];
  rec_[k].act = kActErase;
}

// ---- modifier layers (8.1, 8.2, 8.3) ------------------------------------------

void Ui::fnCombo(int k) {
  const int pad = padKeyIndex(k), note = noteKeyIndex(k);
  if (pad >= 0) {
    setMute(pad, !muted(pad));
    solo_ = -1;                               // changing a mute ends the solo, keeps mutes
    return;
  }
  if (note >= 0) {
    if (solo_ == note) {
      for (int t = 0; t < kNumTracks; ++t) {
        const bool m = (soloSaved_ >> t) & 1;
        if (m != muted(t)) setMute(t, m);
      }
      solo_ = -1;
      return;
    }
    if (solo_ < 0) {
      soloSaved_ = 0;
      for (int t = 0; t < kNumTracks; ++t)
        if (muted(t)) soloSaved_ |= uint8_t(1u << t);
    }
    solo_ = int8_t(note);
    for (int t = 0; t < kNumTracks; ++t)
      if (muted(t) != (t != note)) setMute(t, t != note);
  }
}

void Ui::optCombo(int k, bool shift) {
  const int n = stepKeyIndex(k);
  if (n < 0) return;
  if (shift) {
    char buf[32];
    catP2(cat(buf, "COPIED TO P"), n + 1);
    if (n != pat_) {
      snapshotUndo(n, buf);                   // undo restores the destination
      send(cmd(CmdType::CopyPattern, pat_, n));
    }
    cat(buf + std::strlen(buf), "  CTRL+Z UNDO");
    toast(buf, 1);
  }
  send(cmd(CmdType::QueuePattern, n));
}

void Ui::sendRoll(int t, bool on) {
  Command c = cmd(CmdType::Roll, t, 0, on ? kRollCode[rollRate_] : 0);
  c.step = kDefaultStep;
  c.step.setFlags(kStepOn);
  c.step.vel = 100;
  if (isSynth(t)) {
    c.step.note = uint8_t(noteFor(t, 0));
    if (t == kTrackLead) c.step.setChord(chordForDegree(0));
  }
  send(c);
}

void Ui::altCombo(int k) {
  const int pad = padKeyIndex(k);
  if (pad >= 0) {
    rollMask_ |= uint8_t(1u << pad);
    sendRoll(pad, true);
    rec_[k] = KeyRec{kActRoll, uint8_t(pad), 0, 0};
    return;
  }
  if (k == kKSemicolon || k == kKDot) {
    const int nr = clampi(rollRate_ + (k == kKSemicolon ? 1 : -1), 0, kNumRollRates - 1);
    if (nr == rollRate_) return;
    rollRate_ = uint8_t(nr);
    settingsDirty_ = true;
    for (int t = 0; t < kNumTracks; ++t)
      if ((rollMask_ >> t) & 1) sendRoll(t, true);   // a running roll follows the new rate
  }
}

void Ui::tabUp(int k) {
  (void)k;
  const bool tap = !(comboMods_ & 8) && !after(now_, downMs_[kKTab] + kHoldMs);
  if (!tap) return;
  // Tap tempo (6.2): a gap over 2 s restarts; from the 3rd tap on, BPM = 60 / mean of the last <= 4 intervals.
  const uint32_t t = tabDownUs_;
  if (tapCount_ && t - taps_[tapCount_ - 1] > 2000000u) tapCount_ = 0;
  if (tapCount_ == 5) {
    std::memmove(taps_, taps_ + 1, sizeof(uint32_t) * 4);
    tapCount_ = 4;
  }
  taps_[tapCount_++] = t;
  if (tapCount_ >= 3) {
    const int n = tapCount_ - 1;
    const uint32_t span = taps_[tapCount_ - 1] - taps_[0];
    if (span == 0) return;
    const float bpm = 60.0e6f * float(n) / float(span);
    const ValueRef r{true, 0, kGTempo};
    const ParamInfo* info = infoFor(r);
    const int v = clampi(int(bpm + 0.5f), info ? info->min : 60, info ? info->max : 200);
    if (v != globalValue(kGTempo)) setValue(r, v);
    showPopup(r);
  }
}

// ---- value editing (5.2) -------------------------------------------------------

Ui::Target Ui::valueTarget() const {
  if (isHeld(kKTab)) return Target::Tempo;
  if (heldStep_ >= 0) return view_ == View::Groove ? Target::Field : Target::Lock;
  if (view_ == View::Song) return Target::Song;
  if (view_ == View::Project) return Target::Inert;
  return Target::Focus;
}

bool Ui::targetIsSmall(Target t) const {
  switch (t) {
    case Target::Focus: return isSmallRange(infoFor(focus_));
    case Target::Field: {
      if (stepField_ == kFRatch || stepField_ == kFChord || stepField_ == kFAcc || stepField_ == kFSlide) return true;
      if (stepField_ == kFLock) {
        if (instrumentIdOf(heldTrack_)) return false;   // a macro value
        const Step s = stepAt(heldTrack_, heldStep_);
        const int p = s.lockParam() ? s.lockParam() - 1 : trackFocus_[heldTrack_];
        return isSmallRange(trackInfo(heldTrack_, p));
      }
      return false;
    }
    case Target::Lock: return focus_.global ? false : isSmallRange(trackInfo(heldTrack_, focus_.index));
    default: return false;
  }
}

void Ui::valueKeyDown(int k, bool shift) {
  const int other = k == kKMinus ? kKEquals : kKMinus;
  if (isHeld(other) && rec_[other].act == kActValue) {
    resetValue();                               // both keys: default (5.2)
    repKey_ = -1;
    rec_[other].act = kActNone;
    return;
  }
  rec_[k].act = kActValue;
  const int dir = k == kKMinus ? -1 : 1;
  valueEvent(dir, 1, shift);
  if (valueTarget() == Target::Field && (stepField_ == kFAcc || stepField_ == kFSlide)) return;   // flips don't repeat
  repKey_ = int8_t(k);
  repDir_ = int8_t(dir);
  repStartMs_ = now_;
  repNextMs_ = now_ + kRepeatDelayMs;
}

void Ui::valueEvent(int dir, int mag, bool shift) {
  switch (valueTarget()) {
    case Target::Tempo: {
      const ValueRef r{true, 0, kGTempo};
      editFocus(r, dir * (shift ? 10 : mag), true);
      return;
    }
    case Target::Field: editField(dir, mag, shift); return;
    case Target::Lock: editLock(dir, mag, shift); return;
    case Target::Song: songRepeats(dir * (shift ? 8 : mag)); return;
    case Target::Focus: {
      const ParamInfo* info = infoFor(focus_);
      if (!info) return;
      int d = mag;
      if (shift) d = isSmallRange(info) ? 1 : 8;
      if (!focus_.global && isMacroIndex(focus_.index)) d = shift ? kMacroCoarse : kMacroTap * mag;
      editFocus(focus_, dir * d, true);
      return;
    }
    default: return;
  }
}

void Ui::editFocus(const ValueRef& r, int delta, bool popup) {
  const ParamInfo* info = infoFor(r);
  if (!info) return;
  const int cur = valueOf(r);
  const int nv = clampi(cur + delta, info->min, info->max);
  if (nv != cur) setValue(r, nv);                 // nothing is sent at a limit
  if (popup) showPopup(r);
}

void Ui::resetValue() {
  switch (valueTarget()) {
    case Target::Tempo: case Target::Focus: {
      const ValueRef r = valueTarget() == Target::Tempo ? ValueRef{true, 0, kGTempo} : focus_;
      const ParamInfo* info = infoFor(r);
      if (!info) return;
      if (valueOf(r) != info->def) setValue(r, info->def);
      toast("DEFAULT", 0);
      return;
    }
    case Target::Field: {
      Step s = stepAt(heldTrack_, heldStep_);
      switch (stepField_) {
        case kFVel: s.vel = kDefaultStep.vel; break;
        case kFGate: if (isSynth(heldTrack_)) s.gate = kDefaultStep.gate; else s.note = 60; break;
        case kFProb: s.setProb(100); break;
        case kFRatch: s.clearFlags(kStepRatchetMask); break;
        case kFChord: s.setChord(0); break;
        case kFLock: if (!clearStepOwn(heldTrack_, s)) return; break;
        default: return;
      }
      heldEdited(s);
      setStep(heldTrack_, heldStep_, s);
      toast("DEFAULT", 0);
      return;
    }
    case Target::Lock: {
      Step s = stepAt(heldTrack_, heldStep_);
      if (focus_.global || stepOwnValue(heldTrack_, s, focus_.index) < 0) return;
      if (instrumentIdOf(heldTrack_)) s.clearMacro(focus_.index - kMacroIndex0);
      else s.clearExt();
      heldEdited(s);
      setStep(heldTrack_, heldStep_, s);
      toast("DEFAULT", 0);
      return;
    }
    default: return;
  }
}

void Ui::editField(int dir, int mag, bool shift) {
  const int t = heldTrack_;
  if (!fieldAvailable(t, stepField_)) return;
  Step s = stepAt(t, heldStep_);
  const Step before = s;
  const int big = shift ? 8 : mag;
  switch (stepField_) {
    case kFVel: s.vel = uint8_t(clampi(s.vel + dir * big, 1, 127)); break;
    case kFGate:
      if (isSynth(t)) s.gate = uint8_t(clampi(s.gate + dir * big, 1, 64));
      else s.note = uint8_t(clampi(s.note + dir * (shift ? 12 : mag), 36, 84));   // PTCH 60 +- 24
      break;
    case kFProb: s.setProb(uint8_t(clampi((s.prob() + 2) / 5 * 5 + dir * 5 * (shift ? 4 : mag), 0, 100))); break;
    case kFRatch: {
      const int r = clampi(stepRatchet(s) + dir, 1, 4);
      s.setFlags(uint8_t((s.flags() & ~kStepRatchetMask) | ((r - 1) << kStepRatchetShift)));
      break;
    }
    case kFAcc: s.toggleFlags(kStepAccent); break;
    case kFSlide: s.toggleFlags(kStepSlide); break;
    case kFChord: s.setChord(uint8_t(clampi(s.chord() + dir, 0, kChordNum - 1))); break;
    case kFLock: {
      if (instrumentIdOf(t)) {   // the focused macro's own value on this note
        const int f = focusIndexFor(t);
        if (!isMacroIndex(f) || !instrumentOf(t)) return;
        stepMacroEdit(t, s, f - kMacroIndex0, dir, mag, shift);
        break;
      }
      const int p = s.lockParam() ? s.lockParam() - 1 : focusIndexFor(t);
      const ParamInfo* info = trackInfo(t, p);
      if (!info && !s.lockParam()) return;
      const int lo = info ? info->min : 0, hi = info ? info->max : (isMacroIndex(p) ? 255 : 127);
      int d = shift ? (isSmallRange(info) ? 1 : 8) : mag;
      if (isMacroIndex(p)) d = shift ? kMacroCoarse : kMacroTap * mag;
      const int base = s.lockParam() ? s.lockValue() : paramValue(t, p);
      s.setLock(uint8_t(p + 1), uint8_t(clampi(base + dir * d, lo, hi)));
      break;
    }
    default: return;
  }
  if (std::memcmp(&s, &before, sizeof(Step)) == 0) return;
  heldEdited(s);
  setStep(t, heldStep_, s);
}

// A held note's own value of macro m moves by whole levels of its 63 (a tap
// one level, about 4 of 255; repeats 2 and 4; SHIFT 4). Without a value yet it
// starts from the macro's position.
void Ui::stepMacroEdit(int t, Step& s, int m, int dir, int mag, bool shift) {
  const int from = s.hasMacro(m) ? s.macroCode(m) : macroPosToCode(macroValue(t, m));
  s.setMacroCode(m, clampi(from + dir * (shift ? 4 : mag), 1, kMacroCodeMax));
}

void Ui::editLock(int dir, int mag, bool shift) {
  if (focus_.global) {
    toast("FX VALUES CAN'T BE LOCKED", 2);
    return;
  }
  const int t = heldTrack_, p = focus_.index;
  const ParamInfo* info = trackInfo(t, p);
  if (!info) return;
  Step s = stepAt(t, heldStep_);
  if (instrumentIdOf(t)) {
    // An instrument track: a note carries its own macro values, nothing else.
    if (!isMacroIndex(p)) {
      toast("INSTRUMENT: HOLD STEP + A MACRO", 2);
      return;
    }
    stepMacroEdit(t, s, p - kMacroIndex0, dir, mag, shift);
    heldEdited(s);
    setStep(t, heldStep_, s);
    return;
  }
  int d = shift ? (isSmallRange(info) ? 1 : 8) : mag;
  if (isMacroIndex(p)) d = shift ? kMacroCoarse : kMacroTap * mag;
  int base;
  if (s.lockParam() == p + 1) {
    base = s.lockValue();
  } else {
    base = paramValue(t, p);
    if (s.lockParam()) {
      const ParamInfo* old = trackInfo(t, s.lockParam() - 1);
      char buf[40];
      char* o = cat(buf, "LOCK ");
      o = cat(o, info->name);
      o = cat(o, " REPLACES ");
      if (old) cat(o, old->name);
      else catInt(cat(o, "P"), s.lockParam() - 1);
      toast(buf, 0);
    }
  }
  s.setLock(uint8_t(p + 1), uint8_t(clampi(base + dir * d, info->min, info->max)));
  heldEdited(s);
  setStep(t, heldStep_, s);
}

// ---- [ ] performance sweeps (5.3) ----------------------------------------------

void Ui::sweepDown(int k, bool shift) {
  const int other = k == kKLBracket ? kKRBracket : kKLBracket;
  const ValueRef dj{true, 0, kGMasterFilter};
  // SHIFT: the bass's macro 1 (CLASSIC bass: its cutoff, as before instruments)
  const ValueRef cut{false, kTrackBass, uint8_t(instrumentOf(kTrackBass) ? kMacroIndex0 : 0)};
  if (isHeld(other) && rec_[other].act == kActSweep) {
    const bool bass = rec_[other].a != 0 || shift;
    if (bass) {
      if (paramValue(kTrackBass, cut.index) != bassReturn_) setValue(cut, bassReturn_);
      showPopup(cut);
    } else {
      if (globalValue(kGMasterFilter) != 64) setValue(dj, 64);
      toast("DJ FILTER OFF", 0);
    }
    swKey_ = -1;
    rec_[other].act = kActNone;
    return;
  }
  const bool bass = shift;
  const ValueRef r = bass ? cut : dj;
  if (bass && !(swKey_ >= 0 && swBass_)) bassReturn_ = uint8_t(paramValue(kTrackBass, cut.index));
  const int dir = k == kKLBracket ? -1 : 1;
  editFocus(r, dir * (isMacroIndex(r.index) ? kMacroTap : 1), true);   // the tap nudge
  swKey_ = int8_t(k);
  swDir_ = int8_t(dir);
  swBass_ = bass;
  swPos_ = float(valueOf(r));
  swNextMs_ = now_ + kSweepTapMs;
  swPauseUntil_ = now_;
  rec_[k] = KeyRec{kActSweep, uint8_t(bass ? 1 : 0), 0, 0};
}

// ============================================================== timers ====

void Ui::runRepeat() {
  const Target tg = valueTarget();
  const bool small = targetIsSmall(tg);
  const bool shift = isHeld(kKShift);
  for (int guard = 0; guard < 8 && after(now_, repNextMs_); ++guard) {
    const uint32_t rep = repNextMs_ - (repStartMs_ + kRepeatDelayMs);
    int mag = 1;
    if (!small && !shift) mag = rep < 500 ? 1 : (rep < 1000 ? 2 : 4);
    valueEvent(repDir_, mag, shift);
    repNextMs_ += small ? kRepeatSmallMs : kRepeatMs;
  }
  if (!after(repNextMs_, now_ - 1000)) repNextMs_ = now_;   // never fall far behind after a stall
}

void Ui::runTimers() {
  // -/= repeat
  if (repKey_ >= 0 && isHeld(repKey_)) runRepeat();
  else repKey_ = -1;
  runMacroRepeats();
  if (listUntil_ && (after(now_, listUntil_) || view_ != View::Sound)) { listUntil_ = 0; ++serial_; }
  // [ ] sweep
  if (swKey_ >= 0 && isHeld(swKey_)) {
    const ValueRef r = swBass_ ? ValueRef{false, kTrackBass, uint8_t(instrumentOf(kTrackBass) ? kMacroIndex0 : 0)}
                               : ValueRef{true, 0, kGMasterFilter};
    const bool mac = isMacroIndex(r.index);   // 0..255: twice the units per tick, the same time end to end
    for (int guard = 0; guard < 8 && after(now_, swNextMs_); ++guard) {
      const uint32_t tick = swNextMs_;
      swNextMs_ += kSweepTickMs;
      if (!after(tick, swPauseUntil_)) continue;
      const float prev = swPos_;
      swPos_ += float(swDir_) * kSweepPerTick * (mac ? 2.0f : 1.0f);
      if (!swBass_ && ((prev < 64.0f && swPos_ >= 64.0f) || (prev > 64.0f && swPos_ <= 64.0f))) {
        swPos_ = 64.0f;                            // detent at OFF
        swPauseUntil_ = tick + kDetentMs;
      }
      if (swPos_ < 0.0f) swPos_ = 0.0f;
      if (swPos_ > (mac ? 255.0f : 127.0f)) swPos_ = mac ? 255.0f : 127.0f;
      const int v = int(swPos_ + 0.5f);
      if (v != valueOf(r)) setValue(r, v);
      showPopup(r);
    }
    if (!after(swNextMs_, now_ - 1000)) swNextMs_ = now_;
  } else {
    swKey_ = -1;
  }
  // erase while DEL is held
  if (erasing_) {
    if (!playing_ || view_ != View::Groove || !isHeld(kKDel)) {
      erasing_ = false;
    } else {
      const int len = trackLen(sel_);
      int cur = st_.step[sel_] % len;
      int guard = 0;
      while (eraseLast_ != cur && guard++ < kMaxSteps) {
        eraseLast_ = uint8_t((eraseLast_ + 1) % len);
        Step s = stepAt(sel_, eraseLast_);
        if (s.flags() & kStepOn) {
          if (!eraseSnapped_) {                 // the first erase stores the undo snapshot (8.5)
            char lab[20];
            cat(cat(lab, kTrackNames[sel_]), " ERASED");
            snapshotUndo(pat_, lab, eraseGest_);
            eraseSnapped_ = true;
          }
          eraseDid_ = true;
          s.clearFlags(kStepOn);
          setStep(sel_, eraseLast_, s);
        }
      }
    }
  }
  // overlays: FN > OPT > ALT, 200 ms after the modifier alone or at once on a combo; TAB 250 ms
  Overlay ov = Overlay::None;
  if (isHeld(kKFn) && ((comboMods_ & 1) || after(now_, downMs_[kKFn] + kModOverlayMs))) ov = Overlay::Mute;
  else if (isHeld(kKOpt) && ((comboMods_ & 2) || after(now_, downMs_[kKOpt] + kModOverlayMs))) ov = Overlay::Pattern;
  else if (isHeld(kKAlt) && ((comboMods_ & 4) || after(now_, downMs_[kKAlt] + kModOverlayMs))) ov = Overlay::Roll;
  else if (isHeld(kKTab) && ((comboMods_ & 8) || after(now_, downMs_[kKTab] + kHoldMs))) ov = Overlay::Tempo;
  if (ov != overlay_) { overlay_ = ov; ++serial_; }
  // expiries
  if (toastUntil_ && after(now_, toastUntil_)) { toastUntil_ = 0; ++serial_; }
  if (popUntil_ && after(now_, popUntil_)) { popUntil_ = 0; ++serial_; }
}

void Ui::pollStatus() {
  st_ = eng_->status();
  // Sanitise: never index with a value the engine didn't promise.
  if (st_.pattern >= kNumPatterns) st_.pattern = 0;
  if (st_.queuedPattern >= kNumPatterns) st_.queuedPattern = 0xFF;
  for (int t = 0; t < kNumTracks; ++t)
    if (st_.step[t] >= kMaxSteps) st_.step[t] = 0;
  if (!(st_.peakL >= 0.0f)) st_.peakL = 0.0f;   // also catches NaN
  if (!(st_.peakR >= 0.0f)) st_.peakR = 0.0f;
  if (!(st_.duckGain >= 0.0f)) st_.duckGain = 1.0f;
  if (st_.duckGain > 1.0f) st_.duckGain = 1.0f;
  playing_ = st_.playing;
  if (st_.pattern != pat_) {
    pat_ = st_.pattern;
    ++serial_;
    if (heldStep_ >= 0 && heldPattern_ != pat_) {   // 16.4
      rec_[heldKey_].act = kActNone;
      heldStep_ = -1;
      toast("PATTERN CHANGED", 0);
    }
    const int pages = (trackLen(sel_) + 15) / 16;
    if (shownPage_ >= pages) shownPage_ = 0;
  }
  for (int t = 0; t < kNumTracks; ++t) {
    if (st_.trig[t] != lastTrig_[t]) {
      lastTrig_[t] = st_.trig[t];
      flashUntil_[t] = now_ + kFlashMs;
      lastTrigMs_[t] = now_;
    }
  }
  // Beat lamp: masterStep when the engine provides it, else the kick track's step.
  uint8_t ms = st_.masterStep < patternLen() ? st_.masterStep : st_.step[0];
  if (playing_ && ms != lastMaster_ && (ms & 3) == 0) {
    beatUntil_ = now_ + kFlashMs;
    lastBeatMs_ = now_;
  }
  lastMaster_ = playing_ ? ms : 0xFF;
  const float pk = st_.peakL > st_.peakR ? st_.peakL : st_.peakR;
  if (pk >= 1.0f) clipUntil_ = (now_ + 1000) | 1u;
  meter_ = pk > meter_ * 0.7f ? pk : meter_ * 0.7f;   // light decay so the meter reads at 30 fps
  if (meter_ < 0.004f) meter_ = 0.0f;
}

bool Ui::update(const UiEnv& env, Framebuffer& fb) {
  if (!eng_ || !proj_) return false;
  env_ = env;
  {
    // Downs the ghost filter held back for a partner that never came.
    GhostFilter::Out out[GhostFilter::kMaxOut];
    const int n = ghost_.tick(env.nowMs, out);
    for (int i = 0; i < n; ++i) keyEvent(out[i].ev, out[i].ms);
  }
  now_ = env.nowMs;
  evtGest_ = ++gestSeq_;   // this frame's timers (repeat, erase) are one event
  ++frame_;
  flushOutbox();
  pollStatus();
  if (writeBusy_ && (st_.writesDone == writeExpect_ || after(now_, writeSinceMs_ + 2000))) {
    writeBusy_ = false;   // the engine has consumed the buffer (or was re-initialised)
    ++serial_;
    if (undoQueued_) undo();   // CTRL+Z pressed while it was in flight
  }
  if (!(recOn_ && playing_)) recGest_ = 0;
  prunePending();
  runTimers();
  trackDirty();
  updateFocusLock();
  if (env.sdPresent != listedSd_ && view_ == View::Project) refreshFiles();   // SD inserted / pulled
  autosave();
  if (clipUntil_ && after(now_, clipUntil_)) clipUntil_ = 0;
  // The instrument page's picture: the engine taps the selected track only while it is shown.
  bool scopeMoved = false;
  if (scopeShown()) {
    scopeMoved = scope_.update(*eng_, sel_, now_);
    if (scope_.track() >= 0 && instrumentOf(sel_)) refreshJumps();
  } else if (scope_.track() >= 0) {
    scope_.stop(*eng_);
  }
  const uint32_t bs = bodySignature();
  const uint32_t ss = statusSignature();
  if (!drawn_ || bs != lastBodySig_ || (scopeMoved && scopeCovered())) {
    drawAll(fb);
    lastBodySig_ = bs;
    lastStatusSig_ = ss;
    drawn_ = true;
    dirtyStatusOnly_ = false;
    dirtyY0_ = 0;
    dirtyY1_ = kScreenH - 1;
    ++fullDraws_;
    return true;
  }
  const bool statusMoved = ss != lastStatusSig_ && !helpVisible();   // the help screen covers the status bar
  if (statusMoved) {
    drawStatus(fb);
    lastStatusSig_ = ss;
    dirtyStatusOnly_ = true;
    ++statusDraws_;
  }
  if (scopeMoved) {   // only the picture's band: a compact push to the panel
    drawScope(fb);
    dirtyY0_ = statusMoved ? 0 : uint8_t(kScopeY0);
    dirtyY1_ = uint8_t(kScopeY1);
    return true;
  }
  if (statusMoved) {
    dirtyY0_ = 0;
    dirtyY1_ = 10;
    return true;
  }
  return false;
}

}  // namespace ui
}  // namespace pg
