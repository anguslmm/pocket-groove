// Pocket Groove UI: drawing. Every coordinate here comes from the mockup code
// (tools/ux_mock/screens.py), which wins over the prose in docs/UX.md.
#include <cmath>
#include <cstring>

#include "font.h"
#include "gfx.h"
#include "keys.h"
#include "ui.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {

constexpr int W = kScreenW;

// Python's round() (half to even), so bar lengths match the mockups pixel for pixel.
int rnd(float x) {
  const float f = floorf(x);
  const float d = x - f;
  int i = int(f);
  if (d > 0.5f) return i + 1;
  if (d < 0.5f) return i;
  return (i & 1) ? i + 1 : i;
}
inline bool before(uint32_t now, uint32_t t) { return int32_t(now - t) < 0; }

// The tilt mark (UX.md 10.3 "Tilt") at (x, y), a glyph's 7 rows: a double arrow,
// left-right for axis X (roll; 7 px wide), up-down for axis Y (away / toward; 5 wide).
int tiltGlyphW(int axis) { return axis == 0 ? 7 : 5; }
void tiltGlyph(Gfx& g, int x, int y, int axis, uint8_t c) {
  if (axis == 0) {
    g.rect(x, y + 3, 7, 1, c);
    g.rect(x + 1, y + 2, 1, 3, c);
    g.rect(x + 5, y + 2, 1, 3, c);
  } else {
    g.rect(x + 2, y, 1, 7, c);
    g.rect(x + 1, y + 1, 3, 1, c);
    g.rect(x + 1, y + 5, 3, 1, c);
  }
}

char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
char* catInt(char* o, int v) { return o + itoa10(v, o); }
char* catP2(char* o, int n) {
  *o++ = char('0' + (n / 10) % 10);
  *o++ = char('0' + n % 10);
  *o = 0;
  return o;
}
char* cat3(char* o, int v) {   // "%3d"
  char t[8];
  itoa10(v, t);
  int n = int(std::strlen(t));
  for (int i = n; i < 3; ++i) *o++ = ' ';
  return cat(o, t);
}
void keyStr(char* o, char c) { o[0] = c; o[1] = 0; }

int padX(int i) { return 4 + (i % 8) * 29 + ((i % 8) / 4) * 2; }
int padY(int i) { return i < 8 ? 88 : 108; }
int ovX(int i) { return 42 + i * 11 + (i / 4) * 2; }
int stripX(int i) { return 4 + i * 14 + (i / 4) * 2; }
constexpr int PAD_W = 27, PAD_H = 18;

uint32_t fnv(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 16777619u;
  }
  return h;
}

// One parameter column as the mockups' P()/PI() describe it.
struct Col {
  bool used;
  BarKind kind;
  int n;            // number of values
  int val;          // enum: v-min; else 0..127 normalised
  int lockVal;      // -1 = no lock shown
  char name[6];
  char text[kValueTextCap];
  char lockText[kValueTextCap];
};

void makeCol(Col& c, const ParamInfo* info, const ValueRef& ref, int v, int lock) {
  c.used = info != nullptr;
  c.lockVal = -1;
  c.lockText[0] = 0;
  if (!info) return;
  c.kind = barKind(info, ref);
  c.n = info->max - info->min + 1;
  upperCopy(info->name, c.name, sizeof(c.name));
  formatValue(info, ref, v, c.text);
  c.val = c.kind == BarKind::Segments ? v - info->min : rnd(valueFrac(info, v) * 127.0f);
  if (lock >= 0) {
    c.lockVal = c.kind == BarKind::Segments ? lock - info->min : rnd(valueFrac(info, lock) * 127.0f);
    formatValue(info, ref, lock, c.lockText);
  }
}

// screens.param_cols
void paramCols(Gfx& g, const Col* cols, int focus, uint8_t bright, uint8_t mid, int y0, int barH) {
  const int by = y0 + 9;
  for (int c = 0; c < 8; ++c) {
    const Col& p = cols[c];
    const int cx = c * 30 + 15;
    const bool f = c == focus;
    if (!p.used) {
      g.textC(cx, y0, "-", LINE);
      continue;
    }
    if (f) {
      int n = int(std::strlen(p.name));
      const int w = (n > 3 ? n : 3) * 6 + 3;
      g.rrect(cx - w / 2, y0 - 1, w, 9, bright);
      g.textC(cx, y0, p.name, BG);
    } else {
      g.textC(cx, y0, p.name, TEXT);
    }
    const int bx = cx - 7, bw = 14;
    g.rect(bx, by, bw, barH, f ? RAISED : PANEL);
    uint8_t fill = f ? bright : mid;
    const bool locked = p.lockVal >= 0;
    if (locked) fill = AMBER;
    const int v = locked ? p.lockVal : p.val;
    if (p.kind == BarKind::Segments && p.n > 8) {
      const int hpx = rnd(float(v + 1) / float(p.n) * float(barH));
      g.rect(bx, by + barH - hpx, bw, hpx, fill);
      for (int k = 1; k < p.n; ++k)
        g.rect(bx, by + barH - rnd(float(k) / float(p.n) * float(barH)), bw, 1, f ? RAISED : PANEL);
    } else if (p.kind == BarKind::Segments) {
      const int sh = barH / p.n;
      for (int k = 0; k < p.n; ++k) {
        const int yy = by + barH - (k + 1) * sh;
        g.rect(bx + 2, yy + 1, bw - 4, sh - 2, k == v ? fill : uint8_t(LINE));
      }
    } else if (p.kind == BarKind::Bipolar) {
      const int midy = by + barH / 2;
      const int d = rnd(float(v - 64) / 64.0f * float(barH / 2));
      if (d >= 0) g.rect(bx, midy - d, bw, d + 1, fill);
      else g.rect(bx, midy, bw, -d, fill);
      g.rect(bx - 2, midy, bw + 4, 1, DIM);
    } else {
      const int hpx = rnd(float(v) / 127.0f * float(barH));
      g.rect(bx, by + barH - hpx, bw, hpx, fill);
      if (locked) {
        const int ty = by + barH - rnd(float(p.val) / 127.0f * float(barH));
        g.rect(bx - 2, ty, bw + 4, 1, WHITE);
      }
    }
    g.textC(cx, by + barH + 3, locked ? p.lockText : p.text, locked ? AMBER : (f ? WHITE : TEXT));
    char k[2];
    keyStr(k, kNoteKeys[c]);
    g.textC(cx, by + barH + 12, k, f ? WHITE : DIM);
  }
}

void bigValue(Gfx& g, const char* longName, const char* val, uint8_t col) {
  char up[16];
  upperCopy(longName, up, sizeof(up));
  g.textR(235, 13, val, col, 2);
  g.textR(235 - tw(val, 2) - 6, 19, up, DIM);
}

void footerRight(Gfx& g, const char* s, uint8_t c) { g.textR(235, 127, s, c); }

}  // namespace

// ============================================================== status bar ====

namespace {
struct StatusModel {
  bool playing, rec, recLit;
  uint8_t pat, queued;          // queued 0xFF none
  bool queueLit;
  int tempo;
  bool beat;
  uint8_t chipCol;
  char tag[6], name[6], val[kValueTextCap];
  int meterPx;
  bool clip;
  int duckPx;
  int battPx;
  uint8_t battCol;
  bool battLit;
};
}  // namespace

uint32_t Ui::statusSignature() const {
  // Hash of exactly what the status bar draws (see drawStatus).
  uint32_t h = 2166136261u;
  h = fnv(h, uint32_t(playing_) | uint32_t(recOn_) << 1 | uint32_t(view_) << 4);
  h = fnv(h, recOn_ && !playing_ ? (now_ / 500) & 1 : 2);
  h = fnv(h, uint32_t(pat_) | uint32_t(st_.queuedPattern) << 8 | ((now_ / 250) & 1) << 16);
  h = fnv(h, uint32_t(globalValue(kGTempo)) | uint32_t(before(now_, beatUntil_)) << 8);
  h = fnv(h, uint32_t(focus_.global) | uint32_t(focus_.track) << 8 | uint32_t(focus_.index) << 16);
  h = fnv(h, uint32_t(focus_.global ? globalValue(focus_.index) : paramValue(focus_.track, focus_.index)));
  h = fnv(h, serial_);
  h = fnv(h, uint32_t(dirty_) | uint32_t(songCursor_) << 1);
  const int n = rnd((meter_ > 1.0f ? 1.0f : meter_) * 34.0f);
  h = fnv(h, uint32_t(n) | uint32_t(clipUntil_ != 0) << 8);
  h = fnv(h, uint32_t(rnd((1.0f - st_.duckGain) * 9.0f)));
  h = fnv(h, uint32_t(env_.batteryPct) | uint32_t(env_.batteryPct < 10 ? (now_ / 500) % 4 == 0 : 0) << 8 |
                 uint32_t(focusLocked_) << 9);
  if (tilt_.engaged)   // the tilt indicator's dot
    h = fnv(h, 1u | uint32_t(rnd(tilt_.in[0] * 3.0f) + 8) << 1 | uint32_t(rnd(tilt_.in[1] * 3.0f) + 8) << 6);
  return h;
}

