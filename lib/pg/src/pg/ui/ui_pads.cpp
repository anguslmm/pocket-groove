// Pads (docs/UX.md 10.3 "Pads"; the contract is pads.h): force-sensing pads as the UI
// sees them. The platform reads its sensors and calls padDown / padPressure / padUp.
// Here: what a hit plays (the pad's job), the note through the note keys' own path
// (Ui::noteOn: live record records it with its velocity), and the pressure offset,
// which shares the engine's per-macro offset with tilt (sendOffset, ui_tilt.cpp).
//
// DECISIONS
//  - A hit latches its track and note: the release, and the pressure while held, go to
//    what the hit played, whatever is selected or which job is on by then.
//  - Drums job: the pads never change the selected track (a kit is played without the
//    screen jumping); notes job: the selected track at the hit.
//  - Pressure per track is the largest of its held pads; its macro is latched with the
//    first pad of the track, and the offset glides back when the last one goes up.
//  - Pressure is rounded to whole position units and sent at most every kPressEveryMs
//    per track (a sensor read at 1 kHz would fill the engine's queue); update() sends
//    what waited.
#include "../macro/macro.h"
#include "ui.h"

namespace pg {
namespace ui {

namespace {
inline bool after(uint32_t now, uint32_t t) { return int32_t(now - t) >= 0; }
inline uint8_t pressByte(float p) {
  if (!(p > 0.0f)) return 0;
  return p >= 1.0f ? 255 : uint8_t(p * 255.0f + 0.5f);
}
}  // namespace

void Ui::setPadJob(PadJob job) {
  if (job == padJob_) return;
  padJob_ = job;
  ++serial_;
}

void Ui::togglePadJob() {
  setPadJob(padJob_ == PadJob::Drums ? PadJob::Notes : PadJob::Drums);
  toast(padJob_ == PadJob::Drums ? "PADS: DRUMS" : "PADS: NOTES", 0);
}

int Ui::padTrack(int pad) const {
  if (pad < 0 || pad >= kMaxPads) return -1;
  return padJob_ == PadJob::Drums ? drumPadTrack(pad) : int(sel_);
}

int Ui::padNote(int pad) const {
  const int t = padTrack(pad);
  if (t < 0) return -1;
  if (padJob_ == PadJob::Drums) return 60;
  if (isSynth(t)) return noteFor(t, pad);
  return samplerNote(pad);   // a drum or a sampler instrument: 60 + the scale's degree
}

#if PG_MAX_PADS > 0

bool Ui::padHeld(int pad) const { return pad >= 0 && pad < kMaxPads && pads_[pad].down; }
int Ui::pressOffset(int t) const { return t >= 0 && t < kNumTracks ? pressSent_[t] : 0; }
int Ui::pressMacro(int t) const { return t >= 0 && t < kNumTracks ? pressMac_[t] : -1; }

void Ui::padDown(int pad, float velocity, uint32_t nowMs) {
  if (pad < 0 || pad >= kMaxPads) return;
  now_ = nowMs;
  if (pads_[pad].down) padUp(pad, nowMs);   // a hit without the release seen: release first
  const int t = padTrack(pad);
  if (t < 0) return;
  const int note = padNote(pad);
  uint8_t flags = 0, chord = 0;
  if (padJob_ == PadJob::Notes && isSynth(t)) {
    if (t == kTrackLead) chord = chordForDegree(pad);
    // Legato = slide: another pad of this track is still held (as a second note key).
    for (int i = 0; i < kMaxPads; ++i)
      if (i != pad && pads_[i].down && pads_[i].track == t) flags |= kStepSlide;
  }
  // The track's pressure macro, latched with its first held pad.
  bool first = true;
  for (int i = 0; i < kMaxPads; ++i)
    if (pads_[i].down && pads_[i].track == t) first = false;
  if (first) {
    const macro::MacroSet* set = instrumentOf(t);
    const macro::PressAssign pa = set ? set->pressAssign() : macro::PressAssign{-1, 1};
    pressMac_[t] = pa.macro;
    pressResp_[t] = pa.response;
    pressSent_[t] = pressWant_[t] = 0;
    pressDirty_ = uint8_t(pressDirty_ & ~(1u << t));
  }
  pads_[pad] = PadSlot{int8_t(t), uint8_t(note), 1, 0};
  noteOn(t, note, padVelocity(velocity), flags, chord);
  ++serial_;
}

void Ui::padPressure(int pad, float pressure, uint32_t nowMs) {
  if (pad < 0 || pad >= kMaxPads || !pads_[pad].down) return;
  now_ = nowMs;
  const uint8_t p = pressByte(pressure);
  if (p == pads_[pad].press) return;
  pads_[pad].press = p;
  pressUpdate(pads_[pad].track, false);
}

void Ui::padUp(int pad, uint32_t nowMs) {
  if (pad < 0 || pad >= kMaxPads || !pads_[pad].down) return;
  now_ = nowMs;
  const PadSlot s = pads_[pad];
  pads_[pad] = PadSlot{-1, 0, 0, 0};
  noteOff(s.track, s.note);
  pressUpdate(s.track, true);
  ++serial_;
}

// Track t's pressure now (the largest of its held pads) to the engine: at once when
// force (a release) or when kPressEveryMs has passed since the last one, else later
// (pressTick). The last pad up: the glide back, and the macro is let go.
void Ui::pressUpdate(int t, bool force) {
  if (t < 0 || t >= kNumTracks || pressMac_[t] < 0) return;
  uint8_t mx = 0;
  bool any = false;
  for (int i = 0; i < kMaxPads; ++i)
    if (pads_[i].down && pads_[i].track == t) {
      any = true;
      if (pads_[i].press > mx) mx = pads_[i].press;
    }
  const int want = pressOffset16(float(mx) * (1.0f / 255.0f), pressResp_[t]);
  pressWant_[t] = int16_t(want);
  if (want == pressSent_[t] && any) {
    pressDirty_ = uint8_t(pressDirty_ & ~(1u << t));
    return;
  }
  if (!force && !after(now_, pressMs_[t] + kPressEveryMs)) {
    pressDirty_ = uint8_t(pressDirty_ | (1u << t));
    return;
  }
  pressDirty_ = uint8_t(pressDirty_ & ~(1u << t));
  const int m = pressMac_[t];
  if (want != pressSent_[t]) {
    pressSent_[t] = int16_t(want);
    sendOffset(t, m, any ? kPressRampMs[pressResp_[t] & 3] : kPressReturnMs);
    pressMs_[t] = now_;
  }
  if (!any) pressMac_[t] = -1;   // nothing held: the next first pad latches again
}

void Ui::pressTick() {
  if (!pressDirty_) return;
  for (int t = 0; t < kNumTracks; ++t)
    if ((pressDirty_ >> t) & 1u) pressUpdate(t, false);
}

#else  // no pads in this build: nothing to do, no state

bool Ui::padHeld(int) const { return false; }
int Ui::pressOffset(int) const { return 0; }
int Ui::pressMacro(int) const { return -1; }
void Ui::padDown(int, float, uint32_t) {}
void Ui::padPressure(int, float, uint32_t) {}
void Ui::padUp(int, uint32_t) {}
void Ui::pressUpdate(int, bool) {}
void Ui::pressTick() {}

#endif

}  // namespace ui
}  // namespace pg
