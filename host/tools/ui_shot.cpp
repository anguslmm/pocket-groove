// ui_shot: drive the real Ui + Engine from a script and write screenshots.
//
//   ui_shot <script.txt> [out_dir]          (default out_dir: build/ui/shots)
//
// Script format, one command per line, '#' starts a comment:
//   demo | empty            start a fresh rig with the factory demo / an empty project
//   dev <0|1>               developer mode for the next demo / empty / boot (default 1: the
//                           parameter pages the older scripts shoot; 0 = the player's SOUND page)
//   down <key>  up <key>    key events by legend: ` 1 .. 8 9 0 - = del tab q .. i o p [ ] \ fn sft a .. k
//                           l ; ' ent ctl opt alt z .. , . / spc G0 (aliases: space enter shift minus
//                           equals comma dot semicolon quote backtick lbracket rbracket ...)
//   tap <key> [ms]          down, run ms (default 40), up
//   wait <ms>               advance the clock: the engine renders audio, the UI updates at 30 fps
//   until <track> <step>    run until status.step[track] == step (both 0-based)
//   freeze | unfreeze       stop / resume calling Engine::process (the clock still runs)
//   battery <pct>           UiEnv battery level
//   global <id> <value>     post SetGlobal directly (data setup only)
//   param <t> <i> <value>   post SetTrackParam directly
//   steppost <t> <i> <on> [note vel]  post SetStep directly on the playing pattern
//   boot                    power-on with the current (in-memory) file system: session or demo
//   sd <0|1>                SD card present (file system and UiEnv)
//   fsave <int|sd> <NAME> [bpm]   write the current project to a file (setup only)
//   cursor <ROM|INT|SD> <NAME>   PROJECT view: move the list cursor onto that row
//   session                 write the current project as the session file _last.pgv (setup only)
//   shot <name>             write <out>/<name>.png (240x135) and <out>/<name>_4x.png
//   bench                   print the host time of a full redraw of the current screen
//   tilt <x> <y>            the tilt axes (-1..1, after the response) while G0 is held: `down G0`
//                           starts the gesture as the firmware does (it engages after 150 ms),
//                           the axes go to the Ui every 20 ms, `up G0` glides back; a `- / =`
//                           while G0 is held cancels it (the firmware's volume keys)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "common/ui_harness.h"
#include "pg/io/project_io.h"
#include "pg/demo/demo.h"
#include <initializer_list>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: ui_shot script.txt [out_dir]\n");
    return 2;
  }
  const std::string out = argc > 2 ? argv[2] : "build/ui/shots";
  mkdir(out.c_str(), 0755);
  FILE* f = std::fopen(argv[1], "r");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  static uih::Rig rig;
  bool started = false;
  char line[256];
  int lineNo = 0, errors = 0;
  while (std::fgets(line, sizeof(line), f)) {
    ++lineNo;
    char* hash = std::strchr(line, '#');
    if (hash) *hash = 0;
    char cmd[32] = {0}, a[64] = {0}, b[64] = {0}, c[64] = {0}, d[64] = {0}, e[64] = {0};
    const int n = std::sscanf(line, "%31s %63s %63s %63s %63s %63s", cmd, a, b, c, d, e);
    if (n <= 0) continue;
    const std::string op = cmd;
    if (op == "demo" || op == "empty") {
      rig.start(op == "demo");
      started = true;
      continue;
    }
    if (op == "dev") {
      rig.devMode = std::atoi(a) != 0;
      if (started) rig.ui.setDevMode(rig.devMode);
      continue;
    }
    if (!started) {
      rig.start(true);
      started = true;
    }
    auto keyArg = [&](const char* s) {
      const int k = pg::ui::keyFromName(s);
      if (k < 0) {
        std::fprintf(stderr, "%s:%d: unknown key '%s'\n", argv[1], lineNo, s);
        ++errors;
      }
      return k;
    };
    if (op == "down") {
      const int k = keyArg(a);
      if (k >= 0) rig.down(k);
    } else if (op == "up") {
      const int k = keyArg(a);
      if (k >= 0) rig.up(k);
    } else if (op == "tap") {
      const int k = keyArg(a);
      if (k >= 0) rig.tap(k, n > 2 ? uint32_t(std::atoi(b)) : 40);
    } else if (op == "wait") {
      rig.run(uint32_t(std::atoi(a)));
    } else if (op == "until") {
      if (!rig.runUntilStep(std::atoi(a), std::atoi(b))) {
        std::fprintf(stderr, "%s:%d: step never reached\n", argv[1], lineNo);
        ++errors;
      }
    } else if (op == "boot") {
      rig.boot();
    } else if (op == "sd") {
      rig.fs.sdPresent = std::atoi(a) != 0;
      rig.env.sdPresent = rig.fs.sdPresent;
      rig.run(40);
    } else if (op == "fsave") {
      static pg::Project tmp;
      tmp = *rig.proj;
      if (n > 3) tmp.globals[pg::kGTempo] = uint8_t(std::atoi(c));
      const pg::io::Status st =
          pg::io::saveProject(rig.fs, std::string(a) == "sd" ? "/sd" : "/int", b, tmp);
      if (st != pg::io::Status::Ok) {
        std::fprintf(stderr, "%s:%d: fsave failed: %s\n", argv[1], lineNo, pg::io::statusName(st));
        ++errors;
      }
    } else if (op == "cursor") {
      int target = -1, i = 0;
      const std::string tag = a;
      for (int d = 0; d < pg::demo::count(); ++d, ++i)
        if (tag == "ROM" && std::strcmp(pg::demo::name(d), b) == 0) target = i;
      for (const char* m : {"/int", "/sd"}) {
        if (!rig.fs.mounted(m)) continue;
        pg::io::ProjectEntry e[pg::ui::kListPerMount];
        const int cnt = pg::io::listProjects(rig.fs, m, e, pg::ui::kListPerMount);
        for (int j = 0; j < cnt; ++j, ++i)
          if (((tag == "INT" && m[1] == 'i') || (tag == "SD" && m[1] == 's')) && std::strcmp(e[j].name, b) == 0)
            target = i;
      }
      if (target < 0) {
        std::fprintf(stderr, "%s:%d: no row %s %s\n", argv[1], lineNo, a, b);
        ++errors;
      } else {
        target += rig.ui.projectTab() == 1 ? 1 : 0;
        for (int g = 0; g < 80 && rig.ui.projectCursor() < target; ++g) rig.tap(pg::ui::kKDot, 30);
        for (int g = 0; g < 80 && rig.ui.projectCursor() > target; ++g) rig.tap(pg::ui::kKSemicolon, 30);
      }
    } else if (op == "session") {
      if (pg::io::saveSession(rig.fs, *rig.proj) != pg::io::Status::Ok) ++errors;
    } else if (op == "freeze") {
      rig.engineRuns = false;
    } else if (op == "unfreeze") {
      rig.engineRuns = true;
    } else if (op == "tilt") {
      rig.tiltX = float(std::atof(a));
      rig.tiltY = float(std::atof(b));
    } else if (op == "battery") {
      rig.env.batteryPct = uint8_t(std::atoi(a));
    } else if (op == "global" || op == "param" || op == "steppost") {
      pg::Command cm;
      std::memset(&cm, 0, sizeof(cm));
      if (op == "global") {
        cm.type = pg::CmdType::SetGlobal;
        cm.a = uint8_t(std::atoi(a));
        cm.c = uint8_t(std::atoi(b));
      } else if (op == "param") {
        cm.type = pg::CmdType::SetTrackParam;
        cm.a = uint8_t(std::atoi(a));
        cm.b = uint8_t(std::atoi(b));
        cm.c = uint8_t(std::atoi(c));
      } else {
        cm.type = pg::CmdType::SetStep;
        cm.a = uint8_t(rig.ui.editPattern());
        cm.b = uint8_t(std::atoi(a));
        cm.c = uint8_t(std::atoi(b));
        cm.step = pg::kDefaultStep;
        if (std::atoi(c)) cm.step.setFlags(pg::kStepOn);
        if (n > 4) cm.step.note = uint8_t(std::atoi(d));
        if (n > 5) cm.step.vel = uint8_t(std::atoi(e));
      }
      rig.eng.post(cm);
      rig.run(5);
    } else if (op == "shot") {
      rig.frame();
      const std::string p1 = out + "/" + a + ".png", p4 = out + "/" + a + "_4x.png";
      if (!uih::writeFramebufferPng(p1.c_str(), rig.fb, 1) || !uih::writeFramebufferPng(p4.c_str(), rig.fb, 4)) {
        std::fprintf(stderr, "cannot write %s\n", p1.c_str());
        ++errors;
      } else {
        std::printf("%s\n", p1.c_str());
      }
    } else if (op == "bench") {
      // Force full redraws of the current state and time them.
      const int iters = 2000;
      pg::ui::UiEnv env = rig.env;
      auto t0 = std::chrono::steady_clock::now();
      uint32_t full0 = rig.ui.fullRedraws();
      for (int i = 0; i < iters; ++i) {
        rig.ui.onKey(pg::KeyEvent{uint8_t(pg::ui::kKG0), (i & 1) == 0, 0}, rig.now());   // G0 bumps the UI serial
        rig.ui.update(env, rig.fb);
      }
      auto t1 = std::chrono::steady_clock::now();
      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
      std::printf("bench %s: %.1f us per update (%u full redraws of %d), sizeof(Ui) = %zu B\n", a, us,
                  rig.ui.fullRedraws() - full0, iters, sizeof(pg::ui::Ui));
    } else {
      std::fprintf(stderr, "%s:%d: unknown command '%s'\n", argv[1], lineNo, cmd);
      ++errors;
    }
  }
  std::fclose(f);
  return errors ? 1 : 0;
}
