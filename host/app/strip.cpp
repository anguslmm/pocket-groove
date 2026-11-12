#include "strip.h"

#include <cstdio>
#include <cstring>

#include "pg/ui/gfx.h"
#include "pg/ui/keys.h"

namespace desk {

using namespace pg::ui;

const StripModifier kStripModifiers[7] = {
    {kKShift, "SHIFT", "LSHFT"}, {kKFn, "FN", "RSHFT"}, {kKCtrl, "CTRL", "CTRL"}, {kKOpt, "OPT", "LOPT"},
    {kKAlt, "ALT", "ROPT"},     {kKTab, "TAB", "TAB"}, {kKG0, "G0", "ESC"},
};

namespace {

constexpr int kModW = 34, kModX0 = 1;           // 7 columns of 34 px
constexpr int kMacY = 3, kChipY = 11;
constexpr int kBtnY = 23, kBtnH = 11, kBtnW = 57, kBtnX0 = 2, kBtnGap = 2;
constexpr int kStatusY = 37;

// The Command key sign, 5x5 (no such glyph in the instrument's font).
const uint8_t kCmdGlyph[5] = {0x1B, 0x1F, 0x0A, 0x1F, 0x1B};   // columns, bit 0 = top

int cmdText(Gfx& g, int x, int y, const char* label, char key, uint8_t c) {
  x = g.text(x, y, label, c);
  for (int cx = 0; cx < 5; ++cx)
    for (int cy = 0; cy < 5; ++cy)
      if ((kCmdGlyph[cx] >> cy) & 1) g.pixel(x + cx, y + 1 + cy, c);
  x += 6;
  const char k[2] = {key, 0};
  return g.text(x, y, k, c);
}

int cmdTextWidth(const char* label) { return tw(label) + 1 + 6 + 5; }

}  // namespace

void stripButtonCentre(int b, int* x, int* y) {
  *x = kBtnX0 + b * (kBtnW + kBtnGap) + kBtnW / 2;
  *y = kBtnY + kBtnH / 2;
}

void stripModifierCentre(int i, int* x, int* y) {
  *x = kModX0 + i * kModW + kModW / 2;
  *y = kChipY + 4;
}

int stripHit(int x, int y) {
  if (y < kBtnY || y >= kBtnY + kBtnH) return kBtnNone;
  for (int b = 0; b < kNumButtons; ++b) {
    const int x0 = kBtnX0 + b * (kBtnW + kBtnGap);
    if (x >= x0 && x < x0 + kBtnW) return b;
  }
  return kBtnNone;
}

void drawStrip(pg::Framebuffer& fb, const StripState& s) {
  Gfx g(fb);
  g.rect(0, 0, pg::kScreenW, kStripH, PANEL);
  g.rect(0, 0, pg::kScreenW, 1, LINE);

  // modifier legend
  for (int i = 0; i < 7; ++i) {
    const StripModifier& m = kStripModifiers[i];
    const int cx = kModX0 + i * kModW + kModW / 2;
    const bool on = (s.held >> m.key) & 1;
    g.textC(cx, kMacY, m.mac, DIM);
    const int w = tw(m.card) + 4;
    g.rrect(cx - w / 2, kChipY, w, 9, on ? AMBER : RAISED);
    g.text(cx - w / 2 + 2, kChipY + 1, m.card, on ? BG : TEXT);
  }

  // controls
  char rec[24];
  if (s.rec) std::snprintf(rec, sizeof(rec), "REC %d:%02d", s.recSec / 60, s.recSec % 60);
  struct B {
    const char* label;
    char key;
    bool on;
    uint8_t col;
  } btn[kNumButtons] = {
      {"LISTEN", 'L', s.listenWanted, uint8_t(s.listenWarn ? AMBER : GREEN)},
      {"STEMS", 'B', s.stemsOpen || s.stemsBusy, CYAN},
      {s.rec ? rec : "REC", 'R', s.rec, RED},
      {"STATS", 'I', s.stats, AMBER},
  };
  for (int b = 0; b < kNumButtons; ++b) {
    const int x0 = kBtnX0 + b * (kBtnW + kBtnGap);
    g.rrect(x0, kBtnY, kBtnW, kBtnH, btn[b].on ? btn[b].col : uint8_t(RAISED));
    const uint8_t ink = btn[b].on ? BG : TEXT;
    if (b == kBtnRec && s.rec) {
      g.textC(x0 + kBtnW / 2, kBtnY + 2, btn[b].label, ink);   // the time instead of the shortcut
    } else {
      char l[16];
      std::snprintf(l, sizeof(l), "%s ", btn[b].label);
      cmdText(g, x0 + (kBtnW - cmdTextWidth(l)) / 2, kBtnY + 2, l, btn[b].key, ink);
    }
  }

  // status: the device, then Listen (or the stats)
  int x = 2;
  if (s.devicePresent) x = g.chip(x, kStatusY - 1, "DEVICE", GREEN_D, GREEN);
  else x = g.chip(x, kStatusY - 1, "NO DEVICE", LINE, DIM);
  g.text(x + 4, kStatusY, s.status.c_str(), s.stats ? TEXT : s.listenWarn ? AMBER : s.listenActive ? GREEN : DIM);
}

}  // namespace desk
