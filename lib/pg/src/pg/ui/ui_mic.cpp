// Recording from the microphone on a sampler instrument's page (stage F2; UX.md 10.3
// "Recording"; the platform side: pg/sampler/recorder.h MicInput).
//
// The gesture: hold L. A slot that holds a sample asks first (REPLACE SLOT n?, ENTER
// erases it; then hold L again). A slot that is empty but not erased (a cut-off
// transfer, an older firmware) is erased at once: ERASING is drawn first, the erase
// runs on the next frame (it blocks: the device fades its output out around it). Then
// the platform listens (WAITING) and starts on the first sound with a short pre-roll
// (REC, the level and the wave growing across the box, the box being the whole slot);
// letting go of L ends it, so does a full slot. The new sample is the picture at once
// and a toast names it.
//
// SHIFT+L is the same gesture with the engine's output as the source (resampling,
// UX.md 10.3 "Resampling"; pg/sampler/resampler.h): the same confirm, erase and
// picture; while the groove plays it counts the beats to the next bar line (a large
// 4 3 2 1), starts on it and ends on the bar line after L goes up (the picture marks
// the bar lines); stopped, it waits for the first sound. SHIFT may be let go once L
// is down: L's key-up ends it.
#include <cmath>
#include <cstring>

#include "gfx.h"
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
char* catInt(char* o, int v) { return o + itoa10(v, o); }
// "1.4S" from samples at 22,050 Hz
char* catSecs(char* o, uint32_t samples) {
  const int tenths = int((uint64_t(samples) * 10 + sampler::kPoolRate / 2) / sampler::kPoolRate);
  return cat(catInt(cat(catInt(o, tenths / 10), "."), tenths % 10), "S");
}
// 0..1 across -60..0 dBFS
float meterPos(int v) {
  if (v <= 32) return 0.0f;
  const float db = 20.0f * std::log10(float(v) * (1.0f / 32768.0f));
  const float p = (db + 60.0f) * (1.0f / 60.0f);
  return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

}  // namespace

// Resampling's count-in: the beats (4 steps) until the playing pattern's next bar
// line (its step 1, 17, 33 or 49, or its end), counting the one playing now: 4 3 2 1.
int Ui::beatsToBar() const {
  const int p = st_.pattern < kNumPatterns ? st_.pattern : 0;
  const int l = proj_->patterns[p].length;
  const int len = l == 0 ? 16 : (l > kMaxSteps ? kMaxSteps : l);
  const int pos = st_.masterStep;
  int next = (pos / 16 + 1) * 16;
  if (next > len) next = len;
  const int steps = next > pos ? next - pos : 1;
  return (steps + 3) / 4;
}

bool Ui::micPicture() const {
  return micState_ != kMicIdle && scopeShown() && samplerOn(sel_);
}

bool Ui::micKey(int k, bool shift) {
  if (view_ != View::Sound || !instrumentPage() || heldStep_ >= 0 || !samplerOn(sel_)) return false;
  if (micState_ != kMicIdle) return true;
  micRs_ = shift;
  sampler::MicInput* in = micSrc();
  const char* none = shift ? "NO RESAMPLING" : "NO MIC";
  if (!in) {
    toast(none, 2);
    return true;
  }
  const char* why = "";
  if (!in->ready(&why)) {
    toast(why && *why ? why : none, 2);
    return true;
  }
  const int slot = sampleSlotOf(sel_);
  micSlot_ = uint8_t(slot);
  sampler::SampleRef r;
  sampler::resolve(sampler_ ? sampler_->pool() : nullptr, slot, &r);
  if (!r.builtin) {   // it holds a sample: ask once before erasing it
    openConfirm(kConfirmReplaceSample, slot);
    return true;
  }
  rec_[k].act = kActMic;   // its key-up ends the recording
  if (!in->slotErased(slot)) {
    micState_ = kMicErase;
    micEraseShown_ = false;
    micThenStart_ = true;   // still held after the erase: it starts listening
    ++serial_;
    return true;
  }
  micStart();
  return true;
}

void Ui::micConfirmed() {
  if (!micSrc()) return;
  micState_ = kMicErase;
  micEraseShown_ = false;
  micThenStart_ = false;   // L was let go to answer: hold it again once it is erased
  ++serial_;
}

void Ui::micStart() {
  micView_ = sampler::RecView{};
  sampler::MicInput* in = micSrc();
  if (in->start(micSlot_)) {
    micState_ = kMicArmed;
    micView_.state = micRs_ ? sampler::RecState::Waiting : sampler::RecState::Settling;
    micView_.slot = micSlot_;
    micView_.resample = micRs_;
    micView_.sync = micRs_ && playing_;
  } else {
    micState_ = kMicIdle;
    sampler::RecView v;
    in->poll(&v);
    char buf[40];
    cat(cat(buf, micRs_ ? "RESAMPLE: " : "MIC: "), v.why && *v.why ? v.why : "FAILED");
    toast(buf, 2);
  }
  ++serial_;
}

void Ui::micRelease() {
  if (micState_ == kMicArmed) micSrc()->release();
  if (micState_ == kMicErase) micThenStart_ = false;
}

