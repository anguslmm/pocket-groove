// The desktop key layer: the default physical map, overrides from text, and
// KeyboardInput (repeat, shared keys, Command, release on focus loss).
#include <SDL_keyboard.h>
#include <SDL_scancode.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "input.h"
#include "keymap.h"
#include "pg/ui/keys.h"

namespace {
int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

using namespace pg::ui;

void testDefaultMap() {
  desk::KeyMap m;
  // The four Cardputer rows, left to right, against the Mac key in the same place.
  const SDL_Scancode rows[4][14] = {
      {SDL_SCANCODE_GRAVE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
       SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS,
       SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE},
      {SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T,
       SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
       SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_BACKSLASH},
      {SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F,
       SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON,
       SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_RETURN},
      {SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C,
       SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD,
       SDL_SCANCODE_SLASH, SDL_SCANCODE_SPACE},
  };
  bool seen[kNumKeyIds] = {};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 14; ++c) {
      const int id = pg::Key{uint8_t(r), uint8_t(c)}.id();
      CHECK(m.lookup(rows[r][c]) == id);
      CHECK(m.scancodeFor(id) == rows[r][c]);   // the canonical key for scripts
      seen[id] = true;
    }
  for (int k = 0; k < pg::kNumKeys; ++k) CHECK(seen[k]);
  // Named modifiers as decided: SHIFT left Shift, FN right Shift, OPT left Option, ALT right Option.
  CHECK(m.lookup(SDL_SCANCODE_LSHIFT) == kKShift);
  CHECK(m.lookup(SDL_SCANCODE_RSHIFT) == kKFn);
  CHECK(m.lookup(SDL_SCANCODE_LALT) == kKOpt);
  CHECK(m.lookup(SDL_SCANCODE_RALT) == kKAlt);
  CHECK(m.lookup(SDL_SCANCODE_LCTRL) == kKCtrl && m.lookup(SDL_SCANCODE_RCTRL) == kKCtrl);
  CHECK(m.lookup(SDL_SCANCODE_F1) == kKG0);
  CHECK(m.lookup(SDL_SCANCODE_F9) == desk::kActRecord);
  CHECK(m.lookup(SDL_SCANCODE_F2) == desk::kActStats);
  // Command is never bound, nor Caps Lock or the arrows.
  CHECK(m.lookup(SDL_SCANCODE_LGUI) == desk::kUnbound);
  CHECK(m.lookup(SDL_SCANCODE_RGUI) == desk::kUnbound);
  CHECK(m.lookup(SDL_SCANCODE_CAPSLOCK) == desk::kUnbound);
  CHECK(m.lookup(SDL_SCANCODE_UP) == desk::kUnbound);
  CHECK(m.lookup(0) == desk::kUnbound && m.lookup(-5) == desk::kUnbound && m.lookup(4000) == desk::kUnbound);
}

void testParse() {
  desk::KeyMap m;
  std::string err;
  CHECK(m.parse("# swap the modifiers\n"
                "fn = Right Alt     # FN on right Option\n"
                "alt = right shift\n"
                "  = = Keypad +\n"
                "none = F2\n"
                "record = F10\n"
                "G0 = F12\n",
                &err));
  CHECK(m.lookup(SDL_SCANCODE_RALT) == kKFn);
  CHECK(m.lookup(SDL_SCANCODE_RSHIFT) == kKAlt);
  CHECK(m.lookup(SDL_SCANCODE_KP_PLUS) == kKEquals);
  CHECK(m.lookup(SDL_SCANCODE_EQUALS) == kKEquals);   // the default stays
  CHECK(m.lookup(SDL_SCANCODE_F2) == desk::kUnbound);
  CHECK(m.lookup(SDL_SCANCODE_F10) == desk::kActRecord);
  CHECK(m.lookup(SDL_SCANCODE_F12) == kKG0);
  CHECK(m.scancodeFor(kKFn) == SDL_SCANCODE_RALT);
  desk::KeyMap b;
  CHECK(!b.parse("fn Right Alt\n", &err));
  CHECK(err.find("line 1") != std::string::npos);
  CHECK(!b.parse("\n\nwobble = A\n", &err));
  CHECK(err.find("line 3") != std::string::npos);
  CHECK(!b.parse("a = NoSuchKey\n", &err));
  CHECK(!b.parse("a =\n", &err));
}