void Ui::drawStatus(Framebuffer& fb) {
  Gfx g(fb);
  g.rect(0, 0, W, 11, PANEL);
  g.text(3, 2, playing_ ? PG_ICON_PLAY : PG_ICON_STOP, playing_ ? GREEN : DIM);
  // REC: solid in live rec, 1 Hz blink in step-write.
  uint8_t rc = RAISED;
  if (recOn_) rc = (playing_ || ((now_ / 500) & 1) == 0) ? RED : RAISED;
  g.text(11, 2, PG_ICON_REC, rc);
  char buf[24];
  catP2(cat(buf, "P"), pat_ + 1);
  int x = g.text(20, 2, buf, WHITE);
  if (st_.queuedPattern != 0xFF && st_.queuedPattern != pat_) {
    if (((now_ / 250) & 1) == 0) {
      catP2(cat(buf, ">"), st_.queuedPattern + 1);
      g.text(x, 2, buf, AMBER);
    }
  } else if (proj_->songMode) {
    g.text(x + 1, 2, PG_ICON_CHAIN, GREEN);
    catP2(buf, st_.songSlot + 1);
    g.text(x + 7, 2, buf, GREEN);
  }
  cat3(buf, globalValue(kGTempo));
  g.text(64, 2, buf, TEXT);
  g.rect(84, 4, 3, 3, before(now_, beatUntil_) ? WHITE : LINE);
  // Focus chip x 92..165 (SONG: the cursor slot; PROJECT: the project name)
  if (view_ == View::Song && !helpVisible()) {
    const int len = songLength();
    char t[12];
    catInt(cat(t, "SLOT "), songCursor_ + 1);
    const int xx = g.chip(92, 1, t, WHITE);
    if (songCursor_ < len) {
      const SongSlot s = songSlot(songCursor_);
      catP2(cat(t, "P"), s.pattern + 1);
      g.text(xx + 3, 2, t, TEXT);
      catInt(cat(t, "x"), s.repeats);
      g.textR(165, 2, t, AMBER);
    } else {
      g.text(xx + 3, 2, "END", DIM);
    }
  } else if (view_ == View::Project) {
    char t[12];
    char* o = cat(t, curName_[0] ? curName_ : "NEW");
    if (dirty_) cat(o, "*");
    g.text(92, 2, t, TEXT);
  } else if (tilt_.engaged && !(view_ == View::Sound && instrumentPage() && sel_ == tilt_.track)) {
    // Tilt engaged on another view (the instrument page shows it on its sliders): the
    // track, TILT, and the pose as a dot in a small well (X right, Y away = up).
    const int t = tilt_.track & 7;
    const int w = int(std::strlen(kTrackNames[t])) * 6 + 3;
    g.rrect(92, 1, w, 9, tc(t));
    g.text(94, 2, kTrackNames[t], BG);
    const int tx = g.text(92 + w + 3, 2, "TILT", WHITE);
    tiltGlyph(g, tx + 2, 2, 0, tc(t, kMid));
    g.rrect(156, 1, 9, 9, RAISED);
    g.rect(159 + rnd(tilt_.in[0] * 3.0f), 4 - rnd(tilt_.in[1] * 3.0f), 3, 3, WHITE);
  } else {
    const ParamInfo* info = infoFor(focus_);
    const char* tag = focus_.global ? "FX" : kTrackNames[focus_.track & 7];
    const uint8_t col = focus_.global ? uint8_t(CYAN) : tc(focus_.track & 7);
    int v = valueOf(focus_);
    // While a step is held in SOUND/MIX/FX and it locks the focused param, show the lock.
    if (heldStep_ >= 0 && view_ != View::Groove && !focus_.global) {
      const int own = stepOwnValue(heldTrack_, stepAt(heldTrack_, heldStep_), focus_.index);
      if (own >= 0) v = own;
    }
    char val[kValueTextCap], name[6];
    formatValue(info, focus_, v, val);
    upperCopy(info ? info->name : "?", name, (!focus_.global && isMacroIndex(focus_.index)) ? 5 : int(sizeof(name)));
    const int w = int(std::strlen(tag)) * 6 + 3;
    g.rrect(92, 1, w, 9, col);
    g.text(94, 2, tag, BG);
    // The name gives way to the value: cut it to what fits left of the
    // right-aligned read-out (a 4-letter macro name next to "100%").
    const int nameX = 92 + w + 3;
    const int room = (165 - int(std::strlen(val)) * 6 - 1 - nameX) / 6;
    if (room < int(std::strlen(name))) name[room > 0 ? room : 0] = 0;
    g.text(nameX, 2, name, TEXT);
    g.textR(165, 2, val, WHITE);
    // Locked on some steps: those steps won't follow this value (UX 7).
    if (focusLocked_) g.text(166, 2, PG_ICON_LOCK, AMBER);
  }
  // Level meter (mono: one bar), x 171..204
  const float p = meter_ > 1.0f ? 1.0f : meter_;
  const int n = rnd(p * 34.0f);
  g.rect(171, 3, 34, 5, RAISED);
  g.rect(171, 3, n < 26 ? n : 26, 5, GREEN);
  if (n > 26) g.rect(197, 3, n - 26, 5, AMBER);
  if (clipUntil_) g.rect(203, 2, 2, 7, RED);
  // Ducker gain reduction, x 208..211
  g.rect(208, 1, 4, 9, RAISED);
  g.rect(208, 1, 4, rnd((1.0f - st_.duckGain) * 9.0f), CYAN);
  // Battery
  g.frame(217, 2, 16, 7, DIM);
  g.rect(233, 4, 2, 3, DIM);
  if (env_.batteryPct == kBatteryUnknown) return;     // unknown: an empty battery
  const int pct = env_.batteryPct > 100 ? 100 : env_.batteryPct;
  const uint8_t bc = pct > 30 ? GREEN : (pct > 10 ? AMBER : RED);
  const bool lit = !(pct < 10 && (now_ / 500) % 4 == 0);   // below 10 %: off 0.5 s every 2 s
  if (lit) g.rect(219, 4, rnd(12.0f * float(pct) / 100.0f), 3, bc);
}

// ============================================================== GROOVE ====

void Ui::drawGroove(Framebuffer& fb) {
  Gfx g(fb);
  const int page = shownPage_;
  // ---- overview
  for (int t = 0; t < kNumTracks; ++t) {
    const int y = 13 + t * 9;
    const bool m = muted(t);
    const uint8_t col = tc(t);
    if (t == sel_) g.rect(0, y, W, 9, tc(t, kTint));
    char k[2];
    if (m) {
      g.frame(1, y + 1, 7, 7, DIM);
      g.rect(3, y + 3, 3, 3, DIM);
    } else {
      g.rect(1, y, 7, 9, col);
      keyStr(k, kPadKeys[t]);
      g.text(2, y + 1, k, BG);
    }
    g.text(11, y + 1, kTrackNames[t], m ? uint8_t(DIM) : (t == sel_ ? uint8_t(WHITE) : tc(t, kMid)));
    const int len = trackLen(t);
    int lo = 127, hi = 0;
    if (isSynth(t)) {
      for (int i = 0; i < len; ++i) {
        const Step s = stepAt(t, i);
        if (!(s.flags() & kStepOn)) continue;
        if (s.note < lo) lo = s.note;
        if (s.note > hi) hi = s.note;
      }
      if (lo > hi) lo = hi = 60;
    }
    const int ph = playStep(t);
    for (int i = 0; i < 16; ++i) {
      const int idx = page * 16 + i;
      if (idx >= len) continue;
      const int x = ovX(i);
      const Step s = stepAt(t, idx);
      const bool here = ph == idx;
      if (!(s.flags() & kStepOn)) {
        g.rect(x, y + 1, 10, 7, here ? LINE : (i % 4 == 0 ? RAISED : PANEL));
        continue;
      }
      uint8_t c = col;
      if (m) c = LINE;
      else if (here) c = WHITE;
      else if (s.prob() < 100) c = tc(t, kMid);
      if (isSynth(t)) {
        g.rect(x, y + 1, 10, 7, m ? uint8_t(PANEL) : tc(t, kTint));
        const int lvl = hi == lo ? 0 : rnd(float(s.note - lo) * 4.0f / float(hi - lo));
        g.rect(x, y + 5 - lvl, (s.flags() & kStepSlide) ? 13 : 10, 3, c);
      } else {
        g.rect(x, y + 1, 10, 7, c);
        if ((s.flags() & kStepAccent) && !here && !m) g.rect(x, y + 1, 10, 1, WHITE);
      }
    }
    const bool flash = before(now_, flashUntil_[t]);
    g.rect(228, y + 2, 8, 5, flash ? uint8_t(WHITE) : (m ? uint8_t(PANEL) : tc(t, kDim)));
    if (m) g.text(229, y + 1, "M", DIM);
  }
  // ---- step pads (screens.step_pad)
  const int t = sel_;
  const bool synth = isSynth(t);
  const int len = trackLen(t);
  const int ph = playStep(t);
  const bool held = heldStep_ >= 0 && heldTrack_ == t;
  const bool sw = stepWriteActive();
  for (int i = 0; i < 16; ++i) {
    const int idx = page * 16 + i;
    const int x = padX(i), y = padY(i);
    char key[2];
    keyStr(key, kStepKeys[i]);
    if (idx >= len) {
      g.rframe(x, y, PAD_W, PAD_H, RAISED);
      continue;
    }
    const Step s = stepAt(t, idx);
    const bool play = ph == idx;
    if (!(s.flags() & kStepOn)) {
      uint8_t fill = i % 4 == 0 ? RAISED : PANEL;
      if (play) fill = LINE;
      g.rrect(x, y, PAD_W, PAD_H, fill);
      g.text(x + 11, y + 6, key, play ? DIM : LINE);
    } else {
      const uint8_t fill = play ? uint8_t(WHITE) : (s.prob() < 100 ? tc(t, kMid) : tc(t));
      g.rrect(x, y, PAD_W, PAD_H, fill);
      const uint8_t ink = BG;
      g.text(x + 3, y + 2, key, play ? uint8_t(DIM) : tc(t, kDim));
      if (synth) {
        char nn[6];
        noteName(s.note, nn);
        g.textR(x + 23, y + 9, nn, ink);
      } else {
        const int w = rnd(float(s.vel) * 21.0f / 127.0f);
        g.rect(x + 3, y + 12, w > 2 ? w : 2, 3, ink);
      }
      int mx = x + 23;
      if (t == kTrackLead && s.chord() && s.chord() < kChordNum) {
        const char* lab = chordShort(s.chord());
        g.textR(mx, y + 1, lab, play ? uint8_t(DIM) : tc(t, kDim));
        mx -= int(std::strlen(lab)) * 6 + 1;
      }
      if (s.hasExt() && mx - 4 >= x + 9) {   // a lock, or a note's own macro values
        g.text(mx - 4, y + 1, PG_ICON_LOCK, ink);
        mx -= 7;
      }
      const int r = stepRatchet(s);
      if (r > 1 && mx - r * 2 + 1 >= x + 9) {
        for (int k2 = 0; k2 < r; ++k2) g.rect(mx - k2 * 2, y + 3, 1, 5, ink);
        mx -= r * 2 + 2;
      }
      if (s.prob() < 100 && mx - 4 >= x + 9) g.text(mx - 4, y + 2, "%", ink);
      if (s.flags() & kStepAccent) g.rect(x + 1, y, PAD_W - 2, 2, play ? AMBER : WHITE);
      if ((s.flags() & kStepSlide) && synth) {
        const int gx = x + PAD_W;
        const int gw = (i % 8) < 7 ? padX(i + 1) - gx : 2;
        g.rect(gx - 2, y + 12, gw + 4, 3, WHITE);
      }
    }
    if (held && heldStep_ == idx) {
      g.rframe(x - 1, y - 1, PAD_W + 2, PAD_H + 2, WHITE);
      g.rframe(x, y, PAD_W, PAD_H, WHITE);
    }
    if (sw && stepCursor_ == idx) {
      g.rect(x + 2, y + PAD_H, PAD_W - 4, 1, AMBER);
      g.rframe(x - 1, y - 1, PAD_W + 2, PAD_H + 2, AMBER);
    }
  }
  // ---- footer
  const int pages = (len + 15) / 16;
  const int playPage = ph >= 0 ? ph / 16 : -1;
  for (int p = 0; p < 4; ++p) {
    const int px = 4 + p * 9;
    if (p >= pages) g.frame(px, 128, 7, 5, RAISED);
    else if (p == page) g.rect(px, 128, 7, 5, WHITE);
    else g.rect(px, 128, 7, 5, LINE);
    if (playPage == p && p < pages) g.rect(px + 2, 129, 3, 3, GREEN);
  }
  char buf[24];
  char* o = catInt(buf, page * 16 + 1);
  o = cat(o, "-");
  catInt(o, (page * 16 + 16) < len ? page * 16 + 16 : len);
  int xx = g.text(46, 127, buf, TEXT);
  catInt(cat(buf, "/"), len);
  g.text(xx, 127, buf, DIM);
  xx = 94;
  char oct[8];
  catInt(cat(oct, " OCT"), octave_[t]);
  if (erasing_) {
    g.text(xx, 127, "ERASE", RED);
  } else if (sw) {
    xx = g.text(xx, 127, "STEP WRITE", RED);
    if (synth) g.text(xx, 127, oct, TEXT);
  } else if (!synth) {
    g.text(xx, 127, "NOTES=VEL", tc(t, kMid));
  } else {
    char sc[12], tmp[kValueTextCap];
    const ParamInfo* ri = Engine::globalParamInfo(kGScaleRoot);
    const ParamInfo* si = Engine::globalParamInfo(kGScaleType);
    formatValue(ri, ValueRef{true, 0, kGScaleRoot}, globalValue(kGScaleRoot), tmp);
    char* q = cat(sc, tmp);
    q = cat(q, " ");
    formatValue(si, ValueRef{true, 0, kGScaleType}, globalValue(kGScaleType), tmp);
    cat(q, tmp);
    xx = g.text(xx, 127, sc, tc(t));
    if (t == kTrackLead) {
      cat(oct + std::strlen(oct), " ");
      xx = g.text(xx, 127, oct, TEXT);
      g.text(xx, 127, chordModeName(chordMode_), tc(t));
    } else {
      g.text(xx, 127, oct, TEXT);
    }
  }
  footerRight(g, "/HELP", DIM);
}

