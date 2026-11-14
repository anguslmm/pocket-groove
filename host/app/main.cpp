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
// and G0 + - / = is swallowed as there, but changes no volume (it cancels a tilt,
// as there).
//
// Tilt (docs/UX.md 10.3 "Tilt"): there is no IMU, so while G0 is held the mouse
// stands in for it. Its position at the press is the zero pose; moving it right
// is rolling right, up is tilting away, kTiltDegPerPx degrees per canvas pixel,
// through the same per-axis responses (pg::tilt::Axis) and the same Ui path
// (Ui::tiltPress / tiltInput / tiltRelease / tiltCancel) as the device, at the
// device's 100 Hz. The lab's XY pad is separate and unchanged.
//
// The desktop's own additions (docs/DESKTOP.md), none of which touches the
// instrument: the strip under the screen (strip.h: modifier lights, controls,
// status), Listen (listen.h: the device's USB audio input mixed into the
// output) and the Stems list (stems.h: tools/pg ls / bounce as a subprocess).
// With --lab, the macro lab (lab.h, docs/MACROLAB.md): a development workbench
// that drives track parameters from hot-reloaded .pgm macro sets, through the
// Ui's own value path (Ui::setTrackParam).
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "common/ghost_matrix.h"
#include "common/ui_harness.h"   // writeFramebufferPng (header-only; the Rig is not used)
#include "dirfs.h"
#include "display.h"
#include "input.h"
#include "keymap.h"
#include "lab.h"
#include "listen.h"
#include "pg/engine/engine_hooks.h"
#include "pg/instrument/instrument.h"
#include "pg/io/project_io.h"
#include "pg/macro/macro.h"
#include "pg/tilt/tilt.h"
#include "pg/ui/gfx.h"
#include "pg/ui/keys.h"
#include "pg/ui/ui.h"
#include "recorder.h"
#include "common/host_resample.h"
#include "mic.h"
#include "mic_sdl.h"
#include "samples.h"
#include "script.h"
#include "stems.h"
#include "strip.h"

namespace {

constexpr uint32_t kFrameMs = 33;   // the firmware's UI frame (~30 fps)

struct Options {
  std::string dir = "projects";
  std::string samplesDir;            // --samples; empty = "samples" next to the projects directory
  std::string sdDir;                 // empty = no SD card
  std::string recDir = "bounces/live";
  std::string keymapFile;
  std::string script;
  std::string outDir = "build/desktop";
  std::string pgTool;                // tools/pg for the Stems list (default: next to this build, or $PG_TOOL)
  std::string pgPort;                // --port for tools/pg (default: auto-detect, or $PG_PORT)
  bool listenSim = false;            // --listen-sim PPM: a simulated device instead of SDL's capture list
  double listenSimPpm = 0.0;
  bool listenSimSpec = false;        // --listen-sim-spec: the simulated device delivers this obtained spec
  desk::CaptureSpec listenSimSpecV;
  int listenSimBurst = 1;            // --listen-sim-burst N: callbacks back to back
  bool listenSimVary = false;        // --listen-sim-vary: callback sizes vary
  std::string listenDevice;          // --listen-device NAME: another input than "Pocket Groove"
  std::string listenDump;            // --listen-dump FILE.wav: the raw capture as SDL delivers it
  std::string micDevice;             // --mic-device NAME: record from this input (exact or part of a name)
  bool listInputs = false;           // --list-inputs: print the capture devices' names and exit
  bool lab = false;                  // --lab [DIR]: the macro lab
  bool dev = false;                  // --dev: developer mode (the parameter pages; --lab implies it)
  std::string labDir = "macros";
  std::string labNotes = "lab-notes.txt";
  int scale = 4;
  bool scaleGiven = false;
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
               "  --samples PATH    the sample pool's folder: 01-name.wav .. 12-name.wav, one per slot\n"
               "                    (default: samples/ next to the projects directory; docs/DESKTOP.md)\n"
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
               "  --pg-tool PATH    tools/pg for the Stems list (default: this repository's, or $PG_TOOL)\n"
               "  --pg-port PORT    the device's serial port for the Stems list (default: auto, or $PG_PORT)\n"
               "  --listen-dump FILE.wav  Listen: also write the raw capture exactly as SDL delivers it\n"
               "                    (the obtained format, channels and rate in the WAV header)\n"
               "  --listen-device NAME  Listen to this input (exact, or part of its name), not Pocket Groove\n"
               "  --mic-device NAME  hold L on a sampler page: record from this input (exact, or part of its\n"
               "                    name; default: the system's input unless it is Pocket Groove, else a microphone)\n"
               "  --list-inputs     print the audio inputs' names, one per line, and exit\n"
               "  --listen-sim PPM  test: Listen plays a simulated device whose clock runs PPM fast\n"
               "  --listen-sim-spec FMT:CH:RATE:FRAMES  test: ... which delivers this spec (e.g. f32:1:48000:1024)\n"
               "  --listen-sim-burst N  test: ... N callbacks back to back;  --listen-sim-vary: sizes vary\n"
               "  --lab [DIR]       the macro lab (docs/MACROLAB.md): macro sets from DIR (default ./macros),\n"
               "                    hot-reloaded; marks go to ./lab-notes.txt (--lab-notes FILE). Implies --dev\n"
               "  --dev             developer mode: SOUND can also show the bare parameter pages (l flips)\n"
               "keys: F1 or Esc = G0, F9 or Cmd+R = record on/off, F2 or Cmd+I = stats,\n"
               "Cmd+L = listen to the connected device, Cmd+B = stems. Command never reaches the instrument.\n"
               "With --lab: Cmd+G / Cmd+D mark good / bad, Cmd+Left / Right the set, Cmd+1..4 a macro,\n"
               "Cmd+Up / Down move it (docs/MACROLAB.md).\n");
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
    else if (a == "--samples") o->samplesDir = next("a path");
    else if (a == "--sd") o->sdDir = next("a path");
    else if (a == "--rec-dir") o->recDir = next("a path");
    else if (a == "--keymap") o->keymapFile = next("a file");
    else if (a == "--script") o->script = next("a file");
    else if (a == "--out") o->outDir = next("a directory");
    else if (a == "--pg-tool") o->pgTool = next("a path");
    else if (a == "--pg-port") o->pgPort = next("a port");
    else if (a == "--listen-sim") { o->listenSim = true; o->listenSimPpm = std::atof(next("a clock offset in ppm")); }
    else if (a == "--listen-sim-spec") {
      const char* v = next("FORMAT:CHANNELS:RATE:FRAMES");
      if (!desk::CaptureSpec::parse(v, &o->listenSimSpecV)) {
        std::fprintf(stderr, "--listen-sim-spec %s: want e.g. s16:2:44100:256 or f32:1:48000:1024\n", v);
        return false;
      }
      o->listenSimSpec = true;
    }
    else if (a == "--listen-sim-burst") o->listenSimBurst = std::atoi(next("a callback count"));
    else if (a == "--listen-sim-vary") o->listenSimVary = true;
    else if (a == "--listen-device") o->listenDevice = next("an input name");
    else if (a == "--listen-dump") o->listenDump = next("a .wav file");
    else if (a == "--mic-device") o->micDevice = next("an input name");
    else if (a == "--list-inputs") o->listInputs = true;
    else if (a == "--scale") { o->scale = std::atoi(next("a factor")); o->scaleGiven = true; }
    else if (a == "--lab") {
      o->lab = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') o->labDir = argv[++i];
    }
    else if (a == "--lab-notes") o->labNotes = next("a file");
    else if (a == "--dev") o->dev = true;
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
  if (o->lab && !o->scaleGiven) o->scale = 3;   // the panel widens the window: keep it on a laptop screen
  return true;
}

