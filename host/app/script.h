// --script FILE: drives the running app through the real SDL event path, for
// the headless system test and for reproducing a gesture. Key commands are
// turned into SDL keyboard events (scancodes from the active key map) and
// pushed onto SDL's queue, so they take exactly the path a real key takes.
//
// One command per line, '#' starts a comment. The key commands and their
// timing are those of host/tools/ui_shot, so one key script drives both:
//   down <key> | up <key>      a Cardputer key by legend (` 1 q a fn sft spc G0 ... as in ui_shot)
//   tap <key> [ms]             down, wait ms (default 40), up
//   wait <ms>                  let the app run
//   sdl down|up|tap <name>     an SDL scancode by name ("F9", "Left GUI", "Right Shift")
//   focuslost                  the window loses keyboard focus
//   shot <name>                <out>/<name>.png (240x135) and <out>/<name>_4x.png
//   expect-released            fail unless no Cardputer key is held (desktop layer and Ui)
//   quit                       as closing the window
//   app <command ...>          a desktop-app command (main.cpp, App::command): click X Y
//                              (canvas pixels, through SDL's mouse events), winshot NAME,
//                              listen-sim plug|unplug, wait-stems MS, ...; it may keep the
//                              script waiting until it is done
// Lines with ui_shot's setup commands (battery, sd, boot, demo, empty) are
// skipped: the app boots like the device and its UiEnv is fixed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "keymap.h"

namespace desk {

struct ScriptHost {
  virtual ~ScriptHost() = default;
  virtual void pushKey(int scancode, bool down, uint16_t mod) = 0;
  virtual void focusLost() = 0;
  virtual bool shot(const std::string& name) = 0;
  virtual bool allReleased() = 0;
  virtual void quit() = 0;
  // `app ...`: 0 done, 1 not yet (asked again on the next pass), -1 failed.
  virtual int command(const std::string& args, uint32_t nowMs) { (void)args; (void)nowMs; return -1; }
};

class Script {
 public:
  bool load(const char* path, const KeyMap& map, std::string* err);
  // Runs every command that is due at nowMs. Returns false when finished.
  bool service(uint32_t nowMs, ScriptHost& host);
  int errors() const { return errors_; }

 private:
  struct Cmd {
    enum Op : uint8_t { Down, Up, Wait, Focus, Shot, Expect, Quit, App } op;
    int sc = 0;
    uint32_t ms = 0;
    std::string arg;
    int line = 0;
  };
  std::vector<Cmd> cmds_;
  size_t pc_ = 0;
  bool waiting_ = false;
  uint32_t until_ = 0;
  uint16_t mod_ = 0;   // modifier state the pushed events carry (KMOD_*)
  int errors_ = 0;
  std::string path_;
};

}  // namespace desk