// screens.step_edit_panel, over y 12..85 while a step is held in GROOVE
void Ui::drawStepEdit(Framebuffer& fb) {
  Gfx g(fb);
  const int t = heldTrack_;
  const uint8_t col = tc(t);
  const bool drum = !pitched(t);              // a sampler instrument's note keys set the chop's pitch
  const Step s = stepAt(t, heldStep_);
  g.rect(0, 12, W, 74, PANEL);
  g.rect(0, 12, W, 1, col);
  char buf[24];
  catInt(cat(buf, "STEP "), heldStep_ + 1);
  int x = g.text(4, 16, buf, WHITE);
  g.chip(x + 4, 15, kTrackNames[t], col);
  int cells[8];
  int cur = -1;
  if (drum) {
    g.textR(235, 16, "Z-, VELOCITY", DIM);
    itoa10(s.vel, buf);
    g.text(4, 28, buf, WHITE, 2);
    for (int k = 0; k < 8; ++k) {
      cells[k] = kVelRow[k];
      if (cells[k] == s.vel && cur < 0) cur = k;
    }
  } else {
    g.textR(235, 16, PG_ICON_UP PG_ICON_DOWN " SEMI  SHIFT OCT", DIM);
    noteName(s.note, buf);
    g.text(4, 28, buf, WHITE, 2);
    for (int k = 0; k < 8; ++k) {
      cells[k] = isSynth(t) ? noteFor(t, k) : samplerNote(k);
      if (cells[k] == s.note && cur < 0) cur = k;
    }
  }
  for (int k = 0; k < 8; ++k) {
    const int cx = 50 + k * 23;
    const bool c = k == cur;
    g.rrect(cx, 27, 22, 17, c ? col : uint8_t(RAISED));
    char key[2];
    keyStr(key, kNoteKeys[k]);
    g.text(cx + 2, 28, key, c ? BG : DIM);
    if (drum) itoa10(cells[k], buf);
    else noteName(cells[k], buf);
    g.textR(cx + 19, 36, buf, c ? BG : TEXT);
  }
  static const char* const kFieldNames[kNumFields] = {"VEL", "GATE", "PROB", "RTCH", "ACC", "SLD", "CHRD", "LOCK"};
  for (int k = 0; k < 8; ++k) {
    const int fx = padX(k);
    char key[2];
    keyStr(key, kPadKeys[k]);
    if (!fieldAvailable(t, k)) {
      g.rframe(fx, 48, 27, 37, RAISED);
      g.text(fx + 3, 50, key, LINE);
      continue;
    }
    const char* fname = (k == kFGate && drum) ? "PTCH" : (k == kFLock && instrumentIdOf(t) ? "MOVE" : kFieldNames[k]);
    const bool selF = k == stepField_;
    g.rrect(fx, 48, 27, 37, selF ? RAISED : BG);
    if (selF) g.rframe(fx, 48, 27, 37, WHITE);
    g.text(fx + 3, 50, key, selF ? WHITE : DIM);
    g.textC(fx + 14, 59, fname, selF ? col : uint8_t(DIM));
    char txt[kValueTextCap];
    float fr = -1.0f;
    switch (k) {
      case kFVel: itoa10(s.vel, txt); fr = float(s.vel) / 127.0f; break;
      case kFGate:
        if (drum) {
          const int d = s.note - 60;
          if (d == 0) cat(txt, "0");
          else catInt(cat(txt, d > 0 ? "+" : ""), d);
        } else {
          static const char* const kQ[4] = {"", ".25", ".5", ".75"};
          cat(catInt(txt, s.gate / 4), kQ[s.gate % 4]);
          fr = s.gate / 16.0f > 1.0f ? 1.0f : float(s.gate) / 16.0f;
        }
        break;
      case kFProb: cat(catInt(txt, s.prob()), "%"); fr = float(s.prob()) / 100.0f; break;
      case kFRatch: catInt(cat(txt, "x"), stepRatchet(s)); break;
      case kFAcc: cat(txt, (s.flags() & kStepAccent) ? "ON" : "OFF"); break;
      case kFSlide: cat(txt, (s.flags() & kStepSlide) ? "ON" : "OFF"); break;
      case kFChord: cat(txt, s.chord() ? chordName(s.chord()) : "OFF"); break;
      case kFLock: {
        if (instrumentIdOf(t)) {
          // MOTION: four marks (lit = the note has its own value of that macro), the
          // focused macro's name and value under them (its pad again: the next macro)
          const int f = focusIndexFor(t);
          const int fm = isMacroIndex(f) ? f - kMacroIndex0 : 0;
          for (int m = 0; m < kTrackMacros; ++m)
            g.rect(fx + 10 + m * 4, 52, 3, 3, s.hasMacro(m) ? uint8_t(AMBER) : (m == fm ? uint8_t(TEXT) : uint8_t(LINE)));
          const ParamInfo* info = trackInfo(t, kMacroIndex0 + fm);
          char nm[6], v[kValueTextCap];
          if (info) upperCopy(info->name, nm, 5);
          else catInt(cat(nm, "M"), fm + 1);
          const bool has = s.hasMacro(fm);
          if (has && info) formatValue(info, ValueRef{false, uint8_t(t), uint8_t(kMacroIndex0 + fm)}, s.macroValue(fm), v);
          else if (has) itoa10(s.macroValue(fm), v);
          g.textC(fx + 14, 68, nm, has ? AMBER : DIM);
          g.textC(fx + 14, 77, has ? v : "-", has ? AMBER : DIM);
          continue;
        }
        if (s.lockParam()) {
          const ParamInfo* info = trackInfo(t, s.lockParam() - 1);
          char nm[6], v[kValueTextCap];
          if (info) upperCopy(info->name, nm, 5);
          else catInt(cat(nm, "P"), s.lockParam() - 1);
          if (info) formatValue(info, ValueRef{false, uint8_t(t), uint8_t(s.lockParam() - 1)}, s.lockValue(), v);
          else itoa10(s.lockValue(), v);
          g.textC(fx + 14, 68, nm, AMBER);
          g.textC(fx + 14, 77, v, AMBER);
          continue;
        }
        cat(txt, "OFF");
        break;
      }
      default: txt[0] = 0; break;
    }
    const bool on = std::strcmp(txt, "OFF") != 0;
    g.textC(fx + 14, 68, txt, on ? WHITE : LINE);
    if (k == kFGate && drum) {
      g.rect(fx + 3, 79, 21, 3, LINE);
      g.rect(fx + 13, 78, 1, 5, WHITE);
    } else if (fr >= 0.0f) {
      g.rect(fx + 3, 79, 21, 3, LINE);
      g.rect(fx + 3, 79, rnd(fr * 21.0f), 3, col);
    } else if ((k == kFAcc || k == kFSlide) && on) {
      g.rect(fx + 3, 79, 21, 3, WHITE);
    } else if (k == kFRatch) {
      for (int r = 0; r < 4; ++r) g.rect(fx + 3 + r * 6, 79, 4, 3, r < stepRatchet(s) ? col : uint8_t(LINE));
    }
  }
  // footer
  g.rect(0, 125, W, 10, BG);
  x = g.text(4, 127, "TAP STEP", WHITE);
  x = g.text(x, 127, " COPY HERE  ", DIM);
  x = g.text(x, 127, "DEL", WHITE);
  g.text(x, 127, instrumentIdOf(t) ? " CLEAR" : " NO LOCK", DIM);
  footerRight(g, "/ HELP", DIM);
}

// ============================================================== SOUND ====

namespace {
void miniStrip(Gfx& g, const Ui& ui, int y, int track, int page, int play, int held) {
  const int len = ui.trackLen(track);
  for (int i = 0; i < 16; ++i) {
    const int idx = page * 16 + i;
    if (idx >= len) continue;
    const int x = stripX(i);
    const Step s = ui.stepAt(track, idx);
    const bool on = s.flags() & kStepOn;
    uint8_t c;
    if (on) c = play == idx ? uint8_t(WHITE) : tc(track);
    else c = play == idx ? LINE : (i % 4 == 0 ? RAISED : PANEL);
    g.rect(x, y, 13, 8, c);
    if (on && s.hasExt()) g.rect(x, y, 13, 2, AMBER);
    if (held == idx) g.frame(x - 1, y - 1, 15, 10, WHITE);
  }
}
}  // namespace

