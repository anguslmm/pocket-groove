// SONG view (UX.md 10.7): the chain of up to 32 slots (pattern x repeats).
// A slot with repeats == 0 ends the chain (project.h), so the chain length is
// the index of the first such slot.
#include <cstring>

#include "font.h"
#include "gfx.h"
#include "ui.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {
Command mk(CmdType t, int a, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}
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
}  // namespace

SongSlot Ui::songSlot(int i) const {
  i = i < 0 ? 0 : (i >= kSongSlots ? kSongSlots - 1 : i);
  if (pendSong_[i].used) return SongSlot{pendSong_[i].pattern, pendSong_[i].repeats};
  return proj_->song[i];
}

int Ui::songLength() const {
  for (int i = 0; i < kSongSlots; ++i)
    if (songSlot(i).repeats == 0) return i;
  return kSongSlots;
}

void Ui::setSongSlot(int i, int pattern, int repeats) {
  if (i < 0 || i >= kSongSlots) return;
  pendSong_[i] = PendSlot{uint8_t(pattern), uint8_t(repeats), 1, frame_};
  send(mk(CmdType::SetSongSlot, i, pattern, repeats));
  ++serial_;
}

void Ui::songStepKey(int n, bool shift) {
  const int len = songLength();
  const int c = songCursor_ > len ? len : songCursor_;
  if (shift || c == len) {                  // insert before the cursor / append on END
    if (len >= kSongSlots) { toast("CHAIN IS FULL (32 SLOTS)", 2); return; }
    for (int i = len - 1; i >= c; --i) {
      const SongSlot s = songSlot(i);
      setSongSlot(i + 1, s.pattern, s.repeats);
    }
    setSongSlot(c, n, 1);
    if (len + 1 < kSongSlots && songSlot(len + 1).repeats) setSongSlot(len + 1, 0, 0);   // keep the END marker
    if (c == len && !shift) songCursor_ = uint8_t(c + 1);   // appending: stay on END for the next one
    else songCursor_ = uint8_t(c);
    return;
  }
  setSongSlot(c, n, songSlot(c).repeats);   // overwrite the pattern, keep the repeats
}

void Ui::songDel() {
  const int len = songLength();
  const int c = songCursor_;
  if (c >= len) return;
  for (int i = c; i < len - 1; ++i) {
    const SongSlot s = songSlot(i + 1);
    setSongSlot(i, s.pattern, s.repeats);
  }
  setSongSlot(len - 1, 0, 0);
  if (len - 1 == 0 && proj_->songMode) send(mk(CmdType::SetSongMode, 0, 0, 0));   // nothing left to follow
}

void Ui::songMove(int dir) {
  const int len = songLength();
  int c = songCursor_ + dir;
  songCursor_ = uint8_t(c < 0 ? 0 : (c > len ? len : c));
}

void Ui::songEnter() {
  if (proj_->songMode) {
    send(mk(CmdType::SetSongMode, 0, 0, 0));
    return;
  }
  const int len = songLength();
  if (len == 0) { toast("ADD A PATTERN FIRST", 2); return; }
  // Start from the cursor slot (CCR-5).
  send(mk(CmdType::SetSongMode, songCursor_ < len ? songCursor_ : 0, 0, 1));
}

void Ui::songRepeats(int delta) {
  const int c = songCursor_;
  if (c >= songLength()) return;
  const SongSlot s = songSlot(c);
  int r = s.repeats + delta;
  r = r < 1 ? 1 : (r > 64 ? 64 : r);
  if (r != s.repeats) setSongSlot(c, s.pattern, r);
}

// ---- drawing ------------------------------------------------------------------