void testInput() {
  desk::KeyMap m;
  desk::KeyboardInput in(&m);
  desk::KeyOut o[desk::KeyboardInput::kMaxOut];
  // down, OS repeat ignored, up
  CHECK(in.onScancode(SDL_SCANCODE_A, true, false, false, false, o) == 1 && o[0].binding == 30 && o[0].down);
  CHECK(in.onScancode(SDL_SCANCODE_A, true, true, false, false, o) == 0);
  CHECK(in.onScancode(SDL_SCANCODE_A, true, false, false, false, o) == 0);   // a duplicate down
  CHECK(in.heldKeys() == (1ull << 30));
  CHECK(in.onScancode(SDL_SCANCODE_A, false, false, false, false, o) == 1 && o[0].binding == 30 && !o[0].down);
  CHECK(in.onScancode(SDL_SCANCODE_A, false, false, false, false, o) == 0);  // a stray up
  CHECK(!in.anyHeld());
  // both Controls are one CTRL: down on the first, up on the last
  CHECK(in.onScancode(SDL_SCANCODE_LCTRL, true, false, false, false, o) == 1 && o[0].binding == kKCtrl);
  CHECK(in.onScancode(SDL_SCANCODE_RCTRL, true, false, false, false, o) == 0);
  CHECK(in.onScancode(SDL_SCANCODE_LCTRL, false, false, false, false, o) == 0);
  CHECK(in.onScancode(SDL_SCANCODE_RCTRL, false, false, false, false, o) == 1 && !o[0].down);
  // unbound keys do nothing
  CHECK(in.onScancode(SDL_SCANCODE_UP, true, false, false, false, o) == 0);
  // actions fire on the press only
  CHECK(in.onScancode(SDL_SCANCODE_F9, true, false, false, false, o) == 1 && o[0].binding == desk::kActRecord);
  CHECK(in.onScancode(SDL_SCANCODE_F9, true, true, false, false, o) == 0);
  CHECK(in.onScancode(SDL_SCANCODE_F9, false, false, false, false, o) == 0);
  // Command: nothing goes down while it is held; pressing it releases everything
  CHECK(in.onScancode(SDL_SCANCODE_J, true, false, false, false, o) == 1);
  CHECK(in.onScancode(SDL_SCANCODE_LSHIFT, true, false, false, false, o) == 1);
  int n = in.onScancode(SDL_SCANCODE_LGUI, true, false, false, true, o);
  CHECK(n == 2 && o[0].binding == kKShift && !o[0].down && o[1].binding == 36 && !o[1].down);
  CHECK(in.onScancode(SDL_SCANCODE_Q, true, false, true, false, o) == 0);    // Cmd+Q plays nothing
  CHECK(in.onScancode(SDL_SCANCODE_J, false, false, true, false, o) == 0);   // already released
  CHECK(in.onScancode(SDL_SCANCODE_Q, false, false, true, false, o) == 0);
  CHECK(in.onScancode(SDL_SCANCODE_LGUI, false, false, false, true, o) == 0);
  CHECK(!in.anyHeld());
  // focus loss: an up for every held key, lowest id first, then nothing is held
  in.onScancode(SDL_SCANCODE_SPACE, true, false, false, false, o);
  in.onScancode(SDL_SCANCODE_RSHIFT, true, false, false, false, o);
  in.onScancode(SDL_SCANCODE_1, true, false, false, false, o);
  n = in.releaseAll(o);
  CHECK(n == 3 && o[0].binding == 1 && o[1].binding == kKFn && o[2].binding == kKSpace);
  for (int i = 0; i < n; ++i) CHECK(!o[i].down);
  CHECK(!in.anyHeld());
  CHECK(in.releaseAll(o) == 0);
  // the keys' later real ups (the OS sends them after focus comes back) are ignored
  CHECK(in.onScancode(SDL_SCANCODE_SPACE, false, false, false, false, o) == 0);
  // and the key works again
  CHECK(in.onScancode(SDL_SCANCODE_SPACE, true, false, false, false, o) == 1);
  in.releaseAll(o);
  // every Cardputer key round-trips: its canonical scancode down then up
  for (int k = 0; k < kNumKeyIds; ++k) {
    const int sc = m.scancodeFor(k);
    CHECK(sc > 0);
    CHECK(in.onScancode(sc, true, false, false, false, o) == 1 && o[0].binding == k && o[0].down);
    CHECK(in.onScancode(sc, false, false, false, false, o) == 1 && o[0].binding == k && !o[0].down);
  }
}

void testQueue() {
  desk::EventQueue q;
  desk::InputEvent e{desk::InputEvent::Note, 60, 100, false};
  for (int i = 0; i < 256; ++i) CHECK(q.post(e));
  CHECK(!q.post(e));
  desk::InputEvent got{desk::InputEvent::Key, 0, 0, false};
  int n = 0;
  while (q.pop(&got)) ++n;
  CHECK(n == 256 && got.a == 60 && got.b == 100);
}
}  // namespace

int main() {
  testDefaultMap();
  testParse();
  testInput();
  testQueue();
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