// ---- the instrument page (UX.md 10.3) -----------------------------------------------
// Header: track chip, the instrument's name (L), its place in the list; no caption
// (a set's `about` line is an author's note and is not drawn). The picture (a live
// scope of the track's voice, kScopeY0..kScopeY1) starts right under the name; then
// the four macros as horizontal sliders in a 2 x 2 grid, the left hand's pairs
// (z x, c v) on the left, the right hand's (b n, m ,) on the right. CLASSIC (no
// instrument) and an unknown id keep the picture and invite a choice below it.
namespace {
void fitText(char* out, const char* in, int maxChars) {
  int n = 0;
  for (; in[n] && n < maxChars; ++n) out[n] = (in[n] >= 'a' && in[n] <= 'z') ? char(in[n] - 32) : in[n];
  if (in[n] && n >= 2) { out[n - 2] = '.'; out[n - 1] = '.'; }
  out[n] = 0;
}

constexpr int kCellW = kMacroCellW;            // one macro's cell
constexpr const int* kCellX = kMacroCellX;
constexpr int kLabelY[2] = {75, 104};          // the top row's labels sit above its slider,
constexpr int kSliderY[2] = {84, 94};          // the bottom row's below: the two sliders meet in the middle
constexpr int kMarkW = 5;                      // the motion marker (PG_ICON_LOCK, the diamond a note value carries)
constexpr int kMarkGap = 2;                    // between it and the name (chip)
}  // namespace

// The name sits centred in the room the end labels leave, a tilt mark right of it.
// Where the centred name leaves the tilt mark no room, the name moves left by half
// the mark; the mark is left out only when even that does not fit.
// With motion the marker goes left of the name (2 blank columns between them):
// where the centred name leaves it no room, the marker, the name and the tilt mark
// are centred together between the end labels; then the same with the marker 1 px
// closer to the name and to the low label; then without the tilt mark (the marker
// wins: it says why the slider moves by itself); the marker is left out only when
// even the marker and the name alone do not fit.
MacroLabels macroLabels(const macro::Macro& mc, int cx0, bool motion, uint8_t tiltMarks) {
  MacroLabels L;
  const int w = kCellW;
  fitText(L.name, mc.name, 6);
  L.lo[0] = L.hi[0] = 0;
  L.loEnd = cx0;
  L.hiStart = cx0 + w;
  if (mc.lo[0]) { fitText(L.lo, mc.lo, 8); L.loEnd = cx0 + int(std::strlen(L.lo)) * 6; }
  if (mc.hi[0]) { fitText(L.hi, mc.hi, 8); L.hiStart = cx0 + w - tw(L.hi); }
  const int ncx0 = (mc.lo[0] || mc.hi[0]) ? (L.loEnd + L.hiStart) / 2 : cx0 + w / 2;
  L.tiltAx = (tiltMarks & 1) ? 0 : 1;
  const int nw = int(std::strlen(L.name)) * 6 + 3;   // the chip's width when lit
  const int h = nw / 2;                                // the chip spans ncx - h .. ncx + h
  L.nameW = nw;
  const int tW = tiltGlyphW(L.tiltAx);
  const int rb = L.hiStart - 2;                        // the last column a mark may use
  L.markX = L.tiltX = -1;
  if (motion) {
    // {blank columns between the marker and the chip, the marker's first column
    // after the low label's end, with the tilt mark}
    static constexpr int kTry[4][3] = {{kMarkGap, 1, 1}, {kMarkGap - 1, 0, 1}, {kMarkGap, 1, 0}, {kMarkGap - 1, 0, 0}};
    for (const auto& k : kTry) {
      if (k[2] && !tiltMarks) continue;               // (the tries without it come next)
      const bool tilt = k[2] != 0;
      const int left = kMarkW + k[0], right = tilt ? 1 + tW : 0, lb = L.loEnd + k[1];
      const int G = left + nw + right;                 // the group's columns
      if (G > rb - lb + 1) continue;
      int ncx = ncx0;
      if (ncx - h - left < lb || ncx + h + right > rb) ncx = lb + (rb - lb + 1 - G) / 2 + left + h;
      L.nameX = ncx;
      L.markX = ncx - h - left;
      if (tilt) L.tiltX = ncx + h + 2;
      return L;
    }
  }
  int ncx = ncx0;
  if (tiltMarks && ncx + h + 2 + tW > L.hiStart - 1) ncx -= (tW + 2 + 1) / 2;
  L.nameX = ncx;
  const int tx = ncx + h + 2;
  L.tiltX = tiltMarks && tx + tW <= L.hiStart - 1 ? tx : -1;
  return L;
}

bool Ui::scopeShown() const {
  return view_ == View::Sound && instrumentPage() && !helpVisible();
}

// Something drawn over the picture's band: a partial redraw would erase it.
bool Ui::scopeCovered() const {
  return overlay_ != Overlay::None || dialog_ != Dialog::None || popUntil_ || toastUntil_ || listUntil_ || banner_;
}

// Enum jump ticks of the shown instrument's macros: each macro's are recomputed
// (one macro per frame: 201 evaluations) when the other macros have moved.
void Ui::refreshJumps() {
  const macro::MacroSet* s = instrumentOf(sel_);
  if (!s) return;
  if (s->id != tickId_ || sel_ != tickTrack_) {
    tickId_ = s->id;
    tickTrack_ = sel_;
    tickValid_ = 0;
  }
  uint8_t pos[kTrackMacros];
  for (int m = 0; m < kTrackMacros; ++m) pos[m] = uint8_t(macroValue(sel_, m));
  for (int m = 0; m < s->numMacros; ++m) {
    bool same = (tickValid_ >> m) & 1;
    for (int o = 0; o < kTrackMacros && same; ++o)
      if (o != m && tickKey_[m][o] != pos[o]) same = false;
    if (same) continue;
    macro::ParamTable pt;
    std::memset(&pt, 0, sizeof(pt));
    uint8_t under[kParamsPerTrack];
    for (int i = 0; i < kParamsPerTrack; ++i) pt.info[s->track][i] = eng_->trackParamInfo(s->track, i);
    const ParamInfo* kindScratch[kParamsPerTrack];
    const macro::TrackParams kp = macro::paramsFor(*s, pt, kindScratch);   // the set's voice kind's list
    for (int i = 0; i < kParamsPerTrack; ++i) under[i] = kp[i] ? kp[i]->def : 0;
    float fpos[kTrackMacros], at[16];
    uint8_t par[16];
    for (int o = 0; o < kTrackMacros; ++o) fpos[o] = macro::posFromByte(pos[o]);
    const int n = macro::enumJumps(*s, pt, m, fpos, under, at, par, 16);
    int k = 0;
    for (int j = 0; j < n && k < kMaxTicks; ++j) {
      const uint8_t b = macro::posToByte(at[j]);
      if (k && b - tickPos_[m][k - 1] < 4) continue;   // two parameters jumping together: one tick
      tickPos_[m][k++] = b;
    }
    tickN_[m] = uint8_t(k);
    for (int o = 0; o < kTrackMacros; ++o) tickKey_[m][o] = pos[o];
    tickValid_ |= uint8_t(1u << m);
    ++serial_;
    return;   // one per frame
  }
}

// The picture: the box, a centre line, the trace filled toward the centre (track
// dim) with its edge in the track colour. Silence: the calm centre line.
void Ui::drawScope(Framebuffer& fb) {
  if (samplerOn(sel_)) { drawWave(fb); return; }   // a sampler instrument: its sample instead
  Gfx g(fb);
  const int t = sel_;
  const int x0 = kScopeX0, y0 = kScopeY0, h = kScopeY1 - kScopeY0 + 1, w = Scope::kCols;
  const int cy = y0 + h / 2;
  const float half = float(h / 2 - 2);
  g.rect(x0 - 4, y0, w + 8, h, BG);
  g.rrect(x0, y0, w, h, PANEL);
  const bool muted = this->muted(t);
  const uint8_t edge = muted ? tc(t, kMid) : tc(t);
  const uint8_t body = muted ? tc(t, kTint) : tc(t, kDim);
  const float lv = scope_.track() == t ? scope_.level() : 0.0f;
  const int8_t* tr = scope_.trace();
  const int filled = scope_.track() == t ? scope_.filled() : 0;
  for (int c = 0; c < w; c += 4) g.pixel(x0 + c, cy, RAISED);
  if (lv <= 0.0f || filled <= 0) {
    g.rect(x0 + 2, cy, w - 4, 1, tc(t, kMid));
    return;
  }
  int prev = cy - int(float(tr[0]) * (1.0f / 127.0f) * lv * half);
  for (int c = 0; c < w; ++c) {
    const int yy = c < filled ? cy - int(float(tr[c]) * (1.0f / 127.0f) * lv * half) : cy;
    if (yy < cy) g.rect(x0 + c, yy, 1, cy - yy, body);
    else if (yy > cy) g.rect(x0 + c, cy + 1, 1, yy - cy, body);
    const int a = prev < yy ? prev : yy, b = prev < yy ? yy : prev;
    g.rect(x0 + c, a, 1, b - a + 1, edge);
    prev = yy;
  }
}

