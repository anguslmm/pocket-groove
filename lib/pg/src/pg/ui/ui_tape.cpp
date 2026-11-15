// The TAPE view (UX.md 10.11; the tape itself: pg/tape/tape.h). `p` in SONG.
//
// Keys on this view: z x c v arm tape track 1-4 (the one that records and that the
// fields edit), 1-4 mute it, ; . pick a field (SRC LOOP LVL PAN DLY REV), - = change
// it, L records (tap: on until L again, hold: until it is let go; it starts and ends on
// bar lines; SHIFT+L replaces instead of overdubbing), b n m held: BRAKE HALF REVERSE,
// del erases the armed track's loop (SHIFT: all of it), CTRL+Z undoes the last take
// or erase, CTRL+C lifts the loop into the sample slot. Everything else is global.
#include "ui.h"

#if PG_TAPE

#include <cmath>
#include <cstring>

#include "../io/project_io.h"
#include "gfx.h"
#include "keys.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {

char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
char* catInt(char* o, int v) { return o + itoa10(v, o); }
inline bool after(uint32_t now, uint32_t t) { return int32_t(now - t) >= 0; }
constexpr uint32_t kTapeHoldMs = 400;   // L held longer than this records only while held
void copyName(char* dst, const char* src) {
  int i = 0;
  for (; i < 8 && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
}
const char* mountOf(int m) { return m == kPmSd ? io::kMountSd : io::kMountInt; }
// "1.4S" / "12S" from samples at rate
char* catSecs(char* o, uint32_t samples, uint32_t rate) {
  if (rate == 0) rate = 48000;
  const uint32_t tenths = uint32_t((uint64_t(samples) * 10 + rate / 2) / rate);
  if (tenths >= 100) return cat(catInt(o, int(tenths / 10)), "S");
  return cat(catInt(cat(catInt(o, int(tenths / 10)), "."), int(tenths % 10)), "S");
}
// "9:41" from samples at rate
char* catMinSec(char* o, uint64_t samples, uint32_t rate) {
  const uint32_t s = uint32_t(samples / (rate ? rate : 48000));
  o = catInt(o, int(s / 60));
  *o++ = ':';
  *o++ = char('0' + (s % 60) / 10);
  *o++ = char('0' + s % 10);
  *o = 0;
  return o;
}
Command tcmd(tape::Op op, int b = 0, int c = 0, int note = 0) { return tape::tapeCommand(op, b, c, note); }
constexpr uint8_t kLaneCol[tape::kTracks] = {CYAN, GREEN, AMBER, tc(kTrackLead)};
constexpr uint8_t kLaneDim[tape::kTracks] = {CYAN_D, GREEN_D, AMBER_D, tc(kTrackLead, kDim)};
constexpr const char* kFieldName[kNumTapeFields] = {"SRC", "LOOP", "LVL", "PAN", "DLY", "REV"};
constexpr const char* kFieldLong[kNumTapeFields] = {"SOURCE", "LOOP", "LEVEL", "PAN", "DELAY", "REVERB"};
uint64_t stepFPAt(uint32_t rate, int bpm) {
  const uint64_t srQ8 = uint64_t(rate) * 256u;
  return ((srQ8 * 15u) << 24) / uint64_t(bpm > 0 ? bpm : 120);
}
uint8_t trickOfKey(int k) {
  switch (k - kKNote0) {
    case 4: return tape::kTrickBrake;
    case 5: return tape::kTrickHalf;
    case 6: return tape::kTrickReverse;
    default: return tape::kTrickNone;
  }
}

}  // namespace

void Ui::setTape(tape::Tape* t, tape::SlotWriter* lift, bool line) {
  tape_ = t && t->ready() ? t : nullptr;
  lift_ = lift;
  tapeLine_ = line;
  tapeSavedSig_ = tape_ ? tape_->signature() : 0;
  ++serial_;
}

int Ui::tapeValue(int t, int p) const {
  if (!tape_) return tape::kParamDefault[p % tape::kNumParams];
  t &= 3;
  p %= tape::kNumParams;
  if (tapePend_[t][p]) return tapePend_[t][p] - 1;
  return tape_->param(t, p);
}

