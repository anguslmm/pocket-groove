// Phantom keys (UX.md 16.1, docs/ghosting-audit.md): every gesture of the audit
// that holds three keys, driven through the diode-less matrix model with the
// phantom both last and first in its batch. Each test checks that the intended
// action happens and the phantom's would-be action does not, and that no
// release order leaves a key down.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/ui_harness.h"

using namespace pg;
using namespace pg::ui;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fail;                                                                  \
      std::printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond,           \
                  uih::ghostOrderName(R->matrix.order));                         \
    }                                                                            \
  } while (0)
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    long _a = long(a), _b = long(b);                                                                \
    if (_a != _b) {                                                                                 \
      ++g_fail;                                                                                     \
      std::printf("FAIL %s:%d: %s == %s (%ld vs %ld) [%s]\n", __FILE__, __LINE__, #a, #b, _a, _b,  \
                  uih::ghostOrderName(R->matrix.order));                                            \
    }                                                                                               \
  } while (0)

static uih::Rig* R;
static int K(const char* n) { return keyFromName(n); }
static void down(const char* n, uint32_t ms = 30) { R->down(K(n)); R->run(ms); }
static void up(const char* n, uint32_t ms = 30) { R->up(K(n)); R->run(ms); }
static void tap(const char* n) { R->tap(K(n), 40); R->run(20); }
static const Step& S(int t, int i) { return R->step(t, i); }
static int G(int g) { return R->proj->globals[g]; }
static uint32_t drops() { return R->ui.ghostFilter().droppedDowns(); }
static bool clean() {
  const GhostFilter& f = R->ui.ghostFilter();
  return R->ui.heldKeys() == 0 && !f.holding() && f.realMask() == 0 && f.phantomMask() == 0;
}
static int stepsOn(int pat) {
  int n = 0;
  for (int t = 0; t < kNumTracks; ++t)
    for (int i = 0; i < kMaxSteps; ++i) n += (R->proj->patterns[pat].tracks[t].steps[i].flags & kStepOn) != 0;
  return n;
}
// The phantom of a three-key chord (scan matrix), or -1.
static int phantomOf(const char* a, const char* b, const char* c) {
  const uint64_t p = (1ull << K(a)) | (1ull << K(b)) | (1ull << K(c));
  const uint64_t g = uih::GhostMatrix::closure(p) & ~p;
  return g ? __builtin_ctzll(g) : -1;
}

// ---- the model itself ------------------------------------------------------------