// The sampler's picture (UX.md 10.3 "Sampler instruments"), in the scope's box: the
// slot's waveform overview (the header's 232 min / max columns, one per pixel column),
// the region the next note will play on a raised band and in the track colour, the
// rest dim; the slice boundaries as dotted lines (the chosen slice's in the track's
// mid shade); a white playhead while a chop of this slot sounds; the slot number and
// the sample's name (or BUILT-IN) in the top left corner, its length on the right.
static_assert(sampler::kOverviewCols == Scope::kCols, "one overview column per pixel column of the box");
void Ui::drawWave(Framebuffer& fb) {
  if (micState_ != kMicIdle) { drawMic(fb); return; }   // erasing or recording (ui_mic.cpp)
  Gfx g(fb);
  const int t = sel_;
  const int x0 = kScopeX0, y0 = kScopeY0, h = kScopeY1 - kScopeY0 + 1, w = Scope::kCols;
  const int slot = sampleSlotOf(t);
  sampler::SampleRef r;
  sampler::resolve(samplePool(), slot, &r);
  uint8_t p[kSoundParams];
  for (int i = 0; i < kSoundParams; ++i) p[i] = uint8_t(paramValue(t, i));
  int32_t a = 0, b = 0;
  const bool lead = t == kTrackLead;   // the melodic sampler: its window, no slices
  if (lead) sampler::MelodicSampler::region(p, r.length, r.rate, &a, &b);
  else sampler::SamplerVoice::region(p, r.length, r.rate, &a, &b);
  const int64_t L = r.length ? int64_t(r.length) : 1;
  const int ca = int(int64_t(a) * w / L), cb = int((int64_t(b) * w + L - 1) / L);   // the region's columns
  const bool muted = this->muted(t);
  g.rect(x0 - 4, y0, w + 8, h, BG);
  g.rrect(x0, y0, w, h, PANEL);
  g.rect(x0 + ca, y0 + 1, (cb > ca ? cb - ca : 1), h - 2, RAISED);
  // slices
  const int n = lead ? 1 : (p[sampler::kSlices] < 1 ? 1 : (p[sampler::kSlices] > 16 ? 16 : p[sampler::kSlices]));
  const int pick = p[sampler::kSlice] % n;
  for (int k = 1; k < n; ++k) {
    const int sx = x0 + k * w / n;
    const uint8_t c = (k == pick || k == pick + 1) ? tc(t, kMid) : uint8_t(LINE);
    for (int yy = y0 + 2; yy < y0 + h - 2; yy += 2) g.pixel(sx, yy, c);
  }
  // the waveform
  const int cy = y0 + h / 2;
  const int half = (h / 2 - 2) * 7 / 8;
  const uint8_t in = muted ? tc(t, kMid) : tc(t), out = muted ? uint8_t(LINE) : tc(t, kDim);
  for (int c = 0; c < w; ++c) {
    int lo = r.overview ? r.overview[c][0] : 0, hi = r.overview ? r.overview[c][1] : 0;
    int yt = cy - (hi * half) / 128, yb = cy - (lo * half) / 128;
    if (yb < yt) { const int s = yt; yt = yb; yb = s; }
    g.rect(x0 + c, yt, 1, yb - yt + 1, c >= ca && c < cb ? in : out);
  }
  // the playhead: a chop of this slot sounding on this track (the lead: each note's)
  if (lead && melodic_) {
    for (int k = 0; k < sampler::mel::kMaxNotes; ++k) {
      const int32_t pk = melodic_->playhead(k);
      const int ph = sampler::MelodicSampler::playheadPos(pk);
      if (sampler::MelodicSampler::playheadSlot(pk) == slot && ph >= 0 && ph < int32_t(L))
        g.rect(x0 + int(int64_t(ph) * w / L), y0 + 1, 1, h - 2, WHITE);
    }
  } else if (!lead) {
    int32_t ph = -1;
    if (sampler_ && sampler_->playingSlot() == slot) ph = sampler_->playhead();
    if (ph >= 0 && ph < int32_t(L)) g.rect(x0 + int(int64_t(ph) * w / L), y0 + 1, 1, h - 2, WHITE);
  }
  wavePlayhead_ = wavePlayheads();
  // the slot and the sample's name; its length
  char buf[24];
  catInt(buf, slot + 1);
  int lx = g.chip(x0 + 2, y0 + 2, buf, tc(t), BG);
  const char* nm = r.builtin ? "BUILT-IN" : r.name;
  g.rect(lx + 1, y0 + 1, tw(nm) + 4, 10, PANEL);
  g.text(lx + 3, y0 + 3, nm, r.builtin ? uint8_t(DIM) : uint8_t(WHITE));
  const int tenths = int((int64_t(r.length) * 10 + int64_t(r.rate) / 2) / int64_t(r.rate ? r.rate : 1));
  cat(catInt(cat(catInt(buf, tenths / 10), "."), tenths % 10), "S");
  g.rect(x0 + w - 4 - tw(buf), y0 + 1, tw(buf) + 3, 10, PANEL);
  g.textR(x0 + w - 3, y0 + 3, buf, DIM);
}

void Ui::drawInstrument(Framebuffer& fb) {
  Gfx g(fb);
  const int t = sel_;
  const uint8_t col = tc(t);
  const macro::MacroSet* s = instrumentOf(t);
  const bool held = heldStep_ >= 0 && heldTrack_ == t;
  const Step hs = held ? stepAt(t, heldStep_) : kDefaultStep;
  char buf[48];
  int x = g.chip(4, 14, kTrackNames[t], col);
  const int n = macro::instrumentCount(t);
  if (!s) {
    // CLASSIC (or an id this firmware lacks): the same picture, and two ways in
    const uint16_t id = instrumentIdOf(t);
    g.text(x + 6, 13, id ? "UNKNOWN" : "CLASSIC", id ? AMBER : TEXT, 2);
    g.textR(235, 15, id ? "NOT IN FIRMWARE" : "OWN SAVED SOUND", id ? AMBER : DIM);
    drawScope(fb);
    g.text(4, 76, "PICK AN INSTRUMENT", WHITE);
    catInt(buf, n);
    cat(cat(buf + std::strlen(buf), " FOR "), kTrackNames[t]);
    g.textR(235, 76, buf, DIM);
    const macro::MacroSet* first = macro::instrumentAt(t, 0);
    const macro::MacroSet* last = macro::instrumentAt(t, n - 1);
    for (int c = 0; c < 2; ++c) {   // . or 9: the first one; ;: the last one
      const int cx0 = kCellX[c], cy0 = 86;
      g.rrect(cx0, cy0, kCellW, 18, PANEL);
      g.rframe(cx0, cy0, kCellW, 18, tc(t, kMid));
      int kx = g.chip(cx0 + 4, cy0 + 5, c ? ";" : ".", WHITE);
      if (!c) kx = g.chip(kx + 2, cy0 + 5, "9", WHITE);
      const macro::MacroSet* pick = c ? last : first;
      g.text(kx + 5, cy0 + 2, pick ? pick->name : "-", col, 2);
    }
    g.text(4, 106, "CTRL+Z RIGHT AFTER = BACK TO THIS", DIM);
    miniStrip(g, *this, 114, t, shownPage_, playStep(t), held ? heldStep_ : -1);
    g.text(4, 127, "THEN Z-, MOVE ITS MACROS", DIM);
    footerRight(g, "/ HELP", DIM);
    if (listUntil_) drawInstrumentList(fb);
    return;
  }
  // header
  const int idx = macro::instrumentIndex(t, s->id);
  g.text(x + 6, 13, s->name, WHITE, 2);
  if (held) {
    catInt(cat(buf, "STEP "), heldStep_ + 1);
    const int sx = 235 - tw(buf) - 3 - tw(PG_ICON_LOCK "LOCK") - 4;
    const int ex = g.text(sx, 15, buf, WHITE);
    g.chip(ex + 3, 14, PG_ICON_LOCK "LOCK", AMBER);
  } else {
    catInt(cat(catInt(buf, idx + 1), "/"), n);
    g.textR(235, 15, buf, DIM);
  }
  drawScope(fb);
  // the four macros
  const uint8_t motion = motionMask(t);
  for (int m = 0; m < kTrackMacros; ++m) {
    const int cx0 = kCellX[m / 2], cy0 = kLabelY[m % 2], w = kCellW;
    const int kLo = kKNote0 + 2 * m, kHi = kLo + 1;
    char k1[2] = {kNoteKeys[2 * m], 0}, k2[2] = {kNoteKeys[2 * m + 1], 0};
    const int sy = kSliderY[m % 2];                  // slider row: key cap, groove, key cap
    const int tx0 = cx0 + 13, tx1 = cx0 + w - 14;    // the track's ends (the handle's centre travels between them)
    const int tl = tx1 - tx0;
    if (m >= s->numMacros) {   // a macro the set does not have: its keys, an empty groove
      g.rframe(cx0, sy, 9, 9, RAISED);
      g.textC(cx0 + 4, sy + 1, k1, LINE);
      g.rframe(cx0 + w - 9, sy, 9, 9, RAISED);
      g.textC(cx0 + w - 5, sy + 1, k2, LINE);
      g.rect(tx0, sy + 4, tl + 1, 1, RAISED);
      continue;
    }
    const macro::Macro& mc = s->macros[m];
    const bool f = !focus_.global && focus_.track == t && focus_.index == kMacroIndex0 + m;
    const bool locked = held && hs.hasMacro(m);
    const bool moving = macroMoving(m);
    const bool live = macroLive(t, m);              // motion recording: the hand is written into the notes
    const bool lit = f || moving || locked || live;
    const int v = macroValue(t, m);                  // the base
    const int heard = shownMacro(t, m);              // the motion moves it while playing
    const int shown = locked ? hs.macroValue(m) : heard;
    // the label row: low end, the macro's name, high end; the motion marker and the tilt mark
    const MacroLabels L = macroLabels(mc, cx0, (motion >> m) & 1u, tiltMarks(t, m));
    const uint8_t labInk = lit ? uint8_t(TEXT) : uint8_t(DIM);
    if (L.lo[0]) g.text(cx0, cy0, L.lo, labInk);
    if (L.hi[0]) g.text(L.hiStart, cy0, L.hi, labInk);
    if (lit) {
      g.rrect(L.nameX - L.nameW / 2, cy0 - 1, L.nameW, 9, live ? uint8_t(RED) : (locked ? uint8_t(AMBER) : (moving ? uint8_t(WHITE) : col)));
      g.textC(L.nameX, cy0, L.name, BG);
    } else {
      g.textC(L.nameX, cy0, L.name, TEXT);
    }
    // motion: notes of this pattern carry their own values of this macro (the
    // diamond a note with a value carries on its step)
    if (L.markX >= 0) g.text(L.markX, cy0, PG_ICON_LOCK, AMBER);
    // tilt: which axis moves this macro (lit while G0 tilts it)
    if (L.tiltX >= 0) {
      const bool on = tilt_.engaged && tilt_.track == t;
      tiltGlyph(g, L.tiltX, cy0, L.tiltAx, on ? uint8_t(WHITE) : uint8_t(DIM));
    }
    // the keys as caps at the ends: the left one moves toward the left label
    const bool d1 = isHeld(kLo) && rec_[kLo].act == kActMacro, d2 = isHeld(kHi) && rec_[kHi].act == kActMacro;
    g.rrect(cx0, sy, 9, 9, d1 ? uint8_t(WHITE) : uint8_t(RAISED));
    g.textC(cx0 + 4, sy + 1, k1, d1 ? uint8_t(BG) : (lit ? uint8_t(WHITE) : uint8_t(TEXT)));
    g.rrect(cx0 + w - 9, sy, 9, 9, d2 ? uint8_t(WHITE) : uint8_t(RAISED));
    g.textC(cx0 + w - 5, sy + 1, k2, d2 ? uint8_t(BG) : (lit ? uint8_t(WHITE) : uint8_t(TEXT)));
    // the groove, filled up to the handle; enum jumps as ticks above it, the default as a notch below
    const int hx = tx0 + (shown * tl + 127) / 255;
    g.rect(tx0, sy + 3, tl + 1, 3, RAISED);
    const uint8_t fill = locked ? uint8_t(AMBER_D) : (lit ? tc(t, kMid) : tc(t, kDim));
    g.rect(tx0, sy + 3, hx - tx0 + 1, 3, fill);
    if (tickId_ == s->id && tickTrack_ == t)
      for (int k = 0; k < tickN_[m]; ++k) g.rect(tx0 + (tickPos_[m][k] * tl + 127) / 255, sy, 1, 2, TEXT);
    const int dx = tx0 + (macro::startByte(mc) * tl + 127) / 255;
    g.pixel(dx, sy + 7, DIM);
    g.rect(dx - 1, sy + 8, 3, 1, DIM);
    if (locked || shown != v) g.rect(tx0 + (v * tl + 127) / 255, sy, 1, 9, WHITE);   // the base, where it differs
    // the handle: a cap with a dark outline so it reads over the fill
    const uint8_t hc = live ? uint8_t(RED) : (locked ? uint8_t(AMBER) : (moving ? uint8_t(WHITE) : (f ? col : tc(t, kMid))));
    g.rect(hx - 3, sy, 7, 9, BG);
    g.rrect(hx - 2, sy, 5, 9, hc);
  }
  miniStrip(g, *this, 114, t, shownPage_, playStep(t), held ? heldStep_ : -1);
  if (motionFooter()) {   // the focused macro has motion: how to change it (UX.md 10.3 "Motion")
    x = g.text(4, 127, "STEP", WHITE);
    x = g.text(x, 127, "+KEY ", DIM);
    x = g.text(x, 127, PG_ICON_LOCK, AMBER);
    x = g.text(x + 12, 127, "REC", WHITE);
    x = g.text(x, 127, "+MOVE", DIM);
    x = g.text(x + 12, 127, "DEL", WHITE);
    g.text(x, 127, "+KEY", DIM);
  } else if (!held && samplerOn(t) && isHeld(kKShift)) {   // a sampler, SHIFT held: its layer
    x = g.text(4, 127, "SFT+ ", DIM);
    x = g.text(x, 127, ";", WHITE);
    x = g.text(x, 127, "/", DIM);
    x = g.text(x, 127, ".", WHITE);
    x = g.text(x, 127, " SLOT  ", DIM);
    x = g.text(x, 127, "L", WHITE);
    g.text(x, 127, " RESAMPLE", DIM);
  } else if (!held && samplerOn(t)) {   // a sampler: SHIFT+; / . steps the sample slot, L records
    x = g.text(4, 127, ";", WHITE);
    x = g.text(x, 127, "/", DIM);
    x = g.text(x, 127, ".", WHITE);
    x = g.text(x, 127, " INST  SFT+", DIM);
    x = g.text(x, 127, ";", WHITE);
    x = g.text(x, 127, "/", DIM);
    x = g.text(x, 127, ".", WHITE);
    x = g.text(x, 127, " SLOT  ", DIM);
    x = g.text(x, 127, "L", WHITE);
    g.text(x, 127, " MIC", DIM);
  } else if (!held) {
    x = g.text(4, 127, ";", WHITE);
    x = g.text(x, 127, "/", DIM);
    x = g.text(x, 127, ".", WHITE);
    x = g.text(x, 127, " INSTRUMENT  HOLD STEP+KEY ", DIM);
    g.text(x, 127, PG_ICON_LOCK, AMBER);
  } else {
    g.text(4, 127, "MOVE A MACRO = NOTE VALUE  DEL", DIM);
  }
  footerRight(g, "/ HELP", DIM);
  if (listUntil_) drawInstrumentList(fb);
}

