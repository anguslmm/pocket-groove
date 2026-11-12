// Mac/PC keyboard -> Cardputer keys, by PHYSICAL position (SDL scancodes, never
// keycodes or text input, so it works on any layout and Option+letter does
// not type symbols). docs/DESKTOP.md has the picture.
//
// The default table is kDefaultBindings in keymap.cpp; a small text file
// (--keymap FILE) can override single entries. Pure: no SDL calls except the
// scancode-name lookups, which are plain tables (they work before SDL_Init).
#pragma once
#include <cstdint>
#include <string>

namespace desk {

// What a scancode does. 0..56 = a Cardputer key id (pg::Key::id(), 56 = G0).
enum Binding : int16_t {
  kUnbound = -1,
  kActRecord = 100,   // F9: start / stop recording the output to a WAV
  kActStats = 101,    // F2: audio stats in the window title
};

struct KeyMap {
  static constexpr int kScancodes = 512;   // SDL_NUM_SCANCODES
  int16_t bind[kScancodes];
  // Command+scancode shortcuts: app actions only (kActRecord, kActStats),
  // never a Cardputer key: Command never reaches the instrument.
  int16_t cmdBind[kScancodes];

  KeyMap();           // the default table
  void clear();
  int lookup(int scancode) const { return scancode >= 0 && scancode < kScancodes ? int(bind[scancode]) : int(kUnbound); }
  int lookupCmd(int scancode) const {
    return scancode >= 0 && scancode < kScancodes ? int(cmdBind[scancode]) : int(kUnbound);
  }
  // First scancode bound to `b` (the canonical key the script player presses), or -1.
  int scancodeFor(int b) const;

  // Overrides from text, one per line ('#' comments):
  //     <cardputer key> = <SDL scancode name>      e.g.   fn = Right Shift
  //     none = <SDL scancode name>                 unbinds that scancode
  //     record = Cmd+<SDL scancode name>           a Command shortcut (actions only)
  //     none = Cmd+<SDL scancode name>             removes one
  // Cardputer keys by legend as in the manual and ui scripts (` 1 - = del tab q
  // [ ] \ fn sft a ; ' ent ctl opt alt z , . / spc G0, plus aliases such as
  // space, enter, shift, backspace), or the actions "record" and "stats".
  // Scancode names are SDL's (SDL_GetScancodeName: "A", "1", "Left Shift",
  // "Right Alt", "Backspace", "F1", ...), case-insensitive.
  // Returns false and sets *err ("line N: ...") on the first bad line;
  // the lines before it stay applied.
  bool parse(const char* text, std::string* err);
  bool loadFile(const char* path, std::string* err);
};

// Name of a binding for messages and --print-keymap ("fn", "G0", "record").
const char* bindingName(int b);
// Inverse: a Cardputer key legend / alias / action name -> binding, or kUnbound.
int bindingFromName(const char* name);

}  // namespace desk