int Ui::tapeLoopBars() const {
  if (tapeLoopPend_) return tapeLoopPend_;
  return tape_ ? tape_->loopBars() : tape::kDefaultLoop;
}

uint32_t Ui::tapeSpan(uint32_t* start) const {
  *start = 0;
  if (!tape_) return 1;
  const tape::View v = tape_->view();
  const uint64_t stepFP = stepFPAt(tape_->rate(), globalValue(kGTempo));
  if (proj_->songMode) {   // a page of 8 bars around the playhead
    const uint32_t span = tape_->nominalLoop(8, stepFP);
    *start = span ? (v.addr / span) * span : 0;
    return span ? span : 1;
  }
  const uint32_t nominal = tape_->nominalLoop(tapeLoopBars(), stepFP);
  const uint32_t span = v.rolling && !v.song && v.loopLen && v.loopLen > nominal / 2 && !tapeLoopPend_ ? v.loopLen : nominal;
  return span ? span : 1;
}

bool Ui::tapeRecording() const {
  if (!tape_) return false;
  if (tapeArmSent_) return true;
  const tape::Rec r = tape_->view().rec;
  return r == tape::Rec::CountIn || r == tape::Rec::Waiting || r == tape::Rec::Rec || r == tape::Rec::Ending;
}

int Ui::tapeLiftSlot() const { return samplerOn(sel_) ? sampleSlotOf(sel_) : sampleSlotOf(kTrackPerc); }

// ---- keys ------------------------------------------------------------------------------

bool Ui::tapeKey(int k, bool shift) {
  const int step = stepKeyIndex(k), note = noteKeyIndex(k);
  char buf[40];
  if (step >= 0) {   // 1-4: mute that tape track; the rest of the steps do nothing here
    if (step < tape::kTracks && tape_) {
      const int m = tapeValue(step, tape::kPMute) ? 0 : 1;
      send(tcmd(tape::kOpParam, step, m, tape::kPMute));
      tapePend_[step][tape::kPMute] = uint8_t(m + 1);
      tapePendFrame_[step][tape::kPMute] = frame_;
      ++serial_;
    }
    return true;
  }
  if (note >= 0) {
    if (note < tape::kTracks) {   // z x c v: arm a track
      if (tapeRecording() && note != tapeArm_) {
        toast("RECORDING", 0);
        return true;
      }
      tapeArm_ = uint8_t(note);
      ++serial_;
      return true;
    }
    const uint8_t trick = trickOfKey(k);
    if (trick != tape::kTrickNone && tape_) {   // b n m: held tricks
      send(tcmd(tape::kOpTrick, 0, trick));
      tapeTrickKey_ = int8_t(k);
      rec_[k].act = kActTrick;
      ++serial_;
    }
    return true;
  }
  switch (k) {
    case kKSemicolon: tapeField_ = uint8_t((tapeField_ + kNumTapeFields - 1) % kNumTapeFields); ++serial_; return true;
    case kKDot: tapeField_ = uint8_t((tapeField_ + 1) % kNumTapeFields); ++serial_; return true;
    case kKL: tapeRecDown(k, shift); return true;
    case kKDel:
      if (!tape_) { toast("NO TAPE", 2); return true; }
      if (tapeRecording()) { toast("RECORDING", 0); return true; }
      if (tape_->trackEmpty(tapeArm_)) {
        cat(catInt(cat(buf, "T"), tapeArm_ + 1), " IS EMPTY");
        toast(buf, 0);
        return true;
      }
      openConfirm(shift ? kConfirmTapeEraseAll : kConfirmTapeErase, tapeArm_);
      return true;
    case kKQuote: return true;
    default: return false;
  }
}

bool Ui::tapeCtrl(int k, bool /*shift*/) {
  if (k == kKNote0) {   // CTRL+Z: the tape's undo
    if (!tape_) { toast("NO TAPE", 2); return true; }
    if (!tape_->canUndo()) { toast("NOTHING TO UNDO", 0); return true; }
    if (!tape_->idle() || tapeRecording()) { toast("TAPE BUSY", 0); return true; }
    tape_->undo();
    toast(tape_->undoIsRedo() ? "TAPE UNDO  CTRL+Z REDO" : "TAPE REDO", 1);
    ++serial_;
    return true;
  }
  if (k == kKNote0 + 2) {   // CTRL+C: lift the loop into the sample slot
    tapeLift(false);
    return true;
  }
  return false;
}

