// Tilt (docs/UX.md 10.3 "Tilt"): the G0 gesture as the UI sees it. The host
// (firmware: src/app/main.cpp + src/app/tilt_input.cpp; desktop: host/app) owns
// G0 and the sensor; it calls tiltPress / tiltInput / tiltRelease / tiltCancel.
// Here: when a hold becomes a tilt, which track and macros it moves, the
// offsets sent to the engine (engine.h "TILT OFFSET"), and what the page shows.
//
// DECISIONS
//  - A tap never moves anything: the gesture engages after kTiltEngageMs of hold
//    (the G0 double-tap's taps are shorter). The pose at the press is still the
//    zero (the host's tracker takes it at the press), so a tilt begun in the first
//    150 ms joins in with the first glide.
//  - The track and its macros are taken when the gesture engages; selecting
//    another track during the hold changes nothing until the next hold.
//  - Sensitivity: an axis at its full range (+-1: the response's range, 30-60
//    degrees from the pose at the press) moves the macro kTiltScale = 160 units of
//    255: from the middle, either end is reached at 79 % of the range (about 28
//    degrees with "smooth"), from an end about two thirds of the way across.
//  - Each new input glides to its value in kTiltRampMs (the inputs come every
//    ~10 ms, so the macro never stops between them), release and cancel glide
//    back in kTiltReturnMs.
//  - Recording: the hold flag goes with the offsets once the tilt has left the
//    dead zone in this hold (a hold kept still, or G0 held for the volume keys,
//    records nothing); the engine makes the macro live until the release. An
//    unchanged input is re-sent every kTiltKeepMs so a transport or record
//    started during the hold picks the hand up.
//  - CLASSIC (or an instrument without tilt): nothing moves; one toast per hold.
#include <cstring>

#include "../tilt/tilt.h"
#include "keys.h"
#include "ui.h"

namespace pg {
namespace ui {

namespace {
inline bool after(uint32_t now, uint32_t t) { return int32_t(now - t) >= 0; }
inline float clampAxis(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : (v == v ? v : 0.0f)); }
}  // namespace

Ui::TiltSpec Ui::tiltSpec() const {
  TiltSpec s;
  s.track = int8_t(sel_);
  s.active = false;
  const macro::MacroSet* set = instrumentOf(sel_);
  for (int a = 0; a < 2; ++a) {
    s.macro[a] = -1;
    s.response[a] = uint8_t(tilt::kDefaultResponse);
    if (!set) continue;
    const macro::TiltAssign ta = set->tiltAssign(a);
    s.macro[a] = ta.macro;
    s.response[a] = ta.response;
    if (ta.macro >= 0) s.active = true;
  }
  return s;
}

void Ui::tiltPress(uint32_t nowMs) {
  now_ = nowMs;
  if (explain_) {   // explain mode: G0 describes itself (UX.md 10.10)
    explainKey_ = int8_t(kKG0);
    explainTable_ = uint8_t(helpTable());
    ++serial_;
    return;
  }
  if (tilt_.engaged) tiltLetGo(nowMs);   // a press without the release seen: start clean
  tilt_.down = true;
  tilt_.cancelled = false;
  tilt_.engaged = false;
  tilt_.moved = false;
  tilt_.recSnap = false;
  tilt_.in[0] = tilt_.in[1] = 0.0f;
  tilt_.downMs = nowMs;
}

void Ui::tiltEngage() {
  tilt_.engaged = false;
  const TiltSpec s = tiltSpec();
  if (!s.active) {   // CLASSIC, or an instrument with no tilt: one toast for the hold
    const macro::MacroSet* set = instrumentOf(sel_);
    toast(set ? "NO TILT ON THIS INSTRUMENT" : "TILT: PICK AN INSTRUMENT (9)", 0);
    tilt_.cancelled = true;   // nothing to do until G0 goes up
    return;
  }
  tilt_.engaged = true;
  tilt_.track = uint8_t(s.track);
  tilt_.macro[0] = s.macro[0];
  tilt_.macro[1] = s.macro[1];
  tilt_.sent[0] = tilt_.sent[1] = 0;
  tilt_.sentMs = now_;
  ++serial_;
}

// The offsets for the inputs now, to the engine (force: also when unchanged).
void Ui::tiltSend(bool force) {
  int16_t want[2];
  for (int a = 0; a < 2; ++a) {
    const float v = tilt_.in[a] * float(kTiltScale * kOffsetScale);
    want[a] = int16_t(v < 0.0f ? -int(-v + 0.5f) : int(v + 0.5f));
    if (tilt_.macro[a] >= 0 && want[a] != 0) tilt_.moved = true;
  }
  const bool changed = want[0] != tilt_.sent[0] || want[1] != tilt_.sent[1];
  if (!changed && !force) return;
  if (tilt_.moved && recOn_ && playing_ && !tilt_.recSnap) {
    // The engine writes the heard values into the notes it passes: one undo unit with
    // the pass (as a key move does).
    tilt_.recSnap = true;
    undoForRecording("RECORDED MOTION");
  }
  for (int a = 0; a < 2; ++a) {
    if (tilt_.macro[a] < 0) continue;
    send(macroOffsetCommand(tilt_.track, tilt_.macro[a], want[a], uint16_t(kTiltRampMs), tilt_.moved));
    tilt_.sent[a] = want[a];
  }
  tilt_.sentMs = now_;
}

void Ui::tiltInput(float x, float y, uint32_t nowMs) {
  now_ = nowMs;
  if (!tilt_.down || tilt_.cancelled) return;
  tilt_.in[0] = clampAxis(x);
  tilt_.in[1] = clampAxis(y);
  if (!tilt_.engaged) {
    if (!after(nowMs, tilt_.downMs + kTiltEngageMs)) return;
    tiltEngage();
    if (!tilt_.engaged) return;
  }
  tiltSend(after(nowMs, tilt_.sentMs + kTiltKeepMs));
}

// The gesture ends (release or cancel): the offsets glide back, live ends.
void Ui::tiltLetGo(uint32_t nowMs) {
  if (!tilt_.engaged) return;
  for (int a = 0; a < 2; ++a) {
    if (tilt_.macro[a] < 0) continue;
    send(macroOffsetCommand(tilt_.track, tilt_.macro[a], 0, uint16_t(kTiltReturnMs), false));
    tilt_.sent[a] = 0;
  }
  tilt_.engaged = false;
  tilt_.showUntil = (nowMs + kTiltReturnMs + 120) | 1u;   // the handles follow the glide back
  ++serial_;
}

void Ui::tiltRelease(uint32_t nowMs) {
  now_ = nowMs;
  tiltLetGo(nowMs);
  tilt_.down = false;
  tilt_.cancelled = false;
}

void Ui::tiltCancel(uint32_t nowMs) {
  now_ = nowMs;
  if (!tilt_.down) return;
  tiltLetGo(nowMs);
  tilt_.cancelled = true;   // G0 stays down for the volume keys; the next press tilts again
}

bool Ui::tiltShows(int t, int m) const {
  if (!tilt_.engaged && !(tilt_.showUntil && !after(now_, tilt_.showUntil))) return false;
  return (t & 7) == tilt_.track && (tilt_.macro[0] == m || tilt_.macro[1] == m);
}

uint8_t Ui::tiltMarks(int t, int m) const {
  const macro::MacroSet* set = instrumentOf(t);
  if (!set) return 0;
  uint8_t b = 0;
  for (int a = 0; a < 2; ++a)
    if (set->tiltAssign(a).macro == m) b = uint8_t(b | (1u << a));
  return b;
}

}  // namespace ui
}  // namespace pg
