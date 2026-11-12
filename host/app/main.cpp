// pocket-groove: the instrument on the desktop (docs/DESKTOP.md).
//
// The same pg::Instrument and pg::ui::Ui the firmware runs (src/app/main.cpp),
// wired to SDL2 instead of the Cardputer:
//   audio thread   SDL's audio callback (audio.cpp): Instrument::process, in
//                  stereo (or --mono, the device's mode). Nothing else.
//   main thread    SDL events -> key map -> Ui::onKey as they arrive; every
//                  33 ms (the device's frame) Ui::update, then the changed
//                  rows go to the window. Files, the recorder's WAV writes,
//                  the session save at quit: all here, never on the audio thread.
// Same single-writer contract as the device: the UI changes the engine only
// through Engine::post, and nothing here locks around the engine.
//
// Not here (device-only, src/app): boot menu, diagnostics strip, codec volume
// gesture, backlight. G0 (F1) is held back from the Ui as the firmware does,
// and G0 + - / = is swallowed as there, but changes no volume.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio.h"
#include "common/ghost_matrix.h"
#include "common/ui_harness.h"   // writeFramebufferPng (header-only; the Rig is not used)
#include "dirfs.h"
#include "display.h"
#include "input.h"
#include "keymap.h"
#include "pg/instrument/instrument.h"
#include "pg/io/project_io.h"
#include "pg/ui/gfx.h"
#include "pg/ui/keys.h"
#include "pg/ui/ui.h"
#include "recorder.h"
#include "script.h"

namespace {

constexpr uint32_t kFrameMs = 33;   // the firmware's UI frame (~30 fps)

struct Options {
  std::string dir = "projects";
  std::string sdDir;                 // empty = no SD card
  std::string recDir = "bounces/live";
  std::string keymapFile;
  std::string script;
  std::string outDir = "build/desktop";
  int scale = 4;
  int buffer = 256;
  int sr = 44100;
  bool mono = false;
  bool deviceKeys = false;
  bool record = false;
  bool printKeymap = false;
};

void usage(FILE* f) {
  std::fprintf(f,
               "usage: pocket-groove [options]\n"
               "  --dir PATH        projects directory (default ./projects, where tools/pg pull/push work)\n"
               "  --sd PATH         also mount PATH as the SD card (default: no card)\n"
               "  --scale N         window size, N x 240x135 points (default 4)\n"
               "  --buffer N        audio buffer in frames (default 256)\n"
               "  --sr 44100|32000  sample rate (32000 = the device's fallback rate)\n"
               "  --mono            the device's mono mode (what its jack plays)\n"
               "  --device-keys     model the Cardputer's diode-less key matrix and its phantom filter\n"
               "  --keymap FILE     key map overrides (docs/DESKTOP.md)\n"
               "  --print-keymap    print the active key map and exit\n"
               "  --rec-dir PATH    where F9 recordings go (default ./bounces/live)\n"
               "  --record          start recording at launch\n"
               "  --script FILE     run a key script (host/app/script.h), then keep running until it quits\n"
               "  --out DIR         where script shots go (default build/desktop)\n"
               "keys: F1 or Esc = G0, F9 or Cmd+R = record on/off, F2 or Cmd+I = stats in the title.\n"
               "Command never reaches the instrument.\n");
}

bool parseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs %s\n", a.c_str(), what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--dir") o->dir = next("a path");
    else if (a == "--sd") o->sdDir = next("a path");
    else if (a == "--rec-dir") o->recDir = next("a path");
    else if (a == "--keymap") o->keymapFile = next("a file");
    else if (a == "--script") o->script = next("a file");
    else if (a == "--out") o->outDir = next("a directory");
    else if (a == "--scale") o->scale = std::atoi(next("a factor"));
    else if (a == "--buffer") o->buffer = std::atoi(next("a frame count"));
    else if (a == "--sr") o->sr = std::atoi(next("44100 or 32000"));
    else if (a == "--mono") o->mono = true;
    else if (a == "--device-keys") o->deviceKeys = true;
    else if (a == "--record") o->record = true;
    else if (a == "--print-keymap") o->printKeymap = true;
    else if (a == "-h" || a == "--help") { usage(stdout); std::exit(0); }
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      usage(stderr);
      return false;
    }
  }
  if (o->sr != 44100 && o->sr != 32000) { std::fprintf(stderr, "--sr must be 44100 or 32000\n"); return false; }
  if (o->scale < 1 || o->scale > 16) { std::fprintf(stderr, "--scale must be 1..16\n"); return false; }
  if (o->buffer < 16 || o->buffer > 8192) { std::fprintf(stderr, "--buffer must be 16..8192\n"); return false; }
  return true;
}