void Ui::tapeRecDown(int k, bool shift) {
  if (!tape_) { toast("NO TAPE", 2); return; }
  if (tapeRecording()) {   // L again: the take ends on the next bar line (or does not start)
    send(tcmd(tape::kOpEnd));
    return;
  }
  if (!tape_->idle() || tapeIo_.busy() || tapeLoadWant_) { toast("TAPE BUSY", 0); return; }
  if (tapeSrc_ == tape::kSrcLine && !tapeLine_) { toast("NO LINE INPUT", 2); return; }
  if (!tape_->canRecord()) { toast("TAPE: MEMORY FULL", 2); return; }
  tape_->prepareTake();
  send(tcmd(tape::kOpArm, tapeArm_, tapeSrc_ | (shift ? tape::kArmReplace : 0), sel_));
  tapeArmSent_ = true;
  tapeArmFrame_ = frame_;
  rec_[k].act = kActTapeRec;
  tapeDownMs_ = now_;
  ++serial_;
}

void Ui::tapeRecUp(int /*k*/) {
  // a hold (not a tap) records while held: letting go ends it on the next bar line
  if (tape_ && after(now_, tapeDownMs_ + kTapeHoldMs)) send(tcmd(tape::kOpEnd));
}

void Ui::tapeTrickUp(int k) {
  if (!tape_ || tapeTrickKey_ != k) return;
  int other = -1;
  for (int n = 4; n <= 6; ++n) {
    const int kk = kKNote0 + n;
    if (kk != k && isHeld(kk) && rec_[kk].act == kActTrick) other = kk;
  }
  send(tcmd(tape::kOpTrick, 0, other >= 0 ? int(trickOfKey(other)) : int(tape::kTrickNone)));
  tapeTrickKey_ = int8_t(other);
  ++serial_;
}

void Ui::tapeEdit(int dir, int mag, bool shift) {
  if (!tape_) return;
  switch (tapeField_) {
    case kTfSource: {
      const int s = tapeSrc_ + dir;
      if (s >= 0 && s < tape::kNumSources) tapeSrc_ = uint8_t(s);
      break;
    }
    case kTfLoop: {
      int i = 0;
      for (int j = 0; j < tape::kNumLoops; ++j)
        if (tape::kLoopBars[j] == tapeLoopBars()) i = j;
      i += dir;
      if (i < 0 || i >= tape::kNumLoops) break;
      send(tcmd(tape::kOpLoop, 0, tape::kLoopBars[i]));
      tapeLoopPend_ = tape::kLoopBars[i];
      tapeLoopFrame_ = frame_;
      break;
    }
    default: {
      const int p = tapeField_ - kTfLevel;   // kPLevel kPPan kPDelay kPReverb, in that order
      const int cur = tapeValue(tapeArm_, p);
      int nv = cur + dir * (shift ? 8 : mag);
      nv = nv < 0 ? 0 : (nv > tape::kParamMax[p] ? tape::kParamMax[p] : nv);
      if (nv == cur) break;
      send(tcmd(tape::kOpParam, tapeArm_, nv, p));
      tapePend_[tapeArm_][p] = uint8_t(nv + 1);
      tapePendFrame_[tapeArm_][p] = frame_;
      break;
    }
  }
  ++serial_;
}

void Ui::tapeReset() {
  if (!tape_) return;
  if (tapeField_ == kTfSource) {
    tapeSrc_ = tape::kSrcMix;
  } else if (tapeField_ == kTfLoop) {
    send(tcmd(tape::kOpLoop, 0, tape::kDefaultLoop));
    tapeLoopPend_ = tape::kDefaultLoop;
    tapeLoopFrame_ = frame_;
  } else {
    const int p = tapeField_ - kTfLevel;
    send(tcmd(tape::kOpParam, tapeArm_, tape::kParamDefault[p], p));
    tapePend_[tapeArm_][p] = uint8_t(tape::kParamDefault[p] + 1);
    tapePendFrame_[tapeArm_][p] = frame_;
  }
  toast("DEFAULT", 0);
  ++serial_;
}

