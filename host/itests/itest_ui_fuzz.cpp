// Random-key fuzz of the UI with the engine running: hundreds of thousands of key
// events, random timing, random holds. Asserts no crash, framebuffer writes stay
// inside the buffer (guard bytes) and only use defined palette entries, and the
// UI's own state stays in range. Also built with ASan/UBSan (see the report).
//   itest_ui_fuzz [events] [seed]
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/ui_harness.h"

struct Guarded {
  uint8_t pre[4096];
  pg::Framebuffer fb;
  uint8_t post[4096];
};

int main(int argc, char** argv) {
  const long events = argc > 1 ? std::atol(argv[1]) : 300000;
  uint32_t rng = argc > 2 ? uint32_t(std::atol(argv[2])) : 12345u;
  auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
  static uih::Rig rig;
  static Guarded g;
  std::memset(g.pre, 0xA5, sizeof(g.pre));
  std::memset(g.post, 0x5A, sizeof(g.post));
  rig.start(true);
  pg::ui::initPalette(g.fb);
  rig.target = &g.fb;
  bool held[pg::ui::kNumKeyIds] = {};
  long fails = 0;
  // Bias towards the keys that matter (steps, pads, notes, -/=, modifiers) but hit all of them.
  for (long i = 0; i < events; ++i) {
    int k;
    const uint32_t r = rnd() % 100;
    if (r < 10) k = int(rnd() % pg::ui::kNumKeyIds);
    else if (r < 30) k = pg::ui::stepKeyIndex(0) >= 0 ? (int(rnd() % 16) < 8 ? 1 + int(rnd() % 8) : 15 + int(rnd() % 8)) : 1;
    else if (r < 45) k = pg::ui::kKPad0 + int(rnd() % 8);
    else if (r < 58) k = pg::ui::kKNote0 + int(rnd() % 8);
    else if (r < 70) k = (rnd() & 1) ? pg::ui::kKMinus : pg::ui::kKEquals;
    else if (r < 82) {
      const uint8_t mods[] = {pg::ui::kKFn, pg::ui::kKShift, pg::ui::kKCtrl, pg::ui::kKOpt, pg::ui::kKAlt, pg::ui::kKTab};
      k = mods[rnd() % 6];
    } else {
      const uint8_t misc[] = {pg::ui::kK9, pg::ui::kK0, pg::ui::kKO, pg::ui::kKP, pg::ui::kKBackslash,
                              pg::ui::kKBacktick, pg::ui::kKSpace, pg::ui::kKEnter, pg::ui::kKDel,
                              pg::ui::kKSemicolon, pg::ui::kKDot, pg::ui::kKQuote, pg::ui::kKL,
                              pg::ui::kKLBracket, pg::ui::kKRBracket, pg::ui::kKSlash};
      k = misc[rnd() % sizeof(misc)];
    }
    // Keep the transport mostly running and avoid huge numbers of held keys.
    rig.key(k, !held[k]);
    held[k] = !held[k];
    if (rnd() % 64 == 0)
      for (int j = 0; j < pg::ui::kNumKeyIds; ++j)
        if (held[j]) { rig.key(j, false); held[j] = false; }
    if (rnd() % 4 == 0) rig.ui.onKey(pg::KeyEvent{uint8_t(rnd() % 70), (rnd() & 1) != 0, 0}, rig.now());  // junk ids, stray ups
    const uint32_t wait = (rnd() % 128 == 0) ? rnd() % 500 : rnd() % 4;   // mean ~3.5 ms: holds, repeats and sweeps all happen
    rig.run(wait);
    if (i % 4096 == 0) {
      rig.frame();
      for (size_t j = 0; j < sizeof(g.pre); ++j)
        if (g.pre[j] != 0xA5 || g.post[j] != 0x5A) { ++fails; std::printf("guard bytes overwritten at event %ld\n", i); break; }
      for (int j = 0; j < pg::kScreenW * pg::kScreenH; ++j)
        if (g.fb.px[j] >= 48) { ++fails; std::printf("undefined colour %d at event %ld\n", g.fb.px[j], i); break; }
      const pg::ui::Ui& u = rig.ui;
      if (u.selTrack() > 7 || u.heldStep() >= 64 || u.shownPage() > 3 || u.stepCursor() >= 64 ||
          u.focus().index >= 32 || (!u.focus().global && u.focus().track > 7)) {
        ++fails;
        std::printf("UI state out of range at event %ld\n", i);
      }
      for (int p = 0; p < pg::kNumPatterns; ++p)
        for (int t = 0; t < 8; ++t) {
          const int len = rig.proj->patterns[p].tracks[t].length;
          if (len < 1 || len > 64) { ++fails; std::printf("bad length\n"); }
        }
      if (fails > 10) break;
    }
  }
  std::printf("itest_ui_fuzz: %ld events, %u full redraws, %u status redraws, %u dropped commands, %ld failures\n",
              events, rig.ui.fullRedraws(), rig.ui.statusRedraws(), rig.ui.droppedCommands(), fails);
  return fails ? 1 : 0;
}