// ---- the instrument (static, as on the device) ---------------------------------------
pg::Instrument g_inst;
pg::ui::Ui g_ui;
pg::Framebuffer g_fb;
desk::AudioOut g_audio;     // holds the 4 MB recording ring
desk::KeyMap g_map;
uih::GhostMatrix g_matrix;  // --device-keys only

class App final : public desk::ScriptHost {
 public:
  Options opt;
  desk::DirFs* fs = nullptr;
  desk::Display display;
  desk::Recorder rec;
  desk::KeyboardInput keyboard{&g_map};
  desk::EventQueue events;   // other sources (MIDI, later)
  bool running = true;
  bool audioOk = false;
  bool showStats = false;
  bool needPresent = true;
  bool g0Held = false;
  uint64_t swallow = 0;      // keys whose up must not reach the Ui (G0 + - / =)
  uint64_t t0Us = 0;
  std::string title;

  uint32_t nowMs() const { return uint32_t(SDL_GetTicks64()); }
  uint32_t nowUs() const {
    return uint32_t(SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency());
  }

  // ---- keys into the Ui -------------------------------------------------------------
  // One Cardputer key change, as the device's handleKey sees a controller event.
  void cardKey(int k, bool down) {
    const uint32_t now = nowMs(), t = nowUs();
    const uint64_t bit = 1ull << k;
    if (k == pg::ui::kKG0) {   // the firmware keeps G0 to itself (volume, diagnostics): so do we
      g0Held = down;
      return;
    }
    if (down && g0Held && (k == pg::ui::kKMinus || k == pg::ui::kKEquals)) {   // the firmware's volume gesture
      swallow |= bit;
      return;
    }
    if (!down && (swallow & bit)) {
      swallow &= ~bit;
      return;
    }
    if (!opt.deviceKeys) {
      g_ui.onKey(pg::KeyEvent{uint8_t(k), down, t}, now);
      return;
    }
    // The device's keyboard: phantoms of the diode-less matrix appear, in the
    // same batch, and the Ui's ghost filter has to sort them out.
    pg::KeyEvent ev[pg::kNumKeys];
    const int n = g_matrix.change(k, down, t, ev);
    for (int i = 0; i < n; ++i) g_ui.onKey(ev[i], now);
  }

  void deliver(const desk::KeyOut* out, int n) {
    for (int i = 0; i < n; ++i) {
      const desk::KeyOut& o = out[i];
      if (o.binding == desk::kActRecord) toggleRecord();
      else if (o.binding == desk::kActStats) showStats = !showStats;
      else if (o.binding >= 0 && o.binding < pg::ui::kNumKeyIds) cardKey(o.binding, o.down);
    }
  }

  void releaseAll() {
    desk::KeyOut out[desk::KeyboardInput::kMaxOut];
    deliver(out, keyboard.releaseAll(out));
  }

  void onSdlKey(const SDL_KeyboardEvent& k) {
    const int sc = k.keysym.scancode;
    const bool isCmd = sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI;
    desk::KeyOut out[desk::KeyboardInput::kMaxOut];
    const int n = keyboard.onScancode(sc, k.type == SDL_KEYDOWN, k.repeat != 0, (k.keysym.mod & KMOD_GUI) != 0,
                                      isCmd, out);
    deliver(out, n);
  }

  // ---- recording ------------------------------------------------------------------
  void toggleRecord() {
    if (rec.active()) {
      rec.stop();
      std::printf("[rec] stopped: %s (%.1f s)\n", rec.path().c_str(), rec.seconds());
    } else {
      char err[160];
      if (!rec.start(err, sizeof(err))) {
        std::printf("[rec] FAILED: %s\n", err);
      } else {
        std::printf("[rec] started: %s at audio frame %llu\n", rec.path().c_str(),
                    static_cast<unsigned long long>(rec.startFrame()));
      }
    }
    std::fflush(stdout);
    title.clear();   // refresh
  }

  // ---- window title: project, recording, stats --------------------------------------
  void updateTitle(const desk::AudioStats& as) {
    char t[256];
    int n = std::snprintf(t, sizeof(t), "Pocket Groove - %s%s", g_ui.projectName()[0] ? g_ui.projectName() : "?",
                          g_ui.dirty() ? "*" : "");
    if (rec.active()) {
      const int s = int(rec.seconds());
      n += std::snprintf(t + n, sizeof(t) - size_t(n), "   [REC %d:%02d]", s / 60, s % 60);
    }
    if (!audioOk) n += std::snprintf(t + n, sizeof(t) - size_t(n), "   NO AUDIO");
    if (showStats && audioOk)
      std::snprintf(t + n, sizeof(t) - size_t(n), "   audio %d%% (peak %d%%) late %u gaps %u, %d Hz, %d fr, %dx px%s%s",
                    int(as.loadPct + 0.5f), int(as.peakLoadPct + 0.5f), as.late, as.gaps, as.sampleRate,
                    as.bufferFrames, display.pixelScale(), opt.mono ? ", mono" : "",
                    opt.deviceKeys ? ", device keys" : "");
    if (title != t) {
      title = t;
      display.setTitle(t);
    }
  }