void Ui::drawSong(Framebuffer& fb) {
  Gfx g(fb);
  const int len = songLength();
  if (songCursor_ > len) songCursor_ = uint8_t(len);
  const bool on = proj_->songMode != 0;
  int x = g.chip(4, 14, "SONG", TEXT);
  if (on) g.chip(x + 6, 14, PG_ICON_CHAIN "CHAIN ON", GREEN);
  else g.text(x + 8, 15, "CHAIN OFF", DIM);
  char buf[16];
  for (int i = 0; i < 16; ++i) {
    const int px = 4 + (i % 8) * 19, py = 28 + (i / 8) * 19;
    uint8_t ink;
    if (i == pat_) { g.rrect(px, py, 17, 17, GREEN); ink = BG; }
    else if (patternUsed(i)) { g.rrect(px, py, 17, 17, RAISED); ink = TEXT; }
    else { g.rrect(px, py, 17, 17, PANEL); ink = LINE; }
    catP2(buf, i + 1);
    g.textC(px + 8, py + 5, buf, ink);
  }
  static const char* const kHelp[5][2] = {{"1-8 Q-I", "PUT PATTERN"}, {"SHIFT+", "INSERT PATTERN"},
                                          {PG_ICON_UP PG_ICON_DOWN, "MOVE CURSOR"}, {"-/=", "REPEATS"},
                                          {"ENTER", "CHAIN ON/OFF"}};
  for (int i = 0; i < 5; ++i) {
    g.text(4, 71 + i * 10, kHelp[i][0], TEXT);
    g.text(52, 71 + i * 10, kHelp[i][1], DIM);
  }
  // chain panel
  g.rect(158, 13, 82, 112, PANEL);
  g.text(162, 16, "CHAIN", DIM);
  char* o = buf;
  o += itoa10(len, o);
  cat(o, "/32");
  g.textR(235, 16, buf, DIM);
  // scroll so the cursor stays in rows 1..8 of the 10 visible
  const int rows = len + 1;
  if (songCursor_ < songTop_ + 1) songTop_ = uint8_t(songCursor_ > 0 ? songCursor_ - 1 : 0);
  if (songCursor_ > songTop_ + 8) songTop_ = uint8_t(songCursor_ - 8);
  if (songTop_ + 10 > rows) songTop_ = uint8_t(rows > 10 ? rows - 10 : 0);
  for (int r = 0; r < 10 && songTop_ + r < rows; ++r) {
    const int i = songTop_ + r;
    const int y = 27 + r * 10;
    const bool cur = i == songCursor_;
    if (cur) g.rrect(160, y - 1, 78, 9, WHITE);
    if (i == len) {
      g.text(162, y, "   END", cur ? BG : DIM);
      continue;
    }
    const SongSlot s = songSlot(i);
    const bool playingSlot = on && i == st_.songSlot;
    if (playingSlot) g.text(162, y, PG_ICON_PLAY, cur ? GREEN_D : GREEN);
    char n[4];
    n[0] = i + 1 < 10 ? ' ' : char('0' + (i + 1) / 10);
    n[1] = char('0' + (i + 1) % 10);
    n[2] = 0;
    g.text(170, y, n, cur ? LINE : DIM);
    catP2(cat(buf, "P"), s.pattern + 1);
    g.text(188, y, buf, cur ? BG : TEXT);
    o = cat(buf, "x");
    itoa10(s.repeats, o);
    g.text(212, y, buf, cur ? AMBER_D : AMBER);
    // song position: how far through this slot's repeats the playhead is
    if (playingSlot && playing_ && s.repeats) {
      const int plen = proj_->patterns[s.pattern & 15].length;
      const int ms = st_.masterStep < plen ? st_.masterStep : 0;
      const int done = (st_.songRepeat % s.repeats) * plen + ms;
      const int w = 66 * done / (s.repeats * (plen > 0 ? plen : 1));
      g.rect(170, y + 8, w, 1, GREEN);
    }
  }
  x = g.text(4, 127, "DEL", TEXT);
  x = g.text(x, 127, " REMOVE SLOT   ", DIM);
  x = g.text(x, 127, "OPT+STEP", TEXT);
  g.text(x, 127, " QUEUE", DIM);
}

}  // namespace ui
}  // namespace pg