// The track's instruments after ; . 9 (for 1.5 s): a column at the picture's right
// edge, the current one in the middle as a chip, its neighbours (the list wraps)
// above and below; the rest of the picture stays in view.
void Ui::drawInstrumentList(Framebuffer& fb) {
  Gfx g(fb);
  const int t = sel_;
  const int n = macro::instrumentCount(t);
  const int cur = macro::instrumentIndex(t, instrumentIdOf(t));
  if (n <= 0 || cur < 0) return;
  const int w = 92, x = kScopeX0 + Scope::kCols - w, y = kScopeY0, h = kScopeY1 - kScopeY0 + 1;
  g.rect(x - 2, y, w + 2, h, BG);
  g.rrect(x, y, w, h, RAISED);
  g.rframe(x, y, w, h, tc(t));
  for (int r = -1; r <= 1; ++r) {
    if (n < 2 && r) continue;
    if (n < 3 && r < 0) continue;
    const macro::MacroSet* s = macro::instrumentAt(t, ((cur + r) % n + n) % n);
    if (!s) continue;
    const int yy = y + h / 2 - 5 + r * 11;
    if (!r) {
      g.rrect(x + 3, yy, w - 6, 10, tc(t));
      g.text(x + 7, yy + 2, s->name, BG);
      char pos[8];
      catInt(cat(catInt(pos, cur + 1), "/"), n);
      g.textR(x + w - 7, yy + 2, pos, tc(t, kDim));
    } else {
      g.text(x + 7, yy + 2, s->name, TEXT);
      g.text(x + w - 11, yy + 2, r < 0 ? PG_ICON_UP : PG_ICON_DOWN, DIM);
    }
  }
}

void Ui::drawSound(Framebuffer& fb) {
  if (instrumentPage()) { drawInstrument(fb); return; }
  Gfx g(fb);
  const int t = sel_;
  const uint8_t col = tc(t);
  int n = 0;
  while (n < kSoundParams && eng_->trackParamInfo(t, n)) ++n;
  const int pages = n ? (n + 7) / 8 : 1;
  const int page = soundPage_[t] < pages ? soundPage_[t] : 0;
  const bool held = heldStep_ >= 0 && heldTrack_ == t;
  const Step hs = held ? stepAt(t, heldStep_) : kDefaultStep;
  // the focused value (big read-out), with the held step's lock if it locks it
  const ParamInfo* finfo = infoFor(focus_);
  int fv = valueOf(focus_);
  if (held && !focus_.global && stepOwnValue(t, hs, focus_.index) >= 0) fv = stepOwnValue(t, hs, focus_.index);
  char fval[kValueTextCap];
  formatValue(finfo, focus_, fv, fval);
  int x = g.chip(4, 14, kTrackNames[t], col);
  char buf[16];
  if (!held) {
    x = g.text(x + 5, 15, "SOUND", TEXT);
    for (int i = 0; i < pages; ++i) {
      itoa10(i + 1, buf);
      if (i == page) {
        x = g.chip(x + 5, 14, buf, WHITE);
      } else {
        g.text(x + 7, 15, buf, DIM);
        x += 14;
      }
    }
    bigValue(g, finfo ? finfo->longName : "", fval, WHITE);
  } else {
    catInt(cat(buf, "STEP "), heldStep_ + 1);
    x = g.text(x + 5, 15, buf, WHITE);
    g.chip(x + 3, 14, PG_ICON_LOCK "LOCK", AMBER);
    bigValue(g, finfo ? finfo->longName : "", fval, AMBER);
  }
  Col cols[8];
  int focusCol = -1;
  for (int c = 0; c < 8; ++c) {
    const int idx = page * 8 + c;
    const ParamInfo* info = idx < kSoundParams ? eng_->trackParamInfo(t, idx) : nullptr;
    const int lock = held ? stepOwnValue(t, hs, idx) : -1;
    makeCol(cols[c], info, ValueRef{false, uint8_t(t), uint8_t(idx)}, paramValue(t, idx), lock);
    if (!focus_.global && focus_.track == t && focus_.index == idx) focusCol = c;
  }
  paramCols(g, cols, focusCol, col, tc(t, kMid), 32, 52);
  miniStrip(g, *this, 114, t, shownPage_, playStep(t), held ? heldStep_ : -1);
  if (!held) {
    x = g.text(4, 127, "HOLD STEP + -/= ", DIM);
    g.text(x, 127, PG_ICON_LOCK "LOCK", AMBER);
  } else {
    g.text(4, 127, "-/= LOCK VALUE   DEL REMOVE LOCK", DIM);
  }
  footerRight(g, "/ HELP", DIM);
}

// ============================================================== MIX ====

void Ui::drawMix(Framebuffer& fb) {
  Gfx g(fb);
  const int row = mixRow_;
  const int param = kMixRowParam[row];
  const bool pan = param == kMixPan;
  int x = 4;
  char k[2];
  for (int i = 0; i < kNumMixRows; ++i) {
    const char* tname = kMixRowNames[i];
    const int w = int(std::strlen(tname)) * 6 + 12;
    keyStr(k, kNoteKeys[i]);
    if (i == row) {
      g.rrect(x, 14, w, 9, WHITE);
      g.text(x + 2, 15, k, DIM);
      g.text(x + 10, 15, tname, BG);
    } else {
      g.text(x + 2, 15, k, LINE);
      g.text(x + 10, 15, tname, i == 4 ? LINE : DIM);   // PAN dimmed: mono output
    }
    x += w + 5;
  }
  const bool held = heldStep_ >= 0;
  const Step hs = held ? stepAt(heldTrack_, heldStep_) : kDefaultStep;
  for (int t = 0; t < kNumTracks; ++t) {
    const int x0 = t * 30, cx = x0 + 15;
    const bool m = muted(t);
    const uint8_t col = m ? uint8_t(LINE) : tc(t);
    const bool s = t == sel_;
    if (s) {
      g.rrect(x0 + 1, 26, 28, 98, tc(t, kTint));
      g.rframe(x0 + 1, 26, 28, 98, tc(t, kMid));
    }
    g.rect(cx - 5, 29, 10, 2, before(now_, flashUntil_[t]) ? WHITE : RAISED);
    g.rect(cx - 5, 34, 10, 56, s ? BG : PANEL);
    int v = paramValue(t, param);
    bool locked = false;
    const int own = (held && t == heldTrack_) ? stepOwnValue(t, hs, param) : -1;
    if (own >= 0) { v = own; locked = true; }
    if (pan) {
      const int midy = 34 + 28;
      const int d = rnd(float(v - 64) / 64.0f * 28.0f);
      if (d >= 0) g.rect(cx - 5, midy - d, 10, d + 1, locked ? uint8_t(AMBER) : col);
      else g.rect(cx - 5, midy, 10, -d, locked ? uint8_t(AMBER) : col);
      g.rect(cx - 7, midy, 14, 1, DIM);
    } else {
      const int hpx = rnd(float(v) / 127.0f * 56.0f);
      uint8_t fill = s ? col : (m ? uint8_t(LINE) : tc(t, kMid));
      if (locked) fill = AMBER;
      g.rect(cx - 5, 90 - hpx, 10, hpx, fill);
      g.rect(cx - 7, 90 - hpx, 14, 2, s ? WHITE : TEXT);
    }
    char txt[kValueTextCap];
    formatValue(eng_->trackParamInfo(t, param), ValueRef{false, uint8_t(t), uint8_t(param)}, v, txt);
    g.textC(cx, 93, txt, locked ? uint8_t(AMBER) : uint8_t(s ? WHITE : TEXT));
    if (m) {
      g.rframe(x0 + 2, 103, 26, 9, DIM);
      g.textC(cx, 104, "MUTE", DIM);
    } else {
      const int w = int(std::strlen(kTrackNames[t])) * 6 + 3;
      g.rrect(cx - w / 2, 103, w, 9, col);
      g.textC(cx, 104, kTrackNames[t], BG);
    }
    keyStr(k, kPadKeys[t]);
    g.textC(cx, 115, k, s ? WHITE : DIM);
  }
  if (pan) {
    g.text(4, 127, "PAN: HEADPHONE OUT IS MONO", AMBER);
  } else if (held) {
    g.text(4, 127, "-/= LOCK VALUE   DEL REMOVE LOCK", DIM);
  } else {
    x = g.text(4, 127, "A-K", TEXT);
    x = g.text(x, 127, " STRIP  ", DIM);
    x = g.text(x, 127, "-/=", TEXT);
    x = g.text(x, 127, " MOVE  ", DIM);
    x = g.text(x, 127, "Z-B", TEXT);
    g.text(x, 127, " ROW", DIM);
  }
}