// ---- the instrument (static, as on the device) ---------------------------------------
pg::Instrument g_inst;
pg::ui::Ui g_ui;
pg::Framebuffer g_fb;
desk::AudioOut g_audio;     // holds the 4 MB recording ring
desk::KeyMap g_map;
uih::GhostMatrix g_matrix;  // --device-keys only
// The desktop's additions
pg::Framebuffer g_stripFb;      // the strip (its top kStripH rows)
pg::Framebuffer g_overlayFb;    // the Stems list
desk::DriftBuffer g_listenBuf;  // Listen's rate match (128 KB)
desk::CaptureSink g_listenSink; // Listen's capture conversion
desk::SdlCapture g_sdlCapture;
uint8_t g_labPx[desk::kLabW * desk::kCanvasH];   // the macro lab panel (--lab)

const char* buttonName(int b) {
  static const char* const n[] = {"listen", "stems", "rec", "stats"};
  return b >= 0 && b < desk::kNumButtons ? n[b] : "none";
}

class App final : public desk::ScriptHost, public desk::LabHost {
 public:
  Options opt;
  desk::DirFs* fs = nullptr;
  desk::Display display;
  desk::Recorder rec;
  desk::KeyboardInput keyboard{&g_map};
  desk::EventQueue events;   // other sources (MIDI, later)
  desk::Listen listen;
  desk::Stems stems;
  desk::Lab lab;
  desk::Samples samples;
  desk::SdlMicSource micSource;   // hold L on a sampler page (mic.h)
  desk::Mic mic;
  hostrs::HostResample resample;  // SHIFT+L on a sampler page: the engine's own output (no input device)
  bool labMouse = false;     // a drag that started on the lab panel
  bool textInput = false;    // SDL text input on (only while a lab note is typed)
  std::unique_ptr<desk::SimCapture> simCapture;
  desk::StripState strip;
  bool stripDrawn = false;
  uint64_t litLogged = ~0ull;
  std::string statsText;
  uint32_t uiKeyEvents = 0;  // key events that reached the Ui (the tests count them)
  uint32_t waitUntil = 0;    // `app wait-...` deadline
  bool waiting = false;
  bool running = true;
  bool audioOk = false;
  bool showStats = false;
  bool needPresent = true;
  bool g0Held = false;
  uint64_t swallow = 0;      // keys whose up must not reach the Ui (G0 + - / =)
  // tilt by mouse (see the header)
  static constexpr float kTiltDegPerPx = 0.5f;   // 70 canvas px (280 window points at 4x) = the smooth range
  static constexpr uint32_t kTiltEveryMs = 10;   // the device's IMU rate
  static constexpr float kTiltRangePx = 70.0f;   // script `tilt X Y`: X = 1 is this far from the press point
  pg::tilt::Axis tiltAxis[2];
  int mouseX = 0, mouseY = 0;                    // the last mouse position, window points (from motion events)
  int tiltX0 = 0, tiltY0 = 0;                    // canvas pixels at the press
  uint32_t tiltNext = 0;
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
    if (k == pg::ui::kKG0) {   // the firmware keeps G0 to itself (volume, diagnostics, tilt): so do we
      if (down == g0Held) return;
      g0Held = down;
      if (down) tiltPress(now);
      else g_ui.tiltRelease(now);
      return;
    }
    if (down && g0Held && (k == pg::ui::kKMinus || k == pg::ui::kKEquals)) {   // the firmware's volume gesture
      swallow |= bit;
      g_ui.tiltCancel(now);
      return;
    }
    if (!down && (swallow & bit)) {
      swallow &= ~bit;
      return;
    }
    ++uiKeyEvents;
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
      else if (o.binding == desk::kActStats) toggleStats();
      else if (o.binding == desk::kActListen) toggleListen();
      else if (o.binding == desk::kActStems) toggleStems();
      else if (o.binding >= desk::kActLabGood && o.binding <= desk::kActLast) {
        if (lab.enabled() && !stems.isOpen()) {
          lab.action(o.binding, (SDL_GetModState() & KMOD_SHIFT) != 0);
          labTyping();
        }
      }
      else if (o.binding >= 0 && o.binding < pg::ui::kNumKeyIds) cardKey(o.binding, o.down);
    }
  }

  void releaseAll() {
    desk::KeyOut out[desk::KeyboardInput::kMaxOut];
    deliver(out, keyboard.releaseAll(out));
  }

  // ---- tilt by mouse --------------------------------------------------------------
  void canvasMouse(int* cx, int* cy) const { display.toCanvasAny(mouseX, mouseY, cx, cy); }
  void tiltPress(uint32_t now) {
    g_ui.tiltPress(now);
    const pg::ui::Ui::TiltSpec ts = g_ui.tiltSpec();
    for (int a = 0; a < 2; ++a) tiltAxis[a].setup(ts.response[a], float(kTiltEveryMs) * 0.001f);
    canvasMouse(&tiltX0, &tiltY0);
    tiltNext = now;
  }
  // The axes from the mouse, every kTiltEveryMs while G0 is held.
  void tiltService(uint32_t now) {
    if (!g_ui.tiltHeld() || int32_t(now - tiltNext) < 0) return;
    tiltNext = now + kTiltEveryMs;
    int cx = 0, cy = 0;
    canvasMouse(&cx, &cy);
    const float x = tiltAxis[0].update(float(cx - tiltX0) * kTiltDegPerPx);
    const float y = tiltAxis[1].update(float(tiltY0 - cy) * kTiltDegPerPx);
    g_ui.tiltInput(x, y, now);
  }

  // ---- the macro lab ----------------------------------------------------------------
  // LabHost: the lab moves the track's own instrument and macros through the UI's value
  // path (Ui::setInstrument / Ui::setMacro), and is the library's overlay.
  int selectedTrack() override { return g_ui.selTrack(); }
  int paramValue(int track, int index) override { return g_ui.paramValue(track, index); }
  int projectValue(int track, int index) override {
    const pg::TrackSettings& ts = g_inst.project().tracks[track & 7];
    return index >= pg::kParamsPerTrack ? ts.macro[(index - pg::kParamsPerTrack) & 3] : ts.params[index & 31];
  }
  uint16_t instrument(int track) override { return g_ui.instrumentIdOf(track); }
  uint16_t projectInstrument(int track) override { return pg::instrumentId(g_inst.project().tracks[track & 7]); }
  void setMacro(int track, int m, int value) override { g_ui.setMacro(track, m, value); }
  void setInstrument(int track, uint16_t id, const uint8_t* pos) override { g_ui.setInstrument(track, id, pos); }
  void refreshInstruments() override {
    pg::Command c{};
    c.type = pg::CmdType::RefreshInstruments;
    c.a = 0xFF;
    while (!g_inst.engine().post(c)) SDL_Delay(1);   // the lab's memory waits on this one: never drop it
  }
  uint8_t instrumentRefreshes() override { return g_inst.engine().instrumentRefreshes(); }
  uint32_t lockedMask(int track) override {
    const pg::TrackPattern& tp = g_inst.project().patterns[g_ui.editPattern() & 15].tracks[track & 7];
    uint32_t m = 0;
    if (!pg::instrumentId(g_inst.project().tracks[track & 7])) return 0;   // CLASSIC: locks, no macro values
    for (int i = 0; i < tp.length && i < pg::kMaxSteps; ++i)
      if (tp.steps[i].on())
        for (int k = 0; k < pg::kTrackMacros; ++k)
          if (tp.steps[i].hasMacro(k)) m |= 1u << k;
    return m;
  }
  // While a note is typed after a mark the keyboard is the lab's (text input on).
  void labTyping() {
    if (lab.typing() == textInput) return;
    textInput = lab.typing();
    if (textInput) {
      releaseAll();   // no instrument key stays held while the note has the keyboard
      SDL_StartTextInput();
    } else {
      SDL_StopTextInput();
    }
    needPresent = true;
  }
  void labFrame(uint32_t now) {
    if (!lab.enabled()) return;
    lab.tick(now);
    if (lab.changed()) {
      lab.draw(g_labPx);
      display.uploadLab(g_labPx, g_fb.palette);
      needPresent = true;
    }
  }

  void onSdlKey(const SDL_KeyboardEvent& k) {
    const int sc = k.keysym.scancode;
    const bool isCmd = sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI;
    if (lab.typing()) {   // the note after a mark: nothing reaches the instrument
      if (k.type != SDL_KEYDOWN) return;
      if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) lab.typeKey(desk::Lab::kTypeEnter);
      else if (sc == SDL_SCANCODE_ESCAPE) lab.typeKey(desk::Lab::kTypeEscape);
      else if (sc == SDL_SCANCODE_BACKSPACE) lab.typeKey(desk::Lab::kTypeBackspace);
      labTyping();
      return;
    }
    if (stems.isOpen()) {   // the list has the keyboard: nothing reaches the instrument
      if (k.type != SDL_KEYDOWN || isCmd) return;
      const int a = (k.keysym.mod & KMOD_GUI) ? g_map.lookupCmd(sc) : g_map.lookup(sc);
      if ((k.keysym.mod & KMOD_GUI) || (a >= desk::kActRecord && sc != SDL_SCANCODE_ESCAPE)) {
        if (a >= desk::kActRecord && !k.repeat) {
          const desk::KeyOut o{int16_t(a), true};
          deliver(&o, 1);
        }
        return;
      }
      stems.key(sc, k.repeat != 0);
      needPresent = true;
      return;
    }
    desk::KeyOut out[desk::KeyboardInput::kMaxOut];
    const int n = keyboard.onScancode(sc, k.type == SDL_KEYDOWN, k.repeat != 0, (k.keysym.mod & KMOD_GUI) != 0,
                                      isCmd, out);
    deliver(out, n);
  }

  // ---- the strip's controls ---------------------------------------------------------
  void toggleListen() {
    if (!audioOk) {
      std::printf("[listen] no audio output, so nothing to listen through\n");
      return;
    }
    listen.toggle(nowMs());
  }
  void toggleStats() {
    showStats = !showStats;
    if (showStats && listen.active()) listen.printStats();   // the full counters in the terminal
  }
  void toggleStems() {
    if (stems.isOpen()) {
      stems.close();
    } else {
      releaseAll();   // no instrument key stays held while the list has the keyboard
      stems.open();
    }
    needPresent = true;
  }
  void click(int wx, int wy) {
    int cx = 0, cy = 0;
    if (!display.toCanvas(wx, wy, &cx, &cy)) return;
    if (cx >= desk::kCanvasW) {   // the lab panel
      if (lab.enabled()) {
        labMouse = true;
        lab.mouseDown(cx - desk::kCanvasW, cy);
        labTyping();
      }
      return;
    }
    if (cy < pg::kScreenH) return;   // the device screen: nothing
    const int b = desk::stripHit(cx, cy - pg::kScreenH);
    if (b == desk::kBtnNone) return;
    std::printf("[strip] click %s\n", buttonName(b));
    std::fflush(stdout);
    if (b == desk::kBtnListen) toggleListen();
    else if (b == desk::kBtnStems) toggleStems();
    else if (b == desk::kBtnRec) toggleRecord();
    else if (b == desk::kBtnStats) toggleStats();
  }
  void updateStrip() {
    desk::StripState s;
    s.held = keyboard.heldKeys();
    s.devicePresent = listen.present();
    s.listenWanted = listen.wanted();
    s.listenActive = listen.active();
    s.listenWarn = listen.hintIsWarning();
    s.stemsOpen = stems.isOpen();
    s.stemsBusy = stems.busy();
    s.rec = rec.active();
    s.recSec = rec.active() ? int(rec.seconds()) : 0;
    s.stats = showStats;
    // STATS while listening: the audio stats and Listen's counters take turns (2 s each).
    s.status = !showStats ? listen.statusLine()
               : listen.active() && (nowMs() / 2000) % 2 ? listen.stripStats() : statsText;
    if (stripDrawn && s == strip) return;
    uint64_t lit = 0;
    for (const desk::StripModifier& m : desk::kStripModifiers) lit |= s.held & (1ull << m.key);
    if (lit != litLogged) {
      litLogged = lit;
      std::printf("[strip] lit:");
      for (const desk::StripModifier& m : desk::kStripModifiers)
        if ((lit >> m.key) & 1) std::printf(" %s", m.card);
      std::printf("%s\n", lit ? "" : " -");
      std::fflush(stdout);
    }
    strip = s;
    stripDrawn = true;
    desk::drawStrip(g_stripFb, s);
    display.uploadStrip(g_stripFb);
    needPresent = true;
  }
  void updateOverlay() {
    if (!stems.isOpen() || !stems.changed()) return;
    stems.draw(g_overlayFb);
    display.uploadOverlay(g_overlayFb);
    needPresent = true;
  }
  // The whole window (device screen or Stems list, and the strip) at `scale`.
  bool windowShot(const std::string& path, int scale) {
    const int W = display.canvasW() * scale, H = desk::kCanvasH * scale;
    std::vector<uint8_t> rgb(size_t(W) * size_t(H) * 3);
    const pg::Framebuffer& top = stems.isOpen() ? g_overlayFb : g_fb;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const int cy = y / scale, cx = x / scale;
        const pg::Framebuffer& fb = cy < pg::kScreenH ? top : g_stripFb;
        const int ry = cy < pg::kScreenH ? cy : cy - pg::kScreenH;
        const uint32_t v = cx >= desk::kCanvasW
                               ? desk::rgb565ToArgb(g_fb.palette[g_labPx[cy * desk::kLabW + cx - desk::kCanvasW]])
                               : desk::rgb565ToArgb(fb.palette[fb.px[ry * pg::kScreenW + cx]]);
        uint8_t* o = &rgb[(size_t(y) * size_t(W) + size_t(x)) * 3];
        o[0] = uint8_t(v >> 16);
        o[1] = uint8_t(v >> 8);
        o[2] = uint8_t(v);
      }
    return uih::writePng(path.c_str(), rgb.data(), W, H);
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

  // ---- window title (project) and the strip's stats line ----------------------------
  void updateTitle(const desk::AudioStats& as) {
    char t[256];
    const int n = std::snprintf(t, sizeof(t), "Pocket Groove - %s%s", g_ui.projectName()[0] ? g_ui.projectName() : "?",
                                g_ui.dirty() ? "*" : "");
    if (!audioOk) std::snprintf(t + n, sizeof(t) - size_t(n), "   NO AUDIO");
    char st[64];
    if (!audioOk) std::snprintf(st, sizeof(st), "NO AUDIO OUTPUT");
    else
      std::snprintf(st, sizeof(st), "AUD %d/%d%% LATE %u GAP %u %dFR %dX%s", int(as.loadPct + 0.5f),
                    int(as.peakLoadPct + 0.5f), as.late, as.gaps, as.bufferFrames, display.pixelScale(),
                    opt.mono ? " M" : "");
    statsText = st;
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

  int command(const std::string& args, uint32_t now) override {
    std::vector<std::string> w;
    for (size_t i = 0; i < args.size();) {
      while (i < args.size() && args[i] == ' ') ++i;
      size_t j = i;
      while (j < args.size() && args[j] != ' ') ++j;
      if (j > i) w.push_back(args.substr(i, j - i));
      i = j;
    }
    if (w.empty()) return -1;
    auto pushClick = [&](int cx, int cy) {
      int wx = 0, wy = 0;
      display.fromCanvas(cx, cy, &wx, &wy);
      for (int down = 1; down >= 0; --down) {
        SDL_Event e;
        SDL_zero(e);
        e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.windowID = display.windowId();
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = wx;
        e.button.y = wy;
        SDL_PushEvent(&e);
      }
    };
    // pending until done, failing after the deadline (the argument at index msArg, default 10 s)
    auto waitFor = [&](bool done, const char* what, size_t msArg = 1) {
      if (done) { waiting = false; return 0; }
      if (!waiting) {
        waiting = true;
        waitUntil = now + uint32_t(w.size() > msArg ? std::atoi(w[msArg].c_str()) : 10000);
      }
      if (int32_t(now - waitUntil) >= 0) {
        waiting = false;
        std::fprintf(stderr, "[script] timed out waiting for %s\n", what);
        return -1;
      }
      return 1;
    };
    const std::string& c = w[0];
    if (c == "click" && w.size() == 3) {   // canvas device pixels
      pushClick(std::atoi(w[1].c_str()), std::atoi(w[2].c_str()));
      return 0;
    }
    if (c == "mouse" && w.size() == 3) {   // move the mouse (no button) to canvas device pixels: tilt with G0
      int wx = 0, wy = 0;
      display.fromCanvas(std::atoi(w[1].c_str()), std::atoi(w[2].c_str()), &wx, &wy);
      SDL_Event e;
      SDL_zero(e);
      e.type = SDL_MOUSEMOTION;
      e.motion.windowID = display.windowId();
      e.motion.x = wx;
      e.motion.y = wy;
      SDL_PushEvent(&e);
      return 0;
    }
    if (c == "tilt" && w.size() == 3) {   // ui_shot's `tilt X Y`: the mouse that far from the press point,
      // X * kTiltRangePx right and Y * kTiltRangePx up (with the default response "smooth": about that axis value)
      const int cx = tiltX0 + int(float(std::atof(w[1].c_str())) * kTiltRangePx);
      const int cy = tiltY0 - int(float(std::atof(w[2].c_str())) * kTiltRangePx);
      int wx = 0, wy = 0;
      display.fromCanvas(cx, cy, &wx, &wy);
      SDL_Event e;
      SDL_zero(e);
      e.type = SDL_MOUSEMOTION;
      e.motion.windowID = display.windowId();
      e.motion.x = wx;
      e.motion.y = wy;
      SDL_PushEvent(&e);
      return 0;
    }
    if (c == "print-tilt") {   // what the Ui and the engine have (the desktop tests)
      const pg::ui::Ui::TiltSpec ts = g_ui.tiltSpec();
      const pg::EngineStatus& st = g_ui.engineStatus();
      std::printf("[tilt] held %d engaged %d axes %.2f %.2f track %d macros %d %d heard %d %d base %d %d\n",
                  int(g_ui.tiltHeld()), int(g_ui.tiltEngaged()), double(g_ui.tiltAxis(0)), double(g_ui.tiltAxis(1)),
                  int(ts.track), int(ts.macro[0]), int(ts.macro[1]),
                  ts.macro[0] >= 0 ? int(st.macroPos[ts.track][ts.macro[0]]) : -1,
                  ts.macro[1] >= 0 ? int(st.macroPos[ts.track][ts.macro[1]]) : -1,
                  ts.macro[0] >= 0 ? g_ui.macroValue(ts.track, ts.macro[0]) : -1,
                  ts.macro[1] >= 0 ? g_ui.macroValue(ts.track, ts.macro[1]) : -1);
      std::fflush(stdout);
      return 0;
    }
    if (c == "click-button" && w.size() == 2) {
      for (int b = 0; b < desk::kNumButtons; ++b)
        if (w[1] == buttonName(b)) {
          int x = 0, y = 0;
          desk::stripButtonCentre(b, &x, &y);
          pushClick(x, pg::kScreenH + y);
          return 0;
        }
      return -1;
    }
    if (c == "winshot" && w.size() == 2) {
      mkdirAll(opt.outDir);
      uiFrame();
      updateStrip();
      if (lab.enabled()) lab.draw(g_labPx);
      if (stems.isOpen()) { stems.draw(g_overlayFb); display.uploadOverlay(g_overlayFb); }
      const std::string p1 = opt.outDir + "/" + w[1] + ".png", p4 = opt.outDir + "/" + w[1] + "_4x.png";
      const bool ok = windowShot(p1, 1) && windowShot(p4, 4);
      if (ok) std::printf("[winshot] %s\n", p1.c_str());
      return ok ? 0 : -1;
    }
    if (c == "listen-sim" && w.size() == 2 && simCapture) {
      simCapture->plug(w[1] == "plug");
      std::printf("[listen-sim] %s\n", w[1].c_str());
      listen.rescanSoon();
      return 0;
    }
    if (c == "drop" && w.size() == 2) {   // as a file dropped on the window
      dropFile(w[1]);
      return 0;
    }
    if (c == "print-samples") {   // the pool as the instrument sees it
      for (int i = 0; i < pg::sampler::kPoolSlots; ++i) {
        pg::sampler::SampleRef r;
        pg::sampler::resolve(samples.pool(), i, &r);
        std::printf("[samples] slot %d: %s %u %s\n", i + 1, r.builtin ? "BUILT-IN" : "USER", unsigned(r.length), r.name);
      }
      std::fflush(stdout);
      return 0;
    }
    if (c == "wait-stems") return waitFor(!stems.busy(), "the stems job");
    if (c == "wait-listening") return waitFor(listen.active() && g_listenBuf.stats().primed, "listening");
    if (c == "print-keys") {
      std::printf("[keys] %u key events reached the instrument\n", uiKeyEvents);
      return 0;
    }
    if (c == "print-toast") {   // the toast on the screen now ("" when none)
      std::printf("[toast] \"%s\"\n", g_ui.toastText());
      std::fflush(stdout);
      return 0;
    }
    if (c == "print-listen") {
      listen.printStats();
      return 0;
    }
    if (c.compare(0, 4, "lab-") == 0) return labCommand(w, pushClick, waitFor);
    return -1;
  }

  // `app lab-...` (docs/MACROLAB.md; host/app/tests/desktop_check.py, part "lab"):
  //   lab-select TRACK SET|none   choose a set (name, file name or path) on a track
  //   lab-drag M POS              drag macro M's slider (selected track) to POS, by mouse events
  //   lab-pad X Y                 click the XY pad at (X, Y), 0..1, by mouse events
  //   lab-click prev|next|good|bad|axis-x|axis-y   click a panel button
  //   lab-type TEXT...            type into the note (SDL text input events)
  //   lab-print TRACK             the lab's view of TRACK and the engine's parameter values
  //   lab-wait-synced [MS]        until the engine has every value the lab sent
  //   lab-wait-reloads N [MS]     until N files have been (re)loaded or removed since start
  template <class Click, class Wait>
  int labCommand(const std::vector<std::string>& w, Click pushClick, Wait waitFor) {
    if (!lab.enabled()) {
      std::fprintf(stderr, "[script] %s: start with --lab\n", w[0].c_str());
      return -1;
    }
    const std::string& c = w[0];
    auto trackArg = [&](const std::string& s) { return pg::macro::trackFromName(s.c_str()); };
    if (c == "lab-select" && w.size() == 3) return lab.selectSet(trackArg(w[1]), w[2]) ? 0 : -1;
    if (c == "lab-drag" && w.size() == 3) {
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      const int m = std::atoi(w[1].c_str());
      lab.sliderPoint(m, 0.0f, &x0, &y0);
      lab.sliderPoint(m, float(std::atof(w[2].c_str())), &x1, &y1);
      pushMouse(SDL_MOUSEBUTTONDOWN, desk::kCanvasW + x0, y0);
      pushMouse(SDL_MOUSEMOTION, desk::kCanvasW + (x0 + x1) / 2, y0);
      pushMouse(SDL_MOUSEMOTION, desk::kCanvasW + x1, y1);
      pushMouse(SDL_MOUSEBUTTONUP, desk::kCanvasW + x1, y1);
      return 0;
    }
    if (c == "lab-pad" && w.size() == 3) {
      int x = 0, y = 0;
      lab.padPoint(float(std::atof(w[1].c_str())), float(std::atof(w[2].c_str())), &x, &y);
      pushClick(desk::kCanvasW + x, y);
      return 0;
    }
    if (c == "lab-click" && w.size() == 2) {
      static const char* const names[] = {"prev", "next", "good", "bad", "axis-x", "axis-y"};
      for (int b = 0; b < 6; ++b)
        if (w[1] == names[b]) {
          int x = 0, y = 0;
          lab.buttonCentre(desk::Lab::Button(b), &x, &y);
          pushClick(desk::kCanvasW + x, y);
          return 0;
        }
      return -1;
    }
    if (c == "lab-type" && w.size() >= 2) {
      std::string text;
      for (size_t i = 1; i < w.size(); ++i) text += (i > 1 ? " " : "") + w[i];
      for (size_t i = 0; i < text.size(); i += 8) {   // SDL's text event holds up to 31 bytes
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_TEXTINPUT;
        e.text.windowID = display.windowId();
        std::snprintf(e.text.text, sizeof(e.text.text), "%s", text.substr(i, 8).c_str());
        SDL_PushEvent(&e);
      }
      return 0;
    }
    if (c == "lab-print" && w.size() == 2) {
      const int t = trackArg(w[1]);
      if (t < 0) return -1;
      std::printf("[lab-state] %s\n", lab.describe(t).c_str());
      std::string eng;
      uint8_t eff[pg::kParamsPerTrack];
      pg::engine_detail::effectiveParams(t, eff);   // what the voice gets (the mixer rule applied)
      for (int p = 0; p < pg::kParamsPerTrack; ++p)
        if (const pg::ParamInfo* in = g_inst.engine().trackParamInfo(t, p))
          eng += std::string(" ") + in->name + "=" + std::to_string(int(eff[p]));
      std::printf("[lab-engine] %s%s\n", pg::macro::trackName(t), eng.c_str());
      std::fflush(stdout);
      return 0;
    }
    if (c == "lab-wait-synced") return waitFor(lab.synced(), "the lab's values in the engine", 1);
    if (c == "lab-wait-reloads" && w.size() >= 2)
      return waitFor(lab.reloads() >= std::atoi(w[1].c_str()), "a macro file reload", 2);
    return -1;
  }
  // A file dropped on the window: into the selected track's sample slot (samples.h).
  void dropFile(const std::string& path) {
    const int t = g_ui.selTrack();
    const int slot = g_inst.project().tracks[t].sampleSlot;
    std::string msg;
    const bool ok = samples.drop(path, slot, &msg);
    std::printf("[samples] drop %s -> slot %d: %s\n", path.c_str(), slot + 1, msg.c_str());
    std::fflush(stdout);
    char toast[40];
    if (ok) {
      const pg::sampler::SlotHeader* h = samples.memoryPool().header(slot);
      char nm[pg::sampler::kNameLen + 1] = "";
      if (h) pg::sampler::headerName(*h, nm);
      std::snprintf(toast, sizeof toast, "SLOT %d: %s", slot + 1, nm);
    } else {
      std::snprintf(toast, sizeof toast, "SAMPLE: %.30s", msg.c_str());
    }
    g_ui.notify(toast, !ok);
    needPresent = true;
  }
  void samplesFrame(uint32_t now) {
    if (!samples.tick(now)) return;
    for (const std::string& m : samples.messages()) std::printf("[samples] %s\n", m.c_str());
    std::fflush(stdout);
  }
  void pushMouse(uint32_t type, int cx, int cy) {
    int wx = 0, wy = 0;
    display.fromCanvas(cx, cy, &wx, &wy);
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    if (type == SDL_MOUSEMOTION) {
      e.motion.windowID = display.windowId();
      e.motion.state = SDL_BUTTON_LMASK;
      e.motion.x = wx;
      e.motion.y = wy;
    } else {
      e.button.windowID = display.windowId();
      e.button.button = SDL_BUTTON_LEFT;
      e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
      e.button.clicks = 1;
      e.button.x = wx;
      e.button.y = wy;
    }
    SDL_PushEvent(&e);
  }

  static void mkdirAll(const std::string& d) { desk::makeDirs(d); }

  // ---- one UI frame -------------------------------------------------------------------
  void uiFrame() {
    // A dead microphone input while WAITING (mic.h): the toast, once per hold.
    const std::string silent = mic.takeSilentNotice();
    if (!silent.empty()) g_ui.notify(silent.c_str(), true);
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
  if (opt.listInputs) {   // the names --mic-device and --listen-device take, one per line
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
      std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
      return 1;
    }
    const std::vector<std::string> names = desk::SdlMicSource::inputNames();
    for (const std::string& n : names) std::printf("%s\n", n.c_str());
    if (names.empty()) std::fprintf(stderr, "no audio inputs (SDL audio driver %s)\n",
                                    SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "-");
    SDL_Quit();
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
  SDL_version v, built;
  SDL_GetVersion(&v);
  SDL_VERSION(&built);

  // Files: the projects directory is the device's /int/pg (dirfs.h).
  static desk::DirFs fs(opt.dir, opt.sdDir);
  app.fs = &fs;
  if (!fs.createMounts()) {
    std::fprintf(stderr, "cannot create %s%s%s\n", opt.dir.c_str(), opt.sdDir.empty() ? "" : " or ", opt.sdDir.c_str());
    SDL_Quit();
    return 1;
  }

  char err[200] = "";
  if (!app.display.open(opt.scale, err, sizeof(err), opt.lab)) {
    std::fprintf(stderr, "%s\n", err);
    SDL_Quit();
    return 1;
  }

  // Boot as the firmware's setup(): instrument, UI, session or demo 0, then audio last.
  pg::ui::initPalette(g_fb);
  std::memset(g_fb.px, 0, sizeof(g_fb.px));
  pg::ui::initPalette(g_stripFb);
  pg::ui::initPalette(g_overlayFb);
  g_inst.init(float(opt.sr), opt.mono);
  // The sample pool: samples/ next to the projects directory (before the audio starts).
  {
    std::string sdir = opt.samplesDir;
    if (sdir.empty()) {
      std::string d = opt.dir;
      while (d.size() > 1 && d.back() == '/') d.pop_back();
      const size_t sl = d.find_last_of('/');
      sdir = sl == std::string::npos ? std::string("samples") : d.substr(0, sl + 1) + "samples";
    }
    if (app.samples.init(sdir)) {
      g_inst.setSamplePool(app.samples.pool());
      app.micSource.setDevice(opt.micDevice);
      app.mic.init(&app.samples, &app.micSource);
      desk::Samples* smp = &app.samples;
      app.resample.init(
          &g_inst.engine(), &g_inst.sampler(), float(opt.sr),
          [smp](int slot, const int16_t* d, uint32_t n, std::string* err) {
            return smp->putRecording(slot, d, n, err, "res");
          },
          [smp](int slot) {
            std::string msg;
            return smp->clearSlot(slot, &msg);
          });
      app.resample.log = true;
      int n = 0;
      for (int i = 0; i < pg::sampler::kPoolSlots; ++i) n += app.samples.fileOf(i).empty() ? 0 : 1;
      std::printf("[samples] %s: %d slot(s) with a file\n", sdir.c_str(), n);
      for (const std::string& m : app.samples.messages()) std::printf("[samples] %s\n", m.c_str());
    } else {
      std::printf("[samples] cannot create %s: the built-in sounds only\n", sdir.c_str());
    }
  }
  g_ui.init(g_inst.engine(), g_inst.project(), &fs);
  g_ui.setSampler(&g_inst.sampler());
  g_ui.setMelodic(&g_inst.melodic());
  g_ui.setMic(&app.mic);                 // L on a sampler page records the Mac's microphone
  g_ui.setResample(&app.resample);       // SHIFT+L resamples what it plays (docs/DESKTOP.md "Resampling")
  g_ui.setGhostFilter(opt.deviceKeys);   // a computer keyboard has no phantoms to filter
  g_ui.setDevMode(opt.dev || opt.lab);
  const bool hadSession = fs.exists(pg::io::kSessionPath);
  g_ui.boot(app.nowMs());
  std::printf("[boot] SDL %d.%d.%d (%s, %s), projects in %s%s%s, %s session, project \"%s\"\n", v.major, v.minor,
              v.patch, SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "-",
              opt.dir.c_str(), opt.sdDir.empty() ? "" : ", SD card ", opt.sdDir.c_str(),
              hadSession ? "resumed the" : "no", g_ui.projectName());

  if (opt.lab) {
    pg::macro::ParamTable pt;
    for (int t = 0; t < pg::kNumTracks; ++t)
      for (int p = 0; p < pg::kParamsPerTrack; ++p) pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
    app.lab.init(opt.labDir, opt.labNotes, pt, &app, app.nowMs());
  }

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
  // Listen: SDL's capture list, or the test's simulated device.
  desk::CaptureSource* capture = &g_sdlCapture;
  if (opt.listenSim) {
    app.simCapture.reset(new desk::SimCapture(&g_audio, opt.listenSimPpm));
    if (opt.listenSimSpec) app.simCapture->setSpec(opt.listenSimSpecV, opt.listenSimBurst, opt.listenSimVary);
    capture = app.simCapture.get();
  }
  g_sdlCapture.setWanted(opt.listenDevice);
  app.listen.init(capture, &g_listenBuf, &g_listenSink, &g_audio, opt.sr);
  app.listen.setDumpPath(opt.listenDump);
  {
    // sdl2-compat (Homebrew's "sdl2": the SDL2 API on SDL3) reports 2.32.50 and up; SDL2 itself
    // never got past 2.32.10 (sdl2-compat: "way ahead of what SDL2 Classic would report").
    const bool compat = v.major == 2 && (v.minor > 32 || (v.minor == 32 && v.patch >= 50));
    char info[320];
    std::snprintf(info, sizeof(info),
                  "SDL built against %d.%d.%d, running %d.%d.%d = %s, revision \"%s\", audio driver %s",
                  built.major, built.minor, built.patch, v.major, v.minor, v.patch,
                  compat ? "sdl2-compat (SDL2 API on SDL3)" : "SDL2 itself", SDL_GetRevision(),
                  SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "-");
    app.listen.setSdlInfo(info);
  }
  {
    // The microphone (hold L): which input it would use now (it chooses again at each hold).
    std::vector<std::string> inputs;
    const desk::MicChoice mc = app.micSource.choose(&inputs);
    std::printf("%s\n", desk::micInputsLine(inputs, mc).c_str());
  }
  // Stems: tools/pg of this repository (build/host/pocket-groove -> ../../tools/pg).
  {
    desk::StemsConfig sc;
    sc.tool = opt.pgTool;
    if (sc.tool.empty() && std::getenv("PG_TOOL")) sc.tool = std::getenv("PG_TOOL");
    if (sc.tool.empty()) {
      if (char* base = SDL_GetBasePath()) {
        const std::string t = std::string(base) + "../../tools/pg";
        SDL_free(base);
        if (::access(t.c_str(), X_OK) == 0) sc.tool = t;
      }
    }
    if (sc.tool.empty()) sc.tool = "tools/pg";
    sc.port = opt.pgPort;
    sc.projectsDir = opt.dir;
#ifdef __APPLE__
    sc.reveal = "open";
#else
    sc.reveal = "xdg-open";
#endif
    if (const char* r = std::getenv("PG_REVEAL")) sc.reveal = r;   // "" = do not open the folder
    app.stems.init(sc);
  }
  std::fflush(stdout);

  uint32_t nextFrame = app.nowMs();
  uint32_t lastTitle = 0;
  bool scriptDone = !scripted;
  while (app.running) {
    // Wait for input until the next UI frame; keys go to the Ui the moment they arrive.
    int32_t rem = int32_t(nextFrame - app.nowMs());
    if (g_ui.tiltHeld() && int32_t(app.tiltNext - app.nowMs()) < rem) rem = int32_t(app.tiltNext - app.nowMs());
    SDL_Event e;
    bool got = rem > 0 ? SDL_WaitEventTimeout(&e, rem > 0 ? rem : 0) != 0 : SDL_PollEvent(&e) != 0;
    while (got) {
      switch (e.type) {
        case SDL_QUIT: app.running = false; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: app.onSdlKey(e.key); break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT) app.click(e.button.x, e.button.y);
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT && app.labMouse) {
            app.labMouse = false;
            app.lab.mouseUp();
          }
          break;
        case SDL_MOUSEMOTION:
          app.mouseX = e.motion.x;
          app.mouseY = e.motion.y;
          if (app.labMouse) {
            int cx = 0, cy = 0;
            app.display.toCanvasAny(e.motion.x, e.motion.y, &cx, &cy);
            app.lab.mouseMove(cx - desk::kCanvasW, cy);
          }
          break;
        case SDL_TEXTINPUT:
          if (app.lab.typing()) app.lab.typeText(e.text.text);
          break;
        case SDL_DROPFILE:
          if (e.drop.file) {
            app.dropFile(e.drop.file);
            SDL_free(e.drop.file);
          }
          break;
        case SDL_AUDIODEVICEADDED:
        case SDL_AUDIODEVICEREMOVED:
          if (app.simCapture == nullptr)
            g_sdlCapture.deviceEvent(e.type == SDL_AUDIODEVICEADDED, e.adevice.iscapture != 0, e.adevice.which);
          app.listen.rescanSoon();
          break;
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

    app.tiltService(app.nowMs());
    const uint32_t now = app.nowMs();
    if (int32_t(now - nextFrame) >= 0) {
      app.labFrame(now);   // the lab's values go out before the frame, so the SOUND page shows them
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
    if (app.audioOk) app.listen.tick(now);
    app.stems.poll();
    app.samplesFrame(now);
    app.updateStrip();
    app.updateOverlay();
    if (app.needPresent) {
      app.display.present(app.stems.isOpen());
      app.needPresent = false;
    }
  }

  // Quit (window closed, Cmd+Q, Ctrl+C in the terminal): the device's save path.
  app.lab.finishTyping(false);   // a mark waiting for its note is written without one
  app.releaseAll();
  if (app.rec.active()) app.toggleRecord();
  app.stems.shutdown();
  app.listen.shutdown();
  std::printf("[keys] %u key events reached the instrument\n", app.uiKeyEvents);
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