static void testModel() {
  // the scan matrix is not the printed layout
  CHECK_EQ(phantomOf("a", "s", "d"), K("f"));
  CHECK_EQ(phantomOf("q", "w", "e"), K("tab"));
  CHECK_EQ(phantomOf("z", "x", "c"), K("alt"));
  CHECK_EQ(phantomOf("ctl", "sft", "x"), K("f"));
  CHECK_EQ(phantomOf("q", "w", "r"), -1);
  // two independent implementations agree (union-find in the UI, flood fill here)
  uint32_t s = 7;
  for (int i = 0; i < 20000; ++i) {
    uint64_t m = 0;
    const int n = 1 + int((s = s * 1664525u + 1013904223u) >> 29);
    for (int j = 0; j < n; ++j) m |= 1ull << ((s = s * 1664525u + 1013904223u) >> 8) % kNumKeys;
    CHECK(ghostClosure(m) == uih::GhostMatrix::closure(m));
  }
  // the filter alone: a phantom that comes in a later batch (outside the window)
  GhostFilter f;
  GhostFilter::Out out[GhostFilter::kMaxOut];
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("a")), true, 0}, 0, out), 1);
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("s")), true, 0}, 0, out), 1);
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("d")), true, 100000}, 100, out), 0);   // held back: f is expected
  CHECK(f.holding());
  CHECK_EQ(f.tick(101, out), 0);                                            // not yet
  CHECK_EQ(f.tick(105, out), 1);                                            // no partner came: real
  CHECK_EQ(out[0].ev.key, K("d"));
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("f")), true, 110000}, 110, out), 0);   // completes a loop: phantom
  CHECK_EQ(f.droppedDowns(), 1);
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("f")), false, 120000}, 120, out), 0);  // its up is swallowed
  CHECK_EQ(f.swallowedUps(), 1);
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("d")), false, 130000}, 130, out), 1);
  // a "phantom" whose up got lost: the next down for it counts as a fresh key
  f.reset();
  f.push(KeyEvent{uint8_t(K("a")), true, 0}, 0, out);
  f.push(KeyEvent{uint8_t(K("s")), true, 0}, 0, out);
  f.push(KeyEvent{uint8_t(K("d")), true, 0}, 0, out);
  f.push(KeyEvent{uint8_t(K("f")), true, 0}, 0, out);   // pair d/f, both pads: both dropped
  CHECK_EQ(f.droppedDowns(), 2);
  CHECK(f.realMask() == ((1ull << K("a")) | (1ull << K("s"))));
  f.push(KeyEvent{uint8_t(K("a")), false, 0}, 0, out);
  f.push(KeyEvent{uint8_t(K("s")), false, 0}, 0, out);
  CHECK_EQ(f.push(KeyEvent{uint8_t(K("f")), true, 0}, 0, out), 1);
  // G0 is not in the matrix: straight through
  CHECK_EQ(f.push(KeyEvent{kKeyG0, true, 0}, 0, out), 1);
}

// With diodes (no phantoms) a key that would close a loop is only held back
// until the next frame.
static void testNoGhostHardware() {
  const uih::GhostOrder keep = R->matrix.order;
  R->matrix.order = uih::GhostOrder::Off;
  R->start(false);
  down("1"); down("2");
  R->down(K("3"));
  CHECK(!(S(6, 2).flags & kStepOn));          // held back: a phantom partner is expected
  R->run(40);
  CHECK(S(6, 2).flags & kStepOn);             // none came: the held step was copied there
  up("3"); up("2"); up("1");
  CHECK(clean());
  R->matrix.order = keep;
}

// ---- modifier gestures -------------------------------------------------------------

static void testCtrlShift() {
  // CTRL+SHIFT+X: scan rectangle with f. CTRL+SHIFT+F means nothing, so x wins.
  R->start(true);
  const int before = stepsOn(0);
  CHECK(before > 0);
  const uint32_t d0 = drops();
  down("ctl"); down("sft"); down("x");
  CHECK_EQ(R->ui.clipKind(), 2);
  CHECK_EQ(stepsOn(0), 0);
  CHECK_EQ(drops() - d0, 1u);
  up("x"); up("sft"); up("ctl");
  CHECK(clean());
  // CTRL+SHIFT+C: no rectangle
  R->start(true);
  down("ctl"); down("sft"); down("c");
  CHECK_EQ(R->ui.clipKind(), 2);
  CHECK_EQ(stepsOn(0), before);
  up("c"); up("sft"); up("ctl");
  // CTRL+SHIFT+step (track length), OPT+SHIFT+step (copy pattern): no rectangle for any step
  down("ctl"); down("sft"); down("w"); up("w"); up("sft"); up("ctl");
  CHECK_EQ(R->proj->patterns[0].tracks[kTrackBass].length, 10);
  down("opt"); down("sft"); down("7"); up("7"); up("sft"); up("opt");
  R->run(50);
  CHECK(std::memcmp(&R->proj->patterns[6].tracks[0], &R->proj->patterns[0].tracks[0], sizeof(TrackPattern)) == 0);
  // SHIFT + [ + ]: bass cutoff back where the sweep started
  R->start(true);
  const int cut = R->proj->tracks[kTrackBass].params[0];
  down("sft"); down("]", 600); down("["); up("["); up("]"); up("sft");
  CHECK_EQ(R->proj->tracks[kTrackBass].params[0], cut);
  CHECK(clean());
}