// ============================================================== FX ====

namespace {
int fxTabs(Gfx& g, int active) {
  int x = 4;
  for (int i = 0; i < kNumFxTabs; ++i) {
    if (i == active) {
      x = g.chip(x, 14, kFxTabNames[i], CYAN) + 4;
    } else {
      g.text(x + 2, 15, kFxTabNames[i], DIM);
      x += int(std::strlen(kFxTabNames[i])) * 6 + 3 + 4;
    }
  }
  return x;
}
}  // namespace

void Ui::drawFx(Framebuffer& fb) {
  if (fxTab_ == kFxDuck) { drawFxDuck(fb); return; }
  Gfx g(fb);
  fxTabs(g, fxTab_);
  Col cols[8];
  int focusCol = -1;
  for (int c = 0; c < 8; ++c) {
    const int gid = kFxPage[fxTab_][c];
    const ParamInfo* info = gid == 0xFF ? nullptr : Engine::globalParamInfo(gid);
    makeCol(cols[c], info, ValueRef{true, 0, uint8_t(gid)}, info ? globalValue(gid) : 0, -1);
    if (focus_.global && focus_.index == gid) focusCol = c;
  }
  const ParamInfo* finfo = infoFor(focus_);
  char fval[kValueTextCap];
  formatValue(finfo, focus_, valueOf(focus_), fval);
  bigValue(g, finfo ? finfo->longName : "", fval, WHITE);
  paramCols(g, cols, focusCol, CYAN, CYAN_D, 32, 52);
  miniStrip(g, *this, 114, kTrackKick, shownPage_, playStep(kTrackKick), -1);
  if (fxTab_ == kFxMaster) {
    const int x = g.text(4, 127, "[ ]", CYAN);
    g.text(x, 127, " DJ FILTER FROM ANY VIEW", DIM);
  } else {
    g.text(4, 127, "MIX VIEW SETS EACH TRACK'S SENDS", DIM);
  }
}

void Ui::drawFxDuck(Framebuffer& fb) {
  Gfx g(fb);
  fxTabs(g, kFxDuck);
  char buf[24];
  cat(catInt(cat(buf, "GAIN "), rnd(st_.duckGain * 100.0f)), "%");
  g.textR(235, 15, buf, CYAN);
  // Curve from the engine's formulas (engine/ducker.h): floor = 1 - depth,
  // hold = 200 ms (v/127)^2, release = 20 ms * 50^(v/127), shape x / (x + a (1 - x)).
  const int src = globalValue(kGDuckSource);
  const float depth = float(globalValue(kGDuckDepth)) / 127.0f;
  const float h = float(globalValue(kGDuckHold)) / 127.0f;
  const float holdS = 0.2f * h * h;
  const float relS = 0.02f * powf(2.0f, float(globalValue(kGDuckRelease)) * (5.643856f / 127.0f));
  float cv = float(globalValue(kGDuckCurve) - 64) / 63.0f;
  cv = cv < -1.0f ? -1.0f : (cv > 1.0f ? 1.0f : cv);
  const float shapeA = powf(2.0f, cv * 3.0f);
  const int bpm = globalValue(kGTempo) > 0 ? globalValue(kGTempo) : 120;
  const float beatS = 60.0f / float(bpm);
  auto gain = [&](float beats) {
    if (src == 0) return 1.0f;
    const float tS = beats * beatS;
    const float fl = 1.0f - depth;
    if (tS < holdS) return fl;
    const float x = (tS - holdS) / relS;
    if (x >= 1.0f) return 1.0f;
    return fl + (1.0f - fl) * (x / (x + shapeA * (1.0f - x)));
  };
  const int gx = 6, gy = 30, gw = 144, gh = 45;
  g.rect(4, 27, 148, 50, PANEL);
  for (int q = 1; q < 4; ++q)
    for (int yy = gy; yy < gy + gh; yy += 3) g.pixel(gx + q * gw / 4, yy, LINE);
  for (int xx = gx; xx < gx + gw; xx += 3) g.pixel(xx, gy, LINE);
  int prev = -1;
  for (int i = 0; i < gw; ++i) {
    const float gg = gain(float(i) / float(gw));
    const int y = gy + rnd((1.0f - gg) * float(gh - 1));
    g.rect(gx + i, y, 1, gy + gh - y, CYAN_D);
    if (prev >= 0 && (y - prev > 1 || prev - y > 1)) g.rect(gx + i, y < prev ? y : prev, 1, (y > prev ? y - prev : prev - y) + 1, CYAN);
    g.pixel(gx + i, y, CYAN);
    g.pixel(gx + i, y + 1, CYAN);
    prev = y;
  }
  g.rect(gx, gy, 1, gh, tc(kTrackKick));
  static const char* const kSrc[3] = {"OFF", "KICK", "BEAT"};
  g.text(gx + 3, gy + gh - 8, kSrc[src < 3 ? src : 0], src == 0 ? uint8_t(DIM) : tc(kTrackKick));
  if (playing_ && src != 0) {
    const uint32_t since = now_ - (src == 1 ? lastTrigMs_[kTrackKick] : lastBeatMs_);
    float pos = float(since) / (beatS * 1000.0f);
    if (pos > 1.0f) pos = 1.0f;
    const int lx = gx + int(pos * float(gw - 1));
    const int ly = gy + rnd((1.0f - st_.duckGain) * float(gh - 1));
    g.rect(lx, gy, 1, gh, LINE);
    g.rect(lx - 2, ly - 2, 5, 5, WHITE);
  }
  g.textR(149, gy + gh - 8, "1 BEAT", DIM);
  // per-track duck amounts
  const int tf = (!focus_.global && focus_.index == kMixDuck) ? focus_.track : -1;
  const ParamInfo* dinfo = eng_->trackParamInfo(0, kMixDuck);
  if (tf < 0) {
    g.text(160, 27, "DUCKED", DIM);
  } else {
    g.text(160, 27, kTrackNames[tf], tc(tf));
    char v[kValueTextCap];
    formatValue(dinfo, ValueRef{false, uint8_t(tf), kMixDuck}, paramValue(tf, kMixDuck), v);
    g.textR(236, 27, v, WHITE);
  }
  for (int t = 0; t < kNumTracks; ++t) {
    const int x = 160 + t * 10;
    const bool f = t == tf;
    g.rect(x, 38, 7, 28, PANEL);
    const int hpx = rnd(float(paramValue(t, kMixDuck)) / 127.0f * 28.0f);
    g.rect(x, 66 - hpx, 7, hpx, (tf < 0 || f) ? tc(t) : tc(t, kDim));
    if (f) g.frame(x - 1, 37, 9, 30, WHITE);
    char k[2];
    keyStr(k, kPadKeys[t]);
    g.text(x + 1, 69, k, f ? uint8_t(WHITE) : tc(t, kMid));
  }
  // six parameter cards
  for (int c = 0; c < 6; ++c) {
    const int gid = kFxPage[kFxDuck][c];
    const ParamInfo* info = Engine::globalParamInfo(gid);
    if (!info) continue;
    const ValueRef ref{true, 0, uint8_t(gid)};
    const int v = globalValue(gid);
    const int x = 4 + c * 39;
    const bool f = focus_.global && focus_.index == gid;
    if (f) {
      g.rrect(x, 80, 37, 32, RAISED);
      g.rframe(x, 80, 37, 32, CYAN);
    }
    char k[2], nm[6], txt[kValueTextCap];
    keyStr(k, kNoteKeys[c]);
    upperCopy(info->name, nm, sizeof(nm));
    formatValue(info, ref, v, txt);
    g.text(x + 3, 83, k, f ? WHITE : DIM);
    g.textR(x + 33, 83, nm, f ? CYAN : TEXT);
    g.textR(x + 33, 93, txt, f ? WHITE : TEXT);
    g.rect(x + 3, 104, 31, 4, f ? BG : PANEL);
    if (info->fmt != ParamFormat::Enum) {
      g.rect(x + 3, 104, rnd(valueFrac(info, v) * 31.0f), 4, f ? CYAN : CYAN_D);
    } else {
      for (int s = 0; s < 3; ++s)
        g.rect(x + 3 + s * 11, 104, 9, 4, s != v - info->min ? uint8_t(CYAN_D) : (f ? uint8_t(CYAN) : uint8_t(DIM)));
    }
  }
  miniStrip(g, *this, 116, kTrackKick, shownPage_, playStep(kTrackKick), -1);
  const int x = g.text(4, 127, "A-K", TEXT);
  g.text(x, 127, " EDIT THAT TRACK'S DUCK AMOUNT", DIM);
}


// ============================================================== overlays ====