// ---- erase, lift ---------------------------------------------------------------------------

void Ui::tapeConfirmed(uint8_t action) {
  if (!tape_) return;
  char buf[40];
  if (action == kConfirmLift) {
    tapeLift(true);
    return;
  }
  if (!tape_->idle() || tapeRecording()) { toast("TAPE BUSY", 0); return; }
  const int t = confirmRow_ & 3;
  uint32_t start = 0;
  const uint32_t span = action == kConfirmTapeEraseAll ? tape_->trackCapacity() : tapeSpan(&start);
  const uint32_t from = action == kConfirmTapeEraseAll ? 0 : (proj_->songMode ? start : 0);
  if (!tape_->erase(t, from, from + span)) { toast("ERASE FAILED", 2); return; }
  char* o = catInt(cat(buf, "T"), t + 1);
  cat(o, action == kConfirmTapeEraseAll ? " ERASED  CTRL+Z UNDO" : " LOOP ERASED  CTRL+Z UNDO");
  toast(buf, 1);
  ++serial_;
}

void Ui::tapeLift(bool confirmed) {
  char buf[40];
  if (!tape_) { toast("NO TAPE", 2); return; }
  if (!lift_) { toast("NO LIFT", 2); return; }
  const int t = tapeArm_;
  if (tape_->trackEmpty(t)) {
    cat(catInt(cat(buf, "T"), t + 1), " IS EMPTY");
    toast(buf, 0);
    return;
  }
  if (!tape_->idle() || tapeRecording()) { toast("TAPE BUSY", 0); return; }
  const int slot = tapeLiftSlot();
  sampler::SampleRef r;
  sampler::resolve(samplePool(), slot, &r);
  if (!r.builtin && !confirmed) {   // it holds a sample: the usual question first
    openConfirm(kConfirmLift, slot);
    return;
  }
  // the loop, in whole bars as far as the slot takes them
  const uint64_t stepFP = stepFPAt(tape_->rate(), globalValue(kGTempo));
  const int bars = tapeLoopBars();
  const uint32_t loop = tape_->nominalLoop(bars, stepFP);
  const uint32_t bar = loop / uint32_t(bars > 0 ? bars : 1);
  const uint32_t cap = lift_->capacity(tape_->rate());
  uint32_t n = loop;
  if (n > cap) {
    const uint32_t k = bar ? cap / bar : 0;
    n = k ? k * bar : cap;
  }
  const char* why = "";
  if (!lift_->lift(slot, *tape_, t, 0, n, &why)) {
    cat(cat(buf, "LIFT: "), why && *why ? why : "FAILED");
    toast(buf, 2);
    return;
  }
  sampler::resolve(samplePool(), slot, &r);
  char* o = cat(catInt(cat(buf, "SLOT "), slot + 1), " ");
  if (!r.builtin) o = cat(cat(o, r.name), " ");
  catSecs(o, n, tape_->rate());
  toast(buf, 0);
  ++serial_;
}

// ---- files ---------------------------------------------------------------------------------

void Ui::tapeSave(int mount, const char* name) {
  if (!tape_ || !fs_) return;
  char dir[40], path[64];
  tape::folderOf(dir, mountOf(mount), name);
  cat(cat(path, dir), "/tape.txt");
  if (tape_->empty() && !fs_->exists(path)) {   // no tape here and never was: no folder
    tapeSavedSig_ = tape_->signature();
    return;
  }
  if (!tape_->idle()) { toast("TAPE BUSY: NOT SAVED", 2); return; }
  if (!tapeIo_.startSave(fs_, dir, tape_)) { toast(tapeIo_.why(), 2); return; }
  tapeSavedSig_ = tape_->signature();   // the job runs in tapeTick(), a piece per frame
}

