// Random-key fuzz of the UI with the engine running: hundreds of thousands of key
// events, random timing, random holds. Asserts no crash, framebuffer writes stay
// inside the buffer (guard bytes) and only use defined palette entries, and the
// UI's own state stays in range. Also built with ASan/UBSan (see the report).
// Keys go through the diode-less matrix model (common/ghost_matrix.h), half of
// the events with the phantoms last in their batch and half with them first;
// whenever every key has been released, no key may still be down in the UI.
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
  rig.boot();                         // the full power-on path, with the in-memory file system
  pg::ui::initPalette(g.fb);
  rig.target = &g.fb;
  bool held[pg::ui::kNumKeyIds] = {};
  bool junkHeld[pg::ui::kNumKeyIds] = {};
  long fails = 0, releaseChecks = 0;
  long seenView[6] = {}, seenDialog = 0, seenExplain = 0, seenHelp = 0, samples = 0;
  uint32_t fullTotal = 0;
  // Bias towards the keys that matter (steps, pads, notes, -/=, modifiers) but hit all of them.
  for (long i = 0; i < events; ++i) {
    rig.matrix.order = i < events / 2 ? uih::GhostOrder::PhantomLast : uih::GhostOrder::PhantomFirst;
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
      const uint8_t misc[] = {pg::ui::kKNote0, pg::ui::kKNote0 + 1, pg::ui::kKNote0 + 2, pg::ui::kKNote0 + 3,
                              pg::ui::kKPad0 + 1, pg::ui::kKNote0 + 5,   // with CTRL: undo/cut/copy/paste/save/new
                              pg::ui::kK9, pg::ui::kK0, pg::ui::kKO, pg::ui::kKP, pg::ui::kKBackslash,
                              pg::ui::kKBacktick, pg::ui::kKSpace, pg::ui::kKEnter, pg::ui::kKDel,
                              pg::ui::kKSemicolon, pg::ui::kKDot, pg::ui::kKQuote, pg::ui::kKL,
                              pg::ui::kKLBracket, pg::ui::kKRBracket, pg::ui::kKSlash};
      k = misc[rnd() % sizeof(misc)];
    }
    // Keep the transport mostly running and avoid huge numbers of held keys.
    rig.key(k, !held[k]);
    held[k] = !held[k];
    if (rnd() % 64 == 0) {
      for (int j = 0; j < pg::ui::kNumKeyIds; ++j)
        if (held[j]) { rig.key(j, false); held[j] = false; }
      for (int j = 0; j < pg::ui::kNumKeyIds; ++j)
        if (junkHeld[j]) { rig.ui.onKey(pg::KeyEvent{uint8_t(j), false, 0}, rig.now()); junkHeld[j] = false; }
      rig.run(5);                     // a frame: the ghost filter lets go of anything held back
      rig.frame();
      ++releaseChecks;
      const pg::ui::GhostFilter& gf = rig.ui.ghostFilter();
      if (rig.ui.heldKeys() != 0 || gf.holding() || gf.realMask() != 0 || gf.phantomMask() != 0) {
        ++fails;
        std::printf("keys still down after releasing all at event %ld: ui %llx real %llx phantom %llx\n", i,
                    (unsigned long long)rig.ui.heldKeys(), (unsigned long long)gf.realMask(),
                    (unsigned long long)gf.phantomMask());
      }
    }
    if (rnd() % 4 == 0) {             // junk ids, stray downs and ups, straight to the UI
      const pg::KeyEvent je{uint8_t(rnd() % 70), (rnd() & 1) != 0, 0};
      rig.ui.onKey(je, rig.now());
      if (je.key < pg::ui::kNumKeyIds) junkHeld[je.key] = je.down;
    }
    // storage chaos: faults, a full disk, the SD card coming and going, power cuts
    if (rnd() % 512 == 0) {
      rig.fs.arm();
      rig.fs.failOp = int(rnd() % 40);
      if (rnd() & 1) rig.fs.readFailCall = int(rnd() % 80);
    }
    if (rnd() % 2048 == 0) rig.fs.clearFaults();
    if (rnd() % 1024 == 0) rig.fs.spaceLimit[rnd() & 1] = (rnd() & 1) ? -1 : long(rnd() % 20000);
    if (rnd() % 1024 == 0) { rig.fs.sdPresent = !rig.fs.sdPresent; rig.env.sdPresent = rig.fs.sdPresent; }
    if (rnd() % 4096 == 0) rig.fs.powerLossOp = int(rnd() % 30);
    if (rig.fs.dead && rnd() % 8 == 0) rig.fs.powerCycle();
    if (rnd() % 8192 == 0) {           // reboot with whatever the file system holds now
      for (int j = 0; j < pg::ui::kNumKeyIds; ++j) held[j] = junkHeld[j] = false;
      rig.fs.powerCycle();
      fullTotal += rig.ui.fullRedraws();
      rig.devMode = (rnd() & 1) != 0;   // the player's SOUND page and the developer's parameter pages
      rig.boot();
      rig.target = &g.fb;
    }
    if (rnd() % 256 == 0) rig.env.batteryPct = uint8_t(rnd() % 101);
    const uint32_t wait = (rnd() % 128 == 0) ? rnd() % 500 : rnd() % 4;   // mean ~3.5 ms: holds, repeats and sweeps all happen
    rig.run(wait);
    if (i % 64 == 0) {
      ++samples;
      ++seenView[int(rig.ui.view())];
      seenDialog += rig.ui.dialog() != pg::ui::Dialog::None;
      seenExplain += rig.ui.explainMode();
      seenHelp += rig.ui.helpVisible();
    }
    if (i % 4096 == 0) {
      rig.frame();
      for (size_t j = 0; j < sizeof(g.pre); ++j)
        if (g.pre[j] != 0xA5 || g.post[j] != 0x5A) { ++fails; std::printf("guard bytes overwritten at event %ld\n", i); break; }
      for (int j = 0; j < pg::kScreenW * pg::kScreenH; ++j)
        if (g.fb.px[j] >= 48) { ++fails; std::printf("undefined colour %d at event %ld\n", g.fb.px[j], i); break; }
      const pg::ui::Ui& u = rig.ui;
      if (u.songCursor() > 32 || u.projectCursor() > pg::ui::kMaxFileRows || u.fileRows() > pg::ui::kMaxFileRows ||
          u.selTrack() > 7 || u.heldStep() >= 64 || u.shownPage() > 3 || u.stepCursor() >= 64 ||
          u.focus().index >= 36 || (!u.focus().global && u.focus().track > 7)) {
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
  std::printf("itest_ui_fuzz: %ld events, %u full redraws, %u dropped commands, %ld failures\n", events,
              fullTotal + rig.ui.fullRedraws(), rig.ui.droppedCommands(), fails);
  std::printf("  matrix model: %u phantom downs, %u phantom ups injected; %ld all-released checks\n",
              rig.matrix.phantomDowns, rig.matrix.phantomUps, releaseChecks);
  if (rig.matrix.phantomDowns == 0 || releaseChecks == 0) { std::printf("FAIL: the ghost model never fired\n"); fails++; }
  std::printf("  time share: GROOVE %ld%% SOUND %ld%% MIX %ld%% FX %ld%% SONG %ld%% PROJECT %ld%%, dialog %ld%%, explain %ld%%, help %ld%%\n",
              seenView[0] * 100 / samples, seenView[1] * 100 / samples, seenView[2] * 100 / samples,
              seenView[3] * 100 / samples, seenView[4] * 100 / samples, seenView[5] * 100 / samples,
              seenDialog * 100 / samples, seenExplain * 100 / samples, seenHelp * 100 / samples);
  return fails ? 1 : 0;
}