  // ---- ScriptHost -----------------------------------------------------------------
  void pushKey(int sc, bool down, uint16_t mod) override {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = SDL_GetTicks();
    e.key.windowID = display.windowId();
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = SDL_Scancode(sc);
    e.key.keysym.sym = SDL_GetKeyFromScancode(SDL_Scancode(sc));
    e.key.keysym.mod = mod;
    SDL_PushEvent(&e);
  }
  void focusLost() override {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_WINDOWEVENT;
    e.window.windowID = display.windowId();
    e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    SDL_PushEvent(&e);
  }
  bool shot(const std::string& name) override {
    mkdirAll(opt.outDir);
    uiFrame();   // as ui_shot: one update, then the framebuffer
    const std::string p1 = opt.outDir + "/" + name + ".png", p4 = opt.outDir + "/" + name + "_4x.png";
    const bool ok = uih::writeFramebufferPng(p1.c_str(), g_fb, 1) && uih::writeFramebufferPng(p4.c_str(), g_fb, 4);
    if (ok) std::printf("[shot] %s\n", p1.c_str());
    return ok;
  }
  bool allReleased() override {
    return !keyboard.anyHeld() && g_ui.heldKeys() == 0 && (!opt.deviceKeys || g_matrix.phys == 0);
  }
  void quit() override {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
  }

  static void mkdirAll(const std::string& d) { desk::makeDirs(d); }

  // ---- one UI frame -------------------------------------------------------------------
  void uiFrame() {
    const desk::AudioStats as = g_audio.stats();
    pg::ui::UiEnv env;
    env.nowMs = nowMs();
    env.batteryPct = pg::ui::kBatteryUnknown;   // no battery: empty icon, no low-battery save
    env.sdPresent = !opt.sdDir.empty() && fs->mounted(pg::io::kMountSd);
    const float cpu = audioOk ? as.loadPct : 0.0f;
    env.cpuPct = uint8_t(cpu < 0.0f ? 0 : cpu > 100.0f ? 100 : int(cpu + 0.5f));
    if (g_ui.update(env, g_fb)) {
      int y0 = 0, y1 = 0;
      g_ui.dirtyRows(&y0, &y1);
      display.upload(g_fb, y0, y1);
      needPresent = true;
    }
  }
};

App g_app;

}  // namespace