void Ui::tapeLoad(int mount, const char* name) {
  if (!tape_) return;
  tapeLoadWant_ = true;
  tapeLoadMount_ = uint8_t(mount);
  copyName(tapeLoadName_, name ? name : "");
  for (auto& row : tapePend_) std::memset(row, 0, sizeof(row));
  tapeLoopPend_ = 0;
  tapeArmSent_ = false;
  ++serial_;
}

void Ui::tapeClear() { tapeLoad(kPmNone, ""); }

void Ui::tapeDelete(int mount, const char* name) {
  if (!fs_) return;
  char dir[40], path[64];
  tape::folderOf(dir, mountOf(mount), name);
  cat(cat(path, dir), "/tape.txt");
  if (fs_->exists(path)) fs_->remove(path);
  for (int t = 0; t < tape::kTracks; ++t) {
    char* o = cat(cat(path, dir), "/track");
    *o++ = char('1' + t);
    cat(o, ".wav");
    if (fs_->exists(path)) fs_->remove(path);
  }
}

// ---- every frame ---------------------------------------------------------------------------

void Ui::tapeTick() {
  if (!tape_) return;
  tape_->service();
  // a load (or a clear) waits for the tape to be idle: a take is merged first
  if (tapeLoadWant_ && tape_->idle()) {
    tapeLoadWant_ = false;
    if (tapeLoadMount_ == kPmInt || tapeLoadMount_ == kPmSd) {
      char dir[40];
      tape::folderOf(dir, mountOf(tapeLoadMount_), tapeLoadName_);
      if (!fs_ || !tapeIo_.startLoad(fs_, dir, tape_)) {
        if (fs_) toast(tapeIo_.why(), 2);
        tapeIo_.step();
      }
    } else {
      tape_->clear();
    }
    tapeSavedSig_ = tape_->signature();
    ++serial_;
  }
  if (tapeIo_.busy() || tapeIo_.state() != tape::TapeFiles::State::Idle) {
    const tape::TapeFiles::State s = tapeIo_.step(1u << 18);
    if (s == tape::TapeFiles::State::Done || s == tape::TapeFiles::State::Failed) {
      if (s == tape::TapeFiles::State::Failed) toast(tapeIo_.why(), 2);
      tapeSavedSig_ = tape_->signature();
      ++serial_;
    }
  }
  // the take: its end in a toast
  const tape::View v = tape_->view();
  const uint8_t r = uint8_t(v.rec);
  if (tapeArmSent_ && (v.rec != tape::Rec::Idle || uint8_t(frame_ - tapeArmFrame_) > 3)) tapeArmSent_ = false;
  const bool was = tapeLastRec_ == uint8_t(tape::Rec::Rec) || tapeLastRec_ == uint8_t(tape::Rec::Ending);
  const bool is = v.rec == tape::Rec::Rec || v.rec == tape::Rec::Ending;
  if (was && !is) {
    char buf[40];
    if (v.why == tape::kEndFull) {
      toast("TAPE: MEMORY FULL", 2);
    } else if (v.why == tape::kEndTapeEnd) {
      toast("TAPE: END OF TAPE", 2);
    } else {
      char* o = cat(catInt(cat(buf, "T"), v.armed + 1), " ");
      o = catSecs(o, v.recorded, tape_->rate());
      cat(o, "  CTRL+Z UNDO");
      toast(buf, 1);
    }
  }
  if (r != tapeLastRec_) ++serial_;
  tapeLastRec_ = r;
  // values shown before the audio thread has them
  for (int t = 0; t < tape::kTracks; ++t)
    for (int p = 0; p < tape::kNumParams; ++p)
      if (tapePend_[t][p] && (tape_->param(t, p) == tapePend_[t][p] - 1 || uint8_t(frame_ - tapePendFrame_[t][p]) > 30))
        tapePend_[t][p] = 0;
  if (tapeLoopPend_ && (tape_->loopBars() == tapeLoopPend_ || uint8_t(frame_ - tapeLoopFrame_) > 30)) tapeLoopPend_ = 0;
  // unsaved changes on tape make the project dirty
  if (!dirty_ && !tapeIo_.busy() && !tapeLoadWant_ && tape_->signature() != tapeSavedSig_) {
    dirty_ = true;
    settingsDirty_ = true;
    ++serial_;
  }
}