void Ui::drawOverlay(Framebuffer& fb) {
  Gfx g(fb);
  char buf[24], k[2];
  switch (overlay_) {
    case Overlay::Pattern: {
      g.rect(0, 72, W, 63, BG);
      g.rect(0, 73, W, 62, PANEL);
      g.rect(0, 73, W, 1, AMBER);
      int x = g.chip(4, 76, "PATTERN", AMBER);
      catP2(cat(buf, PG_ICON_PLAY), pat_ + 1);
      x = g.text(x + 6, 77, buf, GREEN);
      const int q = (st_.queuedPattern != 0xFF && st_.queuedPattern != pat_) ? st_.queuedPattern : -1;
      if (q >= 0) {
        catP2(cat(buf, "NEXT "), q + 1);
        g.text(x + 6, 77, buf, AMBER);
      }
      g.textR(235, 77, "+SHIFT = COPY TO", DIM);
      for (int i = 0; i < 16; ++i) {
        const int px = padX(i), py = padY(i);
        const bool used = patternUsed(i);
        uint8_t ink;
        if (i == pat_) { g.rrect(px, py, PAD_W, PAD_H, GREEN); ink = BG; }
        else if (used) { g.rrect(px, py, PAD_W, PAD_H, RAISED); ink = TEXT; }
        else { g.rrect(px, py, PAD_W, PAD_H, PANEL); ink = LINE; }
        if (i == q) {
          g.rframe(px, py, PAD_W, PAD_H, AMBER);
          g.rframe(px + 1, py + 1, PAD_W - 2, PAD_H - 2, AMBER);
          ink = AMBER;
        }
        catP2(buf, i + 1);
        g.textC(px + PAD_W / 2, py + (PAD_H - 7) / 2, buf, ink);
      }
      return;
    }
    case Overlay::Mute: {
      g.rect(0, 83, W, 52, BG);
      g.rect(0, 84, W, 51, PANEL);
      g.rect(0, 84, W, 1, WHITE);
      g.chip(4, 87, "MUTE", WHITE);
      g.textR(235, 88, "A-K MUTE   Z-, SOLO", DIM);
      for (int t = 0; t < kNumTracks; ++t) {
        const int x = 4 + t * 29;
        keyStr(k, kPadKeys[t]);
        if (muted(t)) {
          g.rrect(x, 99, 27, 20, BG);
          g.rframe(x, 99, 27, 20, LINE);
          g.text(x + 3, 101, k, DIM);
          g.textC(x + 14, 110, "MUTE", DIM);
        } else {
          g.rrect(x, 99, 27, 20, tc(t));
          g.text(x + 3, 101, k, tc(t, kDim));
          g.textC(x + 14, 110, kTrackNames[t], BG);
        }
        keyStr(k, kNoteKeys[t]);
        if (solo_ == t) {
          g.rrect(x, 121, 27, 11, WHITE);
          g.text(x + 3, 123, k, DIM);
          g.text(x + 13, 123, "SO", BG);
        } else {
          g.rrect(x, 121, 27, 11, RAISED);
          g.text(x + 3, 123, k, LINE);
          g.text(x + 13, 123, "SO", DIM);
        }
      }
      return;
    }
    case Overlay::Roll: {
      g.rect(0, 95, W, 40, BG);
      g.rect(0, 96, W, 39, PANEL);
      g.rect(0, 96, W, 1, WHITE);
      const int x = g.chip(4, 99, "ROLL", WHITE);
      g.text(x + 6, 100, kRollNames[rollRate_], AMBER);
      g.textR(235, 100, PG_ICON_UP PG_ICON_DOWN " RATE", DIM);
      for (int t = 0; t < kNumTracks; ++t) {
        const int px = 4 + t * 29;
        const bool on = (rollMask_ >> t) & 1;
        keyStr(k, kPadKeys[t]);
        g.rrect(px, 111, 27, 20, on ? uint8_t(WHITE) : tc(t));
        g.text(px + 3, 113, k, on ? uint8_t(DIM) : tc(t, kDim));
        g.textC(px + 14, 122, kTrackNames[t], BG);
      }
      return;
    }
    case Overlay::Tempo: {
      const int x = 44, y = 26, w = 152, h = 84;
      g.rect(x - 2, y - 2, w + 4, h + 4, BG);
      g.rrect(x, y, w, h, PANEL);
      g.rframe(x, y, w, h, WHITE);
      g.text(x + 8, y + 7, "TEMPO", DIM);
      cat3(buf, globalValue(kGTempo));
      g.text(x + 30, y + 20, buf, WHITE, 3);
      g.text(x + 90, y + 34, "BPM", DIM);
      const int ms = lastMaster_ != 0xFF ? lastMaster_ : -1;
      for (int b = 0; b < 4; ++b)
        g.rect(x + 112 + b * 8, y + 8, 5, 5, (ms >= 0 && (ms / 4) % 4 == b) ? WHITE : LINE);
      g.text(x + 8, y + 50, "SWING", DIM);
      const int sw = globalValue(kGSwing);
      formatValue(Engine::globalParamInfo(kGSwing), ValueRef{true, 0, kGSwing}, sw, buf);
      g.textR(x + 68, y + 50, buf, WHITE);
      g.rect(x + 76, y + 52, 66, 3, RAISED);
      g.rect(x + 76, y + 52, sw * 66 / 50, 3, WHITE);
      g.rect(x + 1, y + 62, w - 2, 1, LINE);
      g.text(x + 8, y + 66, "-/= BPM", TEXT);
      g.textR(x + w - 9, y + 66, PG_ICON_UP PG_ICON_DOWN " SWING", TEXT);
      g.textC(x + w / 2, y + 75, "TAP TAB x3 = TAP TEMPO", DIM);
      return;
    }
    default: return;
  }
}

void Ui::drawPopup(Framebuffer& fb) {
  Gfx g(fb);
  const ValueRef r = popRef_;
  const ParamInfo* info = infoFor(r);
  if (!info) return;
  const uint8_t col = r.global ? uint8_t(CYAN) : tc(r.track & 7);
  const int x = 48, y = 34, w = 144, h = 33;
  g.rect(x - 1, y - 1, w + 2, h + 2, BG);
  g.rrect(x, y, w, h, RAISED);
  g.rframe(x, y, w, h, col);
  char title[16], val[kValueTextCap];
  upperCopy(info->longName, title, sizeof(title));
  g.text(x + 6, y + 5, title, TEXT);
  const int sx = g.text(x + 6, y + 15, r.global ? kFxTabNames[fxTabOfGlobal(r.index)] : kTrackNames[r.track & 7], col);
  if (focusLocked_ && !r.global && r.track == focus_.track && r.index == focus_.index)
    g.text(sx + 6, y + 15, PG_ICON_LOCK "LOCKED", AMBER);
  const int v = valueOf(r);
  formatValue(info, r, v, val);
  g.textR(x + w - 7, y + 4, val, WHITE, 2);
  const int bx = x + 6, bw = w - 12;
  g.rect(bx, y + 25, bw, 4, BG);
  const float fr = valueFrac(info, v);
  if (barKind(info, r) == BarKind::Bipolar) {
    const int mid = bx + bw / 2;
    const int n = rnd((fr - 0.5f) * float(bw));
    if (n < 0) g.rect(mid + n, y + 25, -n, 4, col);
    else g.rect(mid, y + 25, n, 4, col);
    g.rect(mid, y + 24, 1, 6, WHITE);
  } else {
    g.rect(bx, y + 25, rnd(fr * float(bw)), 4, col);
  }
}

void Ui::drawToast(Framebuffer& fb) {
  Gfx g(fb);
  static const uint8_t kBorder[3] = {LINE, AMBER, RED};
  static const uint8_t kInk[3] = {TEXT, WHITE, WHITE};
  const int w = tw(toast_) + 16;
  const int x = (W - w) / 2;
  g.rect(x - 1, 43, w + 2, 19, BG);
  g.rrect(x, 44, w, 17, RAISED);
  g.rframe(x, 44, w, 17, kBorder[toastKind_]);
  g.text(x + 8, 49, toast_, kInk[toastKind_]);
}

// ============================================================== frame ====

uint32_t Ui::bodySignature() const {
  uint32_t h = 2166136261u;
  h = fnv(h, serial_);
  h = fnv(h, st_.revision);
  h = fnv(h, uint32_t(view_) | uint32_t(pat_) << 8 | uint32_t(playing_) << 16 | uint32_t(stepEditOpen()) << 17 |
                 uint32_t(overlay_) << 20);
  if (playing_)
    for (int t = 0; t < kNumTracks; ++t) h = fnv(h, st_.step[t]);
  uint32_t flash = 0;
  for (int t = 0; t < kNumTracks; ++t)
    if (before(now_, flashUntil_[t])) flash |= 1u << t;
  h = fnv(h, flash | uint32_t(st_.queuedPattern) << 8);
  if (overlay_ == Overlay::Tempo) h = fnv(h, lastMaster_ / 4);
  if (view_ == View::Fx && fxTab_ == kFxDuck) {
    h = fnv(h, uint32_t(rnd(st_.duckGain * 100.0f)));
    h = fnv(h, (now_ - lastTrigMs_[0]) / 8);
  }
  for (int t = 0; t < kNumTracks; ++t) h = fnv(h, uint32_t(muted(t)) | uint32_t(trackLen(t)) << 1);
  h = fnv(h, uint32_t(erasing_) | uint32_t(helpVisible()) << 1 | uint32_t(dirty_) << 2 | uint32_t(writeBusy_) << 3);
  if (view_ == View::Song) h = fnv(h, uint32_t(st_.songSlot) | uint32_t(st_.songRepeat) << 8 | uint32_t(st_.masterStep) << 16 | uint32_t(proj_->songMode) << 24);
  if (view_ == View::Project) h = fnv(h, uint32_t(env_.sdPresent));
  if (view_ == View::Sound) {
    uint32_t mv = 0;
    for (int m = 0; m < kTrackMacros; ++m) mv |= uint32_t(macroMoving(m)) << m;
    h = fnv(h, mv | uint32_t(held_ >> kKNote0 & 0xFF) << 8 | uint32_t(isHeld(kKShift)) << 16);   // moving macros, lit key letters, SHIFT's footer
    // tilt: the handles move with the engine's heard positions, the marks light
    uint32_t tp = uint32_t(tilt_.engaged && tilt_.track == sel_);
    for (int m = 0; m < kTrackMacros; ++m)
      if (tiltShows(sel_, m)) tp = tp * 257u + st_.macroPos[sel_][m] + 2u;
    h = fnv(h, tp);
  }
  return h;
}

void Ui::drawAll(Framebuffer& fb) {
  Gfx g(fb);
  g.clear(BG);
  drawStatus(fb);
  switch (view_) {
    case View::Groove:
      drawGroove(fb);
      if (stepEditOpen() && heldTrack_ == sel_) drawStepEdit(fb);
      break;
    case View::Sound: drawSound(fb); break;
    case View::Mix: drawMix(fb); break;
    case View::Fx: drawFx(fb); break;
    case View::Song: drawSong(fb); break;
    case View::Project: drawProject(fb); break;
  }
  if (banner_ && view_ == View::Groove) drawBanner(fb);
  if (overlay_ != Overlay::None) drawOverlay(fb);
  if (dialog_ != Dialog::None) drawDialog(fb);
  if (popUntil_) drawPopup(fb);
  if (toastUntil_) drawToast(fb);
  if (helpVisible()) drawHelp(fb);   // full screen, over everything
}

}  // namespace ui
}  // namespace pg
