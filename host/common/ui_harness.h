// Host harness for the UI: the real instrument (voices + engine), a Project,
// the Ui, a framebuffer and a simulated clock. Used by host/tools/ui_shot.cpp
// and host/itests/itest_ui_*.cpp. Desktop only (uses stdio / heap freely).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "common/ghost_matrix.h"
#include "common/memfs.h"
#include "pg/demo/demo.h"
#include "pg/engine.h"
#include "pg/instrument/instrument.h"
#include "pg/ui/gfx.h"
#include "pg/ui/keys.h"
#include "pg/ui/ui.h"

namespace uih {

// ---- the mockups' project (screens.demo_pattern + DEMO_SOUND/GLOBALS), used by the
// screenshot scripts and the tests; independent of the ROM demos, which a composer owns ----
inline void stepOn(pg::Step& s, int vel, int note = 60, bool acc = false, bool slide = false, int prob = 100,
                   int ratch = 1, int chord = 0, int gate = 2) {
  s = pg::kDefaultStep;
  s.flags = uint8_t(pg::kStepOn | (acc ? pg::kStepAccent : 0) | (slide ? pg::kStepSlide : 0) |
                    ((ratch - 1) << pg::kStepRatchetShift));
  s.vel = uint8_t(vel);
  s.note = uint8_t(note);
  s.prob = uint8_t(prob);
  s.chord = uint8_t(chord);
  s.gate = uint8_t(gate);
}

inline void buildMockup(pg::Project& p, const pg::Engine& eng) {
  using namespace pg;
  eng.initProject(p);
  std::memcpy(p.name, "ACIDJAM", 8);
  Pattern& P = p.patterns[0];
  auto st = [&](int t, int i) -> Step& { return P.tracks[t].steps[i]; };
  for (int i : {0, 4, 8, 12}) stepOn(st(0, i), 110, 60, i == 0);
  stepOn(st(1, 12), 96);
  stepOn(st(1, 15), 60, 60, false, false, 50);
  stepOn(st(1, 7), 50, 60, false, false, 40);
  for (int i : {4, 12}) stepOn(st(2, i), 105);
  const int hv[4] = {100, 55, 80, 55};
  for (int i = 0; i < 16; ++i) stepOn(st(3, i), hv[i % 4]);
  stepOn(st(3, 15), 80, 60, false, false, 100, 2);
  for (int i : {2, 6, 10, 14}) stepOn(st(4, i), 95);
  stepOn(st(5, 3), 80);
  stepOn(st(5, 9), 70, 60, false, false, 60);
  stepOn(st(5, 14), 85, 63);
  const int A1 = 33;
  stepOn(st(6, 0), 100, A1, true);
  stepOn(st(6, 2), 100, A1);
  stepOn(st(6, 3), 100, A1 + 12, false, true);
  stepOn(st(6, 5), 100, A1 + 3);
  stepOn(st(6, 6), 100, A1);
  stepOn(st(6, 8), 100, A1, true);
  stepOn(st(6, 10), 100, A1 - 2);
  stepOn(st(6, 11), 100, A1, false, true);
  stepOn(st(6, 13), 100, A1 + 7, true);
  st(6, 13).lockParam = 1;   // CUT
  st(6, 13).lockValue = 110;
  stepOn(st(6, 14), 100, A1 + 5);
  stepOn(st(7, 2), 100, 57, false, false, 100, 1, kChordMin7, 3);
  stepOn(st(7, 7), 100, 57, false, false, 100, 1, kChordMin7, 2);
  stepOn(st(7, 10), 100, 53, false, false, 100, 1, kChordMaj7, 6);
  uint8_t* b = p.tracks[kTrackBass].params;
  b[0] = 58; b[1] = 96; b[2] = 84; b[3] = 40; b[4] = 100; b[5] = 52; b[8] = 0; b[9] = 70; b[13] = 24;
  uint8_t* l = p.tracks[kTrackLead].params;
  l[0] = 70; l[1] = 24; l[4] = 3; l[6] = 64; l[11] = 60;
  p.globals[kGTempo] = 124;
  p.globals[kGSwing] = 8;
  p.globals[kGDuckDepth] = 100;
  p.globals[kGDuckHold] = 30;
  p.globals[kGDuckRelease] = 70;
  p.globals[kGDuckCurve] = 80;
  p.globals[kGReverbSize] = 70;
  p.globals[kGMasterDrive] = 20;
  p.globals[kGScaleRoot] = 9;
  p.globals[kGScaleType] = 2;
}

// ---- minimal PNG writer (stored deflate blocks, no compression) ----
inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c;
}
inline void be32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, uint32_t(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
inline bool writePng(const char* path, const uint8_t* rgb, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    const size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n & 0xFF));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n & 0xFF));
    z.push_back(uint8_t((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
    pos += n;
    if (last) break;
  }
  uint32_t a = 1, b2 = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b2 = (b2 + a) % 65521; }
  be32(z, (b2 << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  be32(ihdr, uint32_t(w));
  be32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return true;
}

// Framebuffer -> RGB888 the way the mockups expand RGB565 (pgfb.from565), at `scale`.
inline bool writeFramebufferPng(const char* path, const pg::Framebuffer& fb, int scale) {
  const int W = pg::kScreenW * scale, H = pg::kScreenH * scale;
  std::vector<uint8_t> rgb(size_t(W) * H * 3);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint16_t v = fb.palette[fb.px[(y / scale) * pg::kScreenW + x / scale]];
      const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
      uint8_t* o = &rgb[(size_t(y) * W + x) * 3];
      o[0] = uint8_t((r << 3) | (r >> 2));
      o[1] = uint8_t((g << 2) | (g >> 4));
      o[2] = uint8_t((b << 3) | (b >> 2));
    }
  return writePng(path, rgb.data(), W, H);
}

// ---- the rig: pg::Instrument + Ui + in-memory file system + simulated clock ----
struct Rig {
  static constexpr float kSr = 44100.0f;
  static constexpr int kBlock = 64;

  pg::Instrument& inst;
  pg::Engine& eng;
  pg::ui::Ui ui;
  pgtest::MemFs fs;
  pg::Framebuffer fb;
  pg::Framebuffer* target = &fb;   // where frame() draws (the fuzz test points it at a guarded buffer)
  pg::Project* proj;
  pg::ui::UiEnv env{0, 80, true, 30};
  // Every key() goes through the diode-less matrix model: the UI sees the
  // phantoms the device would report. Tests set the order (or Off) per run.
  GhostMatrix matrix;
  uint64_t samples = 0;
  uint32_t frames = 0;
  bool engineRuns = true;
  // Developer mode (Ui::setDevMode) for the next start() / boot(). The tests written
  // before instruments drive the parameter pages of CLASSIC tracks, which only
  // developer mode shows; the instrument tests turn it off.
  bool devMode = true;
  int16_t audio[kBlock * 2];

  static pg::Instrument& instrument() {
    static pg::Instrument i;   // ~70 KB: static, never on the stack
    return i;
  }
  Rig() : inst(instrument()), eng(instrument().engine()), proj(&instrument().project()) {}

  void resetClock() { samples = 0; frames = 0; }

  // Fresh instrument; demo = the mockups' project (buildMockup), else an empty project.
  // The file system is wiped unless keepFs. No boot sequence (see boot()).
  void start(bool demo, bool keepFs = false) {
    if (!keepFs) { fs = pgtest::MemFs(); }
    inst.init(kSr, false);
    if (demo) {
      eng.suspend();
      buildMockup(*proj, eng);
      eng.sanitizeProject(*proj);
      eng.resume();
    }
    pg::ui::initPalette(fb);
    std::memset(fb.px, 0, sizeof(fb.px));
    matrix.reset();
    ui.init(eng, *proj, &fs);
    ui.setDevMode(devMode);
    resetClock();
    frame();
  }
  // Power-on with the current file system contents: what the firmware does.
  void boot() {
    inst.init(kSr, false);
    pg::ui::initPalette(fb);
    matrix.reset();
    ui.init(eng, *proj, &fs);
    ui.setDevMode(devMode);
    resetClock();
    ui.boot(now());
    frame();
  }

  uint32_t now() const { return uint32_t(samples * 1000 / uint64_t(kSr)); }

  bool frame() {
    env.nowMs = now();
    return ui.update(env, *target);
  }

  // Advance the clock by ms: audio in 64-frame blocks, a UI frame every 33.3 ms.
  void run(uint32_t ms) {
    const uint64_t end = samples + uint64_t(ms) * uint64_t(kSr) / 1000;
    while (samples < end) {
      if (engineRuns) inst.process(audio, kBlock);
      samples += kBlock;
      if (uint64_t(now()) * 30 >= uint64_t(frames + 1) * 1000) {
        ++frames;
        frame();
      }
    }
  }

  // A physical press / release. Matrix keys go through the ghost model (one
  // controller batch: same timestamp); G0 and junk ids go straight to the UI.
  void key(int k, bool down) {
    const uint32_t t = uint32_t(samples * 1000000 / uint64_t(kSr));
    if (k < 0 || k >= pg::kNumKeys) {
      ui.onKey(pg::KeyEvent{uint8_t(k), down, t}, now());
      return;
    }
    pg::KeyEvent ev[pg::kNumKeys];
    const int n = matrix.change(k, down, t, ev);
    for (int i = 0; i < n; ++i) ui.onKey(ev[i], now());
  }
  // Releases every physically held key (lowest id first).
  void releaseAll() {
    for (int k = 0; k < pg::kNumKeys; ++k)
      if ((matrix.phys >> k) & 1) key(k, false);
  }
  void down(int k) { key(k, true); }
  void up(int k) { key(k, false); }
  void tap(int k, uint32_t holdMs = 40) {
    down(k);
    run(holdMs);
    up(k);
  }
  void downName(const char* n) { down(pg::ui::keyFromName(n)); }
  void upName(const char* n) { up(pg::ui::keyFromName(n)); }

  // Run until track t's playhead is at step n (max maxMs); returns success.
  bool runUntilStep(int t, int n, uint32_t maxMs = 20000) {
    for (uint32_t waited = 0; waited < maxMs; waited += 1) {
      if (eng.status().step[t] == n) { frame(); return true; }
      run(1);
    }
    return false;
  }

  const pg::Step& step(int t, int i, int pat = -1) const {
    return proj->patterns[pat < 0 ? ui.editPattern() : pat].tracks[t].steps[i];
  }
};

}  // namespace uih