uint32_t Ui::tapeSignature() const {
  if (!tape_) return 0;
  const tape::View v = tape_->view();
  uint32_t start = 0;
  const uint32_t span = tapeSpan(&start);
  uint32_t h = 2166136261u;
  auto f = [&](uint32_t x) { h = (h ^ x) * 16777619u; };
  f(uint32_t(uint64_t(v.addr - start) * 220u / span));
  f(uint32_t(uint64_t(v.head - start) * 220u / span) | uint32_t(v.trick) << 16);
  f(uint32_t(v.rec) | uint32_t(v.countBeats) << 8 | uint32_t(v.armed) << 16 | uint32_t(v.source) << 24);
  f(v.recorded / 4096);
  f(tape_->revision());
  f(tape_->signature());
  f(tape_->freeBlocks());
  f(start ^ span);
  f(uint32_t(tapeArm_) | uint32_t(tapeSrc_) << 4 | uint32_t(tapeField_) << 8 | uint32_t(sel_) << 12 |
    uint32_t(isHeld(kKShift)) << 16 | uint32_t(isHeld(kKCtrl)) << 17 | uint32_t(tapeIo_.busy()) << 18);
  for (int t = 0; t < tape::kTracks; ++t)
    for (int p = 0; p < tape::kNumParams; ++p) f(uint32_t(tapeValue(t, p)));
  f(uint32_t(tapeLoopBars()));
  return h;
}

// ---- drawing -------------------------------------------------------------------------------

void Ui::drawTapeConfirm(char* title, char* l1, const char** yes) const {
  const int t = confirmRow_ & 3;
  switch (confirmAction_) {
    case kConfirmTapeErase:
      cat(cat(catInt(cat(title, "ERASE T"), t + 1), " LOOP"), "?");
      cat(l1, "CTRL+Z BRINGS IT BACK");
      *yes = "ENTER  ERASE";
      break;
    case kConfirmTapeEraseAll:
      cat(catInt(cat(title, "ERASE ALL OF T"), t + 1), "?");
      cat(l1, "CTRL+Z BRINGS IT BACK");
      *yes = "ENTER  ERASE";
      break;
    default: {   // kConfirmLift
      cat(catInt(cat(title, "REPLACE SLOT "), confirmRow_ + 1), "?");
      sampler::SampleRef r;
      sampler::resolve(samplePool(), confirmRow_, &r);
      cat(cat(l1, r.name), " IS ERASED");
      *yes = "ENTER  LIFT";
      break;
    }
  }
}

// The status bar's chip on TAPE: the armed track and the focused field.
void Ui::drawTapeChip(Framebuffer& fb) {
  Gfx g(fb);
  char t[12];
  catInt(cat(t, "T"), tapeArm_ + 1);
  const int x = g.chip(92, 1, t, kLaneCol[tapeArm_ & 3]);
  g.text(x + 3, 2, kFieldName[tapeField_], TEXT);
  char v[12];
  switch (tapeField_) {
    case kTfSource:
      cat(v, tapeSrc_ == tape::kSrcTrack ? kTrackNames[sel_] : tapeSrc_ == tape::kSrcMix ? "MIX" : "LINE");
      break;
    case kTfLoop: catInt(v, tapeLoopBars()); break;
    case kTfPan: {
      const int p = tapeValue(tapeArm_, tape::kPPan) - 64;
      if (p == 0) cat(v, "C");
      else catInt(cat(v, p < 0 ? "L" : "R"), p < 0 ? -p : p);
      break;
    }
    default: catInt(v, tapeValue(tapeArm_, tapeField_ - kTfLevel)); break;
  }
  g.textR(165, 2, v, WHITE);
}