// ---- STEP EDIT ------------------------------------------------------------------------

static void testStepEdit() {
  // step 1 + SHIFT + '-': rectangle with ';'. ';' with SHIFT no longer means an
  // octave (remapped to CTRL), so '-' wins: VEL -8, the note is untouched.
  R->start(true);                             // bass step 1: A1 (33), vel 100
  down("1", 300);
  CHECK(R->ui.stepEditOpen());
  down("sft"); down("-");
  CHECK_EQ(S(6, 0).vel, 92);
  CHECK_EQ(S(6, 0).note, 33);
  up("-"); up("sft");
  // the same with SHIFT added last (while '-' is held): the phantom comes with SHIFT
  down("-");
  CHECK_EQ(S(6, 0).vel, 91);
  down("sft");
  CHECK(R->ui.heldKeys() & (1ull << K("sft")));
  CHECK_EQ(S(6, 0).note, 33);
  up("-"); up("sft");
  // CTRL+;/. = octave (was SHIFT): no rectangle for any step
  down("ctl"); down(";"); up(";"); up("ctl");
  CHECK_EQ(S(6, 0).note, 45);
  // the old SHIFT+; habit on step 1: the controller reports '-' too, and '-' wins
  down("sft"); down(";");
  CHECK_EQ(S(6, 0).note, 45);
  CHECK_EQ(S(6, 0).vel, 83);
  up(";"); up("sft");
  // - and = together: field default (needed three keys: the old two-key cut broke it)
  down("-"); down("=");
  CHECK_EQ(S(6, 0).vel, 100);
  up("="); up("-");
  up("1");
  CHECK(clean());
  // step q + CTRL+. : octave down; q + SHIFT + ';' would have ghosted '['
  down("q", 300);
  down("ctl"); down("."); up("."); up("ctl");
  CHECK_EQ(S(6, 8).note, 21);
  up("q");
  // step + field pad still held + '=' (2 + a + = ghosts "'", the page key)
  R->start(true);
  down("sft"); tap("'"); up("sft");          // 32 steps, so a page flip would show
  tap("'");
  CHECK_EQ(R->ui.shownPage(), 0);
  down("2", 300); down("a"); down("=");
  CHECK_EQ(S(6, 1).vel, 101);
  CHECK_EQ(R->ui.shownPage(), 0);
  up("="); up("a"); up("2");
  // 3 + s + '-' ghosts ';' (note +1) on a lead step: GATE -1, note untouched
  down("sft"); tap("k"); up("sft");          // lead, step 3 = A3 (57) MIN7, gate 3
  down("3", 300); down("s"); down("-");
  CHECK_EQ(S(7, 2).gate, 2);
  CHECK_EQ(S(7, 2).note, 57);
  up("-"); up("s");
  // SHIFT+l on a held lead step: previous chord type (no rectangle)
  down("sft"); down("l");
  CHECK_EQ(S(7, 2).chord, kChordMaj7);
  up("l"); up("sft"); up("3");
  CHECK(clean());
}

static void testLockModeColumnHeld() {
  // SOUND, hold step 2, column key z still down, '=' (2 + z + = ghosts SPACE)
  R->start(true);
  tap("9");
  CHECK(R->ui.view() == View::Sound);
  down("2"); down("z"); down("=");
  CHECK_EQ(S(6, 1).lockParam, 1);
  CHECK(!R->ui.playing());
  up("="); up("z"); up("2");
  CHECK(clean());
}

