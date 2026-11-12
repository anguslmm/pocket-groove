#include "script.h"

#include <SDL_keyboard.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pg/ui/keys.h"

namespace desk {

namespace {
uint16_t modOf(int sc) {
  switch (sc) {
    case SDL_SCANCODE_LSHIFT: return KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL: return KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL: return KMOD_RCTRL;
    case SDL_SCANCODE_LALT: return KMOD_LALT;
    case SDL_SCANCODE_RALT: return KMOD_RALT;
    case SDL_SCANCODE_LGUI: return KMOD_LGUI;
    case SDL_SCANCODE_RGUI: return KMOD_RGUI;
    default: return 0;
  }
}
}  // namespace

bool Script::load(const char* path, const KeyMap& map, std::string* err) {
  path_ = path;
  FILE* f = std::fopen(path, "r");
  if (!f) {
    *err = std::string("cannot open ") + path;
    return false;
  }
  char line[256];
  int lineNo = 0;
  bool ok = true;
  auto fail = [&](const std::string& what) {
    *err = path_ + ":" + std::to_string(lineNo) + ": " + what;
    ok = false;
  };
  while (ok && std::fgets(line, sizeof(line), f)) {
    ++lineNo;
    if (char* h = std::strchr(line, '#')) *h = 0;
    char cmd[32] = {0}, a[64] = {0}, b[64] = {0};
    const int n = std::sscanf(line, "%31s %63s %63s", cmd, a, b);
    if (n <= 0) continue;
    const std::string op = cmd;
    Cmd c;
    c.line = lineNo;
    auto cardKey = [&](const char* name) {
      const int k = pg::ui::keyFromName(name);
      const int sc = k >= 0 ? map.scancodeFor(k) : -1;
      if (k < 0) fail(std::string("unknown key '") + name + "'");
      else if (sc < 0) fail(std::string("key '") + name + "' is not on the key map");
      return sc;
    };
    if (op == "down" || op == "up" || op == "tap") {
      c.sc = cardKey(a);
      if (!ok) break;
      c.op = op == "up" ? Cmd::Up : Cmd::Down;
      cmds_.push_back(c);
      if (op == "tap") {
        Cmd w;
        w.op = Cmd::Wait;
        w.ms = n > 2 ? uint32_t(std::atoi(b)) : 40;
        w.line = lineNo;
        cmds_.push_back(w);
        c.op = Cmd::Up;
        cmds_.push_back(c);
      }
    } else if (op == "sdl") {
      // the scancode name is the rest of the line after the verb ("Left GUI")
      const char* rest = std::strstr(line, a);
      rest = rest ? rest + std::strlen(a) : "";
      while (*rest == ' ' || *rest == '\t') ++rest;
      std::string name = rest;
      while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
      const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
      if (sc == SDL_SCANCODE_UNKNOWN) { fail("unknown scancode name '" + name + "'"); break; }
      const std::string verb = a;
      if (verb != "down" && verb != "up" && verb != "tap") { fail("sdl down|up|tap <name>"); break; }
      c.sc = sc;
      c.op = verb == "up" ? Cmd::Up : Cmd::Down;
      cmds_.push_back(c);
      if (verb == "tap") {
        Cmd w;
        w.op = Cmd::Wait;
        w.ms = 40;
        w.line = lineNo;
        cmds_.push_back(w);
        c.op = Cmd::Up;
        cmds_.push_back(c);
      }
    } else if (op == "wait") {
      c.op = Cmd::Wait;
      c.ms = uint32_t(std::atoi(a));
      cmds_.push_back(c);
    } else if (op == "focuslost") {
      c.op = Cmd::Focus;
      cmds_.push_back(c);
    } else if (op == "shot") {
      if (!a[0]) { fail("shot <name>"); break; }
      c.op = Cmd::Shot;
      c.arg = a;
      cmds_.push_back(c);
    } else if (op == "expect-released") {
      c.op = Cmd::Expect;
      cmds_.push_back(c);
    } else if (op == "quit") {
      c.op = Cmd::Quit;
      cmds_.push_back(c);
    } else if (op == "app") {
      const char* rest = std::strstr(line, "app") + 3;
      while (*rest == ' ' || *rest == '\t') ++rest;
      std::string args = rest;
      while (!args.empty() && (args.back() == '\n' || args.back() == '\r' || args.back() == ' ')) args.pop_back();
      if (args.empty()) { fail("app <command>"); break; }
      c.op = Cmd::App;
      c.arg = args;
      cmds_.push_back(c);
    } else if (op == "battery" || op == "sd" || op == "boot" || op == "demo" || op == "empty") {
      continue;   // ui_shot rig setup: see script.h
    } else {
      fail("unknown command '" + op + "'");
    }
  }
  std::fclose(f);
  return ok;
}

bool Script::service(uint32_t nowMs, ScriptHost& host) {
  while (pc_ < cmds_.size()) {
    const Cmd& c = cmds_[pc_];
    if (c.op == Cmd::Wait) {
      if (!waiting_) {
        waiting_ = true;
        until_ = nowMs + c.ms;
      }
      if (int32_t(nowMs - until_) < 0) return true;
      waiting_ = false;
      ++pc_;
      continue;
    }
    switch (c.op) {
      case Cmd::Down:
        mod_ = uint16_t(mod_ | modOf(c.sc));
        host.pushKey(c.sc, true, mod_);
        break;
      case Cmd::Up:
        mod_ = uint16_t(mod_ & ~modOf(c.sc));
        host.pushKey(c.sc, false, mod_);
        break;
      case Cmd::Focus:
        mod_ = 0;
        host.focusLost();
        break;
      case Cmd::Shot:
        if (!host.shot(c.arg)) {
          std::fprintf(stderr, "%s:%d: cannot write shot %s\n", path_.c_str(), c.line, c.arg.c_str());
          ++errors_;
        }
        break;
      case Cmd::Expect:
        if (!host.allReleased()) {
          std::fprintf(stderr, "%s:%d: expect-released: keys still held\n", path_.c_str(), c.line);
          ++errors_;
        }
        break;
      case Cmd::Quit:
        host.quit();
        break;
      case Cmd::App: {
        const int r = host.command(c.arg, nowMs);
        if (r > 0) return true;   // not done yet: again on the next pass
        if (r < 0) {
          std::fprintf(stderr, "%s:%d: app %s: failed\n", path_.c_str(), c.line, c.arg.c_str());
          ++errors_;
        }
        ++pc_;
        return true;   // let the main loop handle what it pushed
      }
      case Cmd::Wait:
        break;
    }
    ++pc_;
    // Let the main loop handle what was just pushed before the next command
    // (a shot or an expectation must see the effect of the keys before it).
    if (c.op == Cmd::Down || c.op == Cmd::Up || c.op == Cmd::Focus) return true;
  }
  return false;
}

}  // namespace desk