void Ui::micTick() {
  if (micState_ == kMicErase) {
    // ERASING must be on the screen before the erase blocks (one frame), unless the
    // page is not showing at all
    if (!micEraseShown_ && micPicture()) return;
    const bool ok = micSrc()->erase(micSlot_);
    micState_ = kMicIdle;
    ++serial_;
    char buf[40];
    if (!ok) {
      toast("ERASE FAILED", 2);
    } else if (micThenStart_ && isHeld(kKL)) {
      micStart();
    } else {
      cat(catInt(cat(buf, "SLOT "), micSlot_ + 1), " EMPTY");
      toast(buf, 0);
    }
    return;
  }
  if (micState_ != kMicArmed) return;
  sampler::RecView v;
  micSrc()->poll(&v);
  micView_ = v;
  char buf[40];
  switch (v.state) {
    case sampler::RecState::Done:
    case sampler::RecState::Full: {
      sampler::SampleRef r;
      sampler::resolve(sampler_ ? sampler_->pool() : nullptr, micSlot_, &r);
      char* o = cat(catInt(cat(buf, "SLOT "), micSlot_ + 1), " ");
      o = cat(cat(o, r.builtin ? "?" : r.name), " ");
      o = catSecs(o, r.builtin ? v.samples : r.length);
      if (v.resample && v.why && *v.why) cat(cat(o, ": "), v.why);   // NO WHOLE BAR (a bar longer than the slot)
      toast(buf, 0);
      micState_ = kMicIdle;
      ++serial_;
      break;
    }
    case sampler::RecState::Cancelled:
      toast("NO SOUND", 0);
      micState_ = kMicIdle;
      ++serial_;
      break;
    case sampler::RecState::Failed:
      cat(cat(buf, micRs_ ? "RESAMPLE: " : "MIC: "), v.why && *v.why ? v.why : "FAILED");
      toast(buf, 2);
      micState_ = kMicIdle;
      ++serial_;
      break;
    case sampler::RecState::Off:
      micState_ = kMicIdle;
      ++serial_;
      break;
    default:
      break;
  }
}

// The picture while erasing or recording (in the scope's box, as drawWave): the slot
// chip and ERASING / WAITING / REC with the time; the recording so far across the box
// (the box is the whole slot, so it fills up as the slot does), a white write head;
// the input level along the bottom (-60..0 dBFS) with a white tick at the threshold.
void Ui::drawMic(Framebuffer& fb) {
  Gfx g(fb);
  const int t = sel_;
  const int x0 = kScopeX0, y0 = kScopeY0, h = kScopeY1 - kScopeY0 + 1, w = Scope::kCols;
  g.rect(x0 - 4, y0, w + 8, h, BG);
  g.rrect(x0, y0, w, h, PANEL);
  char buf[24];
  catInt(buf, micSlot_ + 1);
  const int lx = g.chip(x0 + 2, y0 + 2, buf, tc(t), BG);
  if (micState_ == kMicErase) {
    g.textC(x0 + w / 2, y0 + h / 2 - 7, "ERASING", AMBER, 2);
    micEraseShown_ = true;
    return;
  }
  const sampler::RecView& v = micView_;
  const bool rec = v.state == sampler::RecState::Recording || v.state == sampler::RecState::Full ||
                   v.state == sampler::RecState::Done;
  // the state: a red dot (hollow while waiting) and its word
  const bool countIn = !rec && v.resample && v.sync;
  if (rec) {
    g.rect(lx + 3, y0 + 3, 5, 5, RED);
    g.text(lx + 11, y0 + 3, "REC", RED);
    catSecs(buf, v.samples);
    g.textR(x0 + w - 3, y0 + 3, buf, WHITE);
  } else {
    g.frame(lx + 3, y0 + 3, 5, 5, RED);
    if (!countIn) g.text(lx + 11, y0 + 3, "WAITING", TEXT);
  }
  // the recording so far
  const int top = y0 + 12, bot = y0 + h - 10;
  const int cy = (top + bot) / 2, half = (bot - top) / 2;
  int scale = v.peak;
  if (scale < int(v.threshold)) scale = v.threshold;
  if (scale < 64) scale = 64;
  for (int c = 0; c < w; c += 4) g.pixel(x0 + c, cy, RAISED);
  const int used = v.cols ? (v.colsUsed < w ? v.colsUsed : w) : 0;
  for (int c = 0; c < used; ++c) {
    int yt = cy - int(v.cols[c][1]) * half / scale, yb = cy - int(v.cols[c][0]) * half / scale;
    if (yt < top) yt = top;
    if (yb > bot) yb = bot;
    if (yb < yt) { const int s = yt; yt = yb; yb = s; }
    g.rect(x0 + c, yt, 1, yb - yt + 1, tc(t));
  }
  if (rec && used < w) g.rect(x0 + used, top, 1, bot - top + 1, WHITE);
  // resampling in time: the bar lines recorded so far (short WHITE ticks at the top
  // and bottom); counting in: the beats to the bar line where it starts, large
  if (v.resample && v.barAt)
    for (int b = 0; b < v.bars; ++b) {
      const int c = int(uint64_t(v.barAt[b]) * uint32_t(w) / sampler::kSlotMaxSamples);
      if (c <= 0 || c >= w) continue;
      g.rect(x0 + c, top, 1, 3, WHITE);
      g.rect(x0 + c, bot - 2, 1, 3, WHITE);
    }
  if (countIn) {
    catInt(buf, beatsToBar());
    g.textC(x0 + w / 2, cy - 10, buf, WHITE, 3);
  }
  // the level, and where the recording starts
  const int mx = x0 + 4, mw = w - 8, my = y0 + h - 6;
  g.rect(mx, my, mw, 3, RAISED);
  const int lv = int(meterPos(v.level) * float(mw) + 0.5f);
  const bool over = v.level >= v.threshold && v.threshold > 0;
  if (lv > 0) g.rect(mx, my, lv, 3, v.level >= 29000 ? uint8_t(RED) : (over ? tc(t) : tc(t, kMid)));
  if (v.threshold > 0) g.rect(mx + int(meterPos(v.threshold) * float(mw) + 0.5f), my - 1, 1, 5, WHITE);
}

}  // namespace ui
}  // namespace pg