static void testTempo() {
  R->start(true);                            // 124 BPM
  down("tab"); down("sft"); down("-"); up("-"); up("sft"); up("tab");
  CHECK_EQ(G(kGTempo), 114);
  down("tab"); down("-"); down("=");         // both: default (three keys, no rectangle)
  CHECK_EQ(G(kGTempo), Engine::globalParamInfo(kGTempo)->def);
  up("="); up("-"); up("tab");
  // step 1 held + TAB + '-' ghosts 'p' (SONG view): tempo -1, view stays
  R->start(true);
  down("1", 300); down("tab"); down("-");
  CHECK_EQ(G(kGTempo), 123);
  CHECK(R->ui.view() == View::Groove);
  up("-"); up("tab"); up("1");
  CHECK(clean());
}

// ---- playing -------------------------------------------------------------------------

static void testPlaying() {
  // pad + SHIFT + note: the phantom is CTRL. f + SHIFT + x would have been
  // CTRL+SHIFT+X (cut the pattern), k + SHIFT + n CTRL+N (new project).
  R->start(true);
  const int before = stepsOn(0);
  down("f"); down("sft"); down("x");
  CHECK_EQ(stepsOn(0), before);
  CHECK_EQ(R->ui.clipKind(), 0);
  CHECK(!(R->ui.heldKeys() & (1ull << K("ctl"))));
  up("x"); up("sft"); up("f");
  down("k"); down("sft"); down("n");
  CHECK(R->ui.dialog() == Dialog::None);
  CHECK_EQ(stepsOn(0), before);
  up("n"); up("sft"); up("k");
  // s + SHIFT + z: the phantom is OPT, whose layer would take the next keys
  down("s"); down("sft"); down("z", 300);
  CHECK(R->ui.overlay() == Overlay::None);
  up("z"); up("sft"); up("s");
  CHECK(clean());
  // note held + FN + pad (z + fn + a ghosts OPT): mute
  down("z"); down("fn"); down("a", 250);
  CHECK(R->ui.muted(0));
  CHECK(R->ui.overlay() == Overlay::Mute);
  up("a"); up("fn"); up("z");
  // pad held + FN + pad (a + fn + s ghosts SHIFT)
  down("a"); down("fn"); down("s");
  CHECK(R->ui.muted(1));
  CHECK(!(R->ui.heldKeys() & (1ull << K("sft"))));
  up("s"); up("fn"); up("a");
  CHECK(clean());
}

static void testRolls() {
  // ALT + a + d (ghosts x): both tracks roll
  R->start(false);
  tap("spc");
  R->run(100);
  const uint8_t k0 = R->eng.status().trig[0], c0 = R->eng.status().trig[2];
  down("alt"); down("a"); down("d", 600);
  const uint8_t k1 = R->eng.status().trig[0], c1 = R->eng.status().trig[2];
  CHECK(k1 != k0);
  CHECK(c1 != c0);
  up("d");
  up("a");
  // ALT + s + ';' ghosts ',' (a note, meaningless with ALT): the rate changes
  CHECK_EQ(R->ui.rollRate(), 1);
  down("s"); down(";");
  CHECK_EQ(R->ui.rollRate(), 2);
  up(";"); up("s"); up("alt");
  tap("spc");
  CHECK(clean());
}

static void testOptWhileHoldingNote() {
  // c held + OPT + 4 ghosts ` (home): pattern 4, and the view is not left
  R->start(true);
  tap("9");
  down("c"); down("opt"); down("4");
  CHECK_EQ(R->ui.editPattern(), 3);
  CHECK(R->ui.view() == View::Sound);
  up("4"); up("opt"); up("c");
  // z held + OPT + 1 ghosts step 3: OPT+1 and OPT+3 are indistinguishable,
  // both are dropped (documented limitation: release the note first)
  R->start(true);
  down("z"); down("opt"); down("1");
  CHECK_EQ(R->ui.editPattern(), 0);
  up("1"); up("opt"); up("z");
  CHECK(clean());
}