void Ui::drawTape(Framebuffer& fb) {
  Gfx g(fb);
  char buf[40];
  int x = g.chip(4, 13, "TAPE", TEXT);
  if (!tape_) {
    g.text(x + 8, 14, "NO TAPE", DIM);
    return;
  }
  const tape::View v = tape_->view();
  const uint32_t rate = tape_->rate();
  const bool song = proj_->songMode != 0;
  const bool countIn = v.rec == tape::Rec::CountIn || v.rec == tape::Rec::Waiting || tapeArmSent_;
  const bool rec = v.rec == tape::Rec::Rec || v.rec == tape::Rec::Ending;
  // the header: LOOP n (or SONG), the record state, the tape's tempo when it differs, FREE
  if (song) {
    x = g.text(x + 6, 14, "SONG", GREEN);
  } else {
    catInt(cat(buf, "LOOP "), tapeLoopBars());
    x = g.text(x + 6, 14, buf, TEXT);
  }
  if (rec) {
    g.rect(x + 8, 14, 5, 5, RED);
    x = g.text(x + 16, 14, v.replace ? "REPLACE" : "REC", RED);
    catSecs(buf, v.recorded, rate);
    g.text(x + 6, 14, buf, WHITE);
  } else if (countIn) {
    g.frame(x + 8, 14, 5, 5, RED);
    if (v.rec == tape::Rec::CountIn && v.countBeats) {
      catInt(buf, v.countBeats);
      g.text(x + 16, 14, buf, WHITE);
    } else if (v.rec == tape::Rec::Waiting && playing_) {
      catInt(buf, beatsToBar());
      g.text(x + 16, 14, buf, WHITE);
    } else {
      g.text(x + 16, 14, "WAITING", TEXT);
    }
  }
  const uint64_t free = tape_->freeSamples();
  catMinSec(cat(buf, "FREE "), free, rate);
  int fx = 235 - tw(buf) + 1;
  if (fx > x + 30) g.text(fx, 14, buf, free < uint64_t(rate) * 10 ? uint8_t(AMBER) : uint8_t(DIM));
  else fx = 236;
  if (v.bpm && v.bpm != globalValue(kGTempo) && !rec) {
    catInt(buf, v.bpm);
    cat(buf + std::strlen(buf), " BPM");
    g.textR(fx - 8, 14, buf, AMBER);
  }
  // the lanes
  uint32_t start = 0;
  const uint32_t span = tapeSpan(&start);
  const int bx = 15, bw = 220;
  const uint64_t stepFP = stepFPAt(rate, globalValue(kGTempo));
  const uint32_t bar = tape_->nominalLoop(1, stepFP);
  for (int t = 0; t < tape::kTracks; ++t) {
    const int y0 = 25 + 20 * t, h = 18;
    const bool armed = t == tapeArm_;
    const bool muted = tapeValue(t, tape::kPMute) != 0;
    const uint8_t col = muted ? uint8_t(LINE) : (armed ? kLaneCol[t] : kLaneDim[t]);
    // the key cap: the track's number (z x c v arm it), its colour when armed
    if (armed) g.rrect(4, y0, 9, h, rec ? uint8_t(RED) : kLaneCol[t]);
    else g.rframe(4, y0, 9, h, RAISED);
    buf[0] = char('1' + t);
    buf[1] = 0;
    g.text(6, y0 + 2, buf, armed ? uint8_t(BG) : uint8_t(TEXT));
    if (muted) g.text(6, y0 + 10, "M", armed ? uint8_t(BG) : uint8_t(DIM));
    g.rrect(bx, y0, bw, h, PANEL);
    // bar lines
    if (bar > 0)
      for (uint64_t b = (uint64_t(start) + bar - 1) / bar * bar; b < uint64_t(start) + span; b += bar) {
        const int cx = bx + int((b - start) * uint64_t(bw) / span);
        if (cx <= bx) continue;
        for (int yy = y0 + 1; yy < y0 + h - 1; yy += 2) g.pixel(cx, yy, RAISED);
      }
    // the waveform: the peak of each column's stretch of tape, from the centre
    const int cy = y0 + h / 2, half = h / 2 - 1;
    for (int c = 0; c < bw; ++c) {
      const uint32_t a0 = start + uint32_t(uint64_t(c) * span / bw);
      const uint32_t a1 = start + uint32_t(uint64_t(c + 1) * span / bw);
      const uint8_t p = tape_->peak(t, a0, a1 > a0 ? a1 - a0 : 1);
      if (!p) continue;
      // a square-root scale: a quiet layer still shows (a peak of -24 dB fills a quarter)
      int hh = int(std::sqrt(float(p) * (1.0f / 255.0f)) * float(half) + 0.5f);
      if (hh < 1) hh = 1;
      g.rect(bx + c, cy - hh, 1, 2 * hh + 1, armed && rec ? uint8_t(RED) : col);
    }
    // the playhead (where the tape is) and, during a trick, where it is heard from
    if (v.rolling || rec) {
      const int px = bx + int(uint64_t(v.addr >= start ? v.addr - start : 0) * uint64_t(bw) / span);
      if (px >= bx && px < bx + bw) g.rect(px, y0, 1, h, WHITE);
      if (v.trick != tape::kTrickNone) {
        const int hx = bx + int(uint64_t(v.head >= start ? v.head - start : 0) * uint64_t(bw) / span);
        if (hx >= bx && hx < bx + bw) g.rect(hx, y0 + 2, 1, h - 4, AMBER);
      }
    }
  }
  // the count-in, large over the armed lane (as resampling shows it)
  if (countIn && v.rec == tape::Rec::CountIn && v.countBeats) {
    catInt(buf, v.countBeats);
    g.textC(bx + bw / 2, 25 + 20 * tapeArm_ + 2, buf, WHITE, 2);
  }
  // the fields of the armed track (SRC and LOOP are the tape's)
  for (int f = 0; f < kNumTapeFields; ++f) {
    const int fx2 = 4 + 39 * f, fy = 105;
    char val[12];
    switch (f) {
      case kTfSource: cat(val, tapeSrc_ == tape::kSrcTrack ? kTrackNames[sel_] : tapeSrc_ == tape::kSrcMix ? "MIX" : "LINE"); break;
      case kTfLoop: catInt(val, tapeLoopBars()); break;
      case kTfPan: {
        const int p = tapeValue(tapeArm_, tape::kPPan) - 64;
        if (p == 0) cat(val, "C");
        else catInt(cat(val, p < 0 ? "L" : "R"), p < 0 ? -p : p);
        break;
      }
      default: catInt(val, tapeValue(tapeArm_, f - kTfLevel)); break;
    }
    const bool on = f == tapeField_;   // the label over the value; the field -/= change is white
    if (on) g.rrect(fx2, fy, 37, 19, WHITE);
    g.textC(fx2 + 18, fy + 1, kFieldName[f], on ? uint8_t(BG) : uint8_t(DIM));
    g.textC(fx2 + 18, fy + 10, val, on ? uint8_t(BG) : uint8_t(TEXT));
  }
  // the footer: what the keys do now
  x = 4;
  if (isHeld(kKCtrl)) {
    x = g.text(x, 127, "CTRL+", DIM);
    x = g.text(x, 127, "C", TEXT);
    catInt(cat(buf, " LIFT TO SLOT "), tapeLiftSlot() + 1);
    x = g.text(x, 127, buf, DIM);
    x = g.text(x + 6, 127, "Z", TEXT);
    g.text(x, 127, " UNDO", DIM);
  } else if (isHeld(kKShift)) {
    x = g.text(x, 127, "SFT+", DIM);
    x = g.text(x, 127, "L", TEXT);
    x = g.text(x, 127, " REPLACE  ", DIM);
    x = g.text(x, 127, "DEL", TEXT);
    g.text(x, 127, " ALL OF T", DIM);
  } else if (rec || countIn) {
    x = g.text(x, 127, "L", TEXT);
    g.text(x, 127, " ENDS ON THE BAR LINE", DIM);
  } else {
    x = g.text(x, 127, "L", TEXT);
    x = g.text(x, 127, " REC  ", DIM);
    x = g.text(x, 127, "1-4", TEXT);
    x = g.text(x, 127, " MUTE  ", DIM);
    x = g.text(x, 127, "B N M", TEXT);
    g.text(x, 127, " BRAKE HALF REV", DIM);
  }
}

}  // namespace ui
}  // namespace pg

#endif  // PG_TAPE