int main(int argc, char** argv) {
  App& app = g_app;
  Options& opt = app.opt;
  if (!parseArgs(argc, argv, &opt)) return 2;
  if (!opt.keymapFile.empty()) {
    std::string err;
    if (!g_map.loadFile(opt.keymapFile.c_str(), &err)) {
      std::fprintf(stderr, "--keymap %s: %s\n", opt.keymapFile.c_str(), err.c_str());
      return 2;
    }
  }
  if (opt.printKeymap) {
    for (int sc = 1; sc < desk::KeyMap::kScancodes; ++sc)
      if (g_map.cmdBind[sc] != desk::kUnbound)
        std::printf("%-6s = Cmd+%s\n", desk::bindingName(g_map.cmdBind[sc]), SDL_GetScancodeName(SDL_Scancode(sc)));
    for (int sc = 1; sc < desk::KeyMap::kScancodes; ++sc)
      if (g_map.bind[sc] != desk::kUnbound)
      {
        const char* name = SDL_GetScancodeName(SDL_Scancode(sc));
        if (name && *name) std::printf("%-6s = %s\n", desk::bindingName(g_map.bind[sc]), name);
        else std::printf("%-6s = (scancode %d, no name: ISO key)\n", desk::bindingName(g_map.bind[sc]), sc);
      }
    return 0;
  }
  desk::Script script;
  bool scripted = false;
  if (!opt.script.empty()) {
    std::string err;
    if (!script.load(opt.script.c_str(), g_map, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    scripted = true;
  }

  SDL_SetMainReady();
  // Ctrl+click is the Cardputer CTRL key here, not a right click; keep the
  // keyboard ours (no IME / text input: scancodes only).
  SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "0");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Pocket Groove");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_StopTextInput();
  SDL_version v;
  SDL_GetVersion(&v);

  // Files: the projects directory is the device's /int/pg (dirfs.h).
  static desk::DirFs fs(opt.dir, opt.sdDir);
  app.fs = &fs;
  if (!fs.createMounts()) {
    std::fprintf(stderr, "cannot create %s%s%s\n", opt.dir.c_str(), opt.sdDir.empty() ? "" : " or ", opt.sdDir.c_str());
    SDL_Quit();
    return 1;
  }

  char err[200] = "";
  if (!app.display.open(opt.scale, err, sizeof(err))) {
    std::fprintf(stderr, "%s\n", err);
    SDL_Quit();
    return 1;
  }

  // Boot as the firmware's setup(): instrument, UI, session or demo 0, then audio last.
  pg::ui::initPalette(g_fb);
  std::memset(g_fb.px, 0, sizeof(g_fb.px));
  g_inst.init(float(opt.sr), opt.mono);
  g_ui.init(g_inst.engine(), g_inst.project(), &fs);
  g_ui.setGhostFilter(opt.deviceKeys);   // a computer keyboard has no phantoms to filter
  const bool hadSession = fs.exists(pg::io::kSessionPath);
  g_ui.boot(app.nowMs());
  std::printf("[boot] SDL %d.%d.%d (%s, %s), projects in %s%s%s, %s session, project \"%s\"\n", v.major, v.minor,
              v.patch, SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "-",
              opt.dir.c_str(), opt.sdDir.empty() ? "" : ", SD card ", opt.sdDir.c_str(),
              hadSession ? "resumed the" : "no", g_ui.projectName());

  app.audioOk = g_audio.open(&g_inst, opt.sr, opt.buffer, err, sizeof(err));
  if (!app.audioOk) std::printf("[audio] NOT RUNNING: %s\n", err);
  app.rec.init(&g_audio.ring(), opt.sr, opt.recDir);
  if (app.audioOk) {
    const desk::AudioStats as = g_audio.stats();
    std::printf("[audio] %d Hz %s, %d frames per callback (asked %d), device \"%s\"\n", opt.sr,
                opt.mono ? "mono (device mode)" : "stereo", as.bufferFrames, opt.buffer,
                SDL_GetCurrentAudioDriver());
    if (opt.record) app.toggleRecord();
    g_audio.start();
  }
  std::fflush(stdout);

  uint32_t nextFrame = app.nowMs();
  uint32_t lastTitle = 0;
  bool scriptDone = !scripted;
  while (app.running) {
    // Wait for input until the next UI frame; keys go to the Ui the moment they arrive.
    int32_t rem = int32_t(nextFrame - app.nowMs());
    SDL_Event e;
    bool got = rem > 0 ? SDL_WaitEventTimeout(&e, rem > 0 ? rem : 0) != 0 : SDL_PollEvent(&e) != 0;
    while (got) {
      switch (e.type) {
        case SDL_QUIT: app.running = false; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: app.onSdlKey(e.key); break;
        case SDL_WINDOWEVENT:
          switch (e.window.event) {
            case SDL_WINDOWEVENT_FOCUS_LOST:
            case SDL_WINDOWEVENT_HIDDEN:
            case SDL_WINDOWEVENT_MINIMIZED: app.releaseAll(); break;   // no stuck notes or modifiers
            case SDL_WINDOWEVENT_CLOSE: app.running = false; break;
            default: app.needPresent = true; break;                   // exposed, resized, moved
          }
          break;
        default: break;
      }
      got = SDL_PollEvent(&e) != 0;
    }
    // Other input sources (none yet: MIDI will post here).
    for (desk::InputEvent ie; app.events.pop(&ie);)
      if (ie.kind == desk::InputEvent::Key && ie.a < pg::ui::kNumKeyIds) app.cardKey(ie.a, ie.down);

    if (!scriptDone && !script.service(app.nowMs(), app)) scriptDone = true;

    const uint32_t now = app.nowMs();
    if (int32_t(now - nextFrame) >= 0) {
      app.uiFrame();
      nextFrame += kFrameMs;
      if (int32_t(now - nextFrame) > int32_t(kFrameMs)) nextFrame = now + kFrameMs;   // after a stall: no burst
    }
    app.rec.drain();
    if (app.rec.active() && app.rec.full()) app.toggleRecord();
    if (now - lastTitle >= 250) {
      lastTitle = now;
      app.updateTitle(g_audio.stats());
    }
    if (app.needPresent) {
      app.display.present();
      app.needPresent = false;
    }
  }

  // Quit (window closed, Cmd+Q, Ctrl+C in the terminal): the device's save path.
  app.releaseAll();
  if (app.rec.active()) app.toggleRecord();
  const bool saved = g_ui.saveSessionNow();
  std::printf("[session] %s %s\n", saved ? "saved to" : "NOT SAVED to", fs.real(pg::io::kSessionPath).c_str());
  g_audio.close();
  app.display.close();
  SDL_Quit();
  const int scriptErrors = scripted ? script.errors() : 0;
  if (scriptErrors) std::printf("[script] %d error(s)\n", scriptErrors);
  std::fflush(stdout);
  return (saved ? 0 : 1) | (scriptErrors ? 1 : 0);
}