static void testThreePadsAndNotes() {
  // a + s + d ghosts f: d and f can't be told apart, both dropped
  R->start(true);
  const uint8_t chh = R->eng.status().trig[3];
  down("a"); down("s"); down("d");
  R->run(50);
  CHECK_EQ(R->ui.selTrack(), 1);
  CHECK_EQ(R->eng.status().trig[3], chh);
  up("d"); up("s"); up("a");
  // a + d + h: no rectangle, all three play (the old two-key cut would have dropped h)
  down("a"); down("d"); down("h");
  CHECK_EQ(R->ui.selTrack(), 5);
  up("h"); up("d"); up("a");
  // z + x + c ghosts ALT: c plays, no roll layer appears
  down("z"); down("x"); down("c", 300);
  CHECK(R->ui.overlay() == Overlay::None);
  CHECK(!(R->ui.heldKeys() & (1ull << K("alt"))));
  CHECK(R->ui.heldKeys() & (1ull << K("c")));
  up("c"); up("x"); up("z");
  CHECK(clean());
}

// ---- every release order of every audited chord ------------------------------------

static void testReleaseOrders() {
  static const char* const kChords[][3] = {
      {"ctl", "sft", "x"}, {"ctl", "sft", "c"}, {"ctl", "sft", "1"}, {"opt", "sft", "q"},
      {"sft", "[", "]"},   {"1", "sft", "-"},   {"1", "sft", ";"},   {"q", "sft", ";"},
      {"1", "ctl", ";"},   {"q", "ctl", "."},   {"5", "sft", "l"},   {"2", "-", "="},
      {"2", "a", "="},     {"3", "s", "-"},     {"8", "k", "="},     {"tab", "sft", "-"},
      {"tab", "-", "="},   {"1", "tab", "-"},   {"f", "sft", "x"},   {"k", "sft", "n"},
      {"s", "sft", "z"},   {"z", "fn", "a"},    {"a", "fn", "s"},    {"alt", "a", "d"},
      {"alt", "s", ";"},   {"x", "alt", "a"},   {"c", "opt", "4"},   {"z", "opt", "1"},
      {"a", "s", "d"},     {"z", "x", "c"},     {",", "x", "d"},     {"2", "z", "="},
      {"1", "2", "3"},     {"q", "w", "e"},
  };
  int perm[3] = {0, 1, 2};
  int runs = 0;
  for (const auto& c : kChords) {
    for (int pressOrder = 0; pressOrder < 2; ++pressOrder) {
      std::sort(perm, perm + 3);
      do {
        R->start(true);
        for (int i = 0; i < 3; ++i) down(c[pressOrder ? 2 - i : i], 40);
        R->run(100);
        for (int i = 0; i < 3; ++i) up(c[perm[i]], 20);
        R->run(50);
        if (R->ui.dialog() != Dialog::None) tap("`");
        ++runs;
        if (!clean()) std::printf("  stuck after %s %s %s released %d%d%d\n", c[0], c[1], c[2], perm[0], perm[1], perm[2]);
        CHECK(clean());
        CHECK_EQ(R->matrix.closed, 0);
      } while (std::next_permutation(perm, perm + 3));
    }
  }
  CHECK_EQ(runs, int(sizeof(kChords) / sizeof(kChords[0])) * 12);
}

int main() {
  static uih::Rig rig;
  R = &rig;
  for (uih::GhostOrder order : {uih::GhostOrder::PhantomLast, uih::GhostOrder::PhantomFirst}) {
    rig.matrix.order = order;
    std::printf("-- key matrix model: %s\n", uih::ghostOrderName(order));
    testModel();
    testNoGhostHardware();
    testCtrlShift();
    testStepEdit();
    testLockModeColumnHeld();
    testTempo();
    testPlaying();
    testRolls();
    testOptWhileHoldingNote();
    testThreePadsAndNotes();
    testReleaseOrders();
  }
  std::printf("itest_ui_ghost: %d checks, %d failed (%u phantom downs, %u phantom ups injected)\n", g_checks, g_fail,
              rig.matrix.phantomDowns, rig.matrix.phantomUps);
  return g_fail ? 1 : 0;
}
