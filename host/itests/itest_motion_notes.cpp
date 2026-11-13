// System test: motion (engine.h "MOTION"): notes carry their own macro values
// (keyframes) and the engine moves each macro in a straight line from one to
// the next, arriving exactly at the next note's start. Sampled through the
// engine's own trajectory (engine_detail::macroPosition) once per quantum, with
// the quantum each step fired in from the status, on the bass (ACID, OPEN):
//
//   keyframes   the value at each keyframe's quantum is exactly its value, a
//               straight line in between (with and without swing), through silence
//               and past notes with no value of their own, and across the wrap
//   single      one keyframe: constant from the first quantum
//   next        a queued pattern / the next song slot: the last note glides to
//               its first keyframe, or to the base (arriving at the boundary)
//   late        a pattern queued after the last keyframe re-aims from where it is
//   tempo       a tempo change mid-glide still arrives on the note
//   polymeter   a track shorter than the pattern wraps on its own length
//   start       the first pass equals the later ones (start value from the loop)
//   base        SetMacro of a macro with motion moves only its base (stored, not
//               heard); a macro without motion moves as before; clearing the
//               values glides to the base; stop rests where it is
//   published   the status shows the heard positions and the motion bits
//   live        motion recording: a moved macro is live (heard, written into the
//               notes it passes), stops being live after a still bar
#include <cmath>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "pg/macro/macro.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;
const pm::MacroSet* g_acid = nullptr;
constexpr int kBass = kTrackBass;

int lvl(int pos) { return macroCodeToPos(macroPosToCode(pos)); }

struct Fire {
  long q;            // the quantum (index into Rec::pos) the step fired in
  int pattern, step; // master step
};
struct Rec {
  std::vector<float> pos;     // macro 0's position in each quantum
  std::vector<float> pos1;    // macro 1's
  std::vector<Fire> fires;
  int lastPattern = -1, lastStep = -1;
};

Rec g_rec;

void quantum() {
  int16_t buf[2 * kQuantum];
  g_inst.process(buf, kQuantum);
  const EngineStatus st = g_inst.engine().status();
  g_rec.pos.push_back(engine_detail::macroPosition(kBass, 0));
  g_rec.pos1.push_back(engine_detail::macroPosition(kBass, 1));
  if (st.playing && (st.masterStep != g_rec.lastStep || st.pattern != g_rec.lastPattern)) {
    g_rec.fires.push_back(Fire{long(g_rec.pos.size()) - 1, st.pattern, st.masterStep});
    g_rec.lastStep = st.masterStep;
    g_rec.lastPattern = st.pattern;
  }
}
void quanta(int n) { for (int i = 0; i < n; ++i) quantum(); }
void untilFires(size_t n) {
  for (int guard = 0; g_rec.fires.size() < n && guard < 400000; ++guard) quantum();
}

void post(const Command& c) { g_inst.engine().post(c); }
Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command k{};
  k.type = t;
  k.a = uint8_t(a); k.b = uint8_t(b); k.c = uint8_t(c);
  return k;
}

Step& S(int pattern, int step) { return g_inst.project().patterns[pattern].tracks[kBass].steps[step]; }
Step note(int value0 = -1, int value1 = -1) {
  Step s = kDefaultStep;
  s.setFlags(kStepOn);
  s.note = 45;
  s.gate = 1;
  if (value0 >= 0) s.setMacroValue(0, value0);
  if (value1 >= 0) s.setMacroValue(1, value1);
  return s;
}

// A fresh engine, ACID on the bass at OPEN 128 (the base), BITE 0, tempo 120, swing.
void setup(int swing = 0, int tempo = 120) {
  g_inst.init(44100.0f, false);
  for (int i = 0; i < pm::instrumentCount(kBass); ++i)
    if (std::strcmp(pm::instrumentAt(kBass, i)->name, "ACID") == 0) g_acid = pm::instrumentAt(kBass, i);
  const uint8_t pos[4] = {128, 0, 0, 255};
  post(setInstrumentCommand(kBass, g_acid->id, pos));
  Command g = cmd(CmdType::SetGlobal, kGTempo, 0, tempo);
  post(g);
  post(cmd(CmdType::SetGlobal, kGSwing, 0, swing));
  quanta(2);
  g_rec = Rec();
}

void play() {
  post(cmd(CmdType::Play));
  g_rec.lastStep = g_rec.lastPattern = -1;
}

// The fire record of the n-th time (0-based) master step `step` of `pattern` fired.
const Fire* fireOf(int pattern, int step, int nth = 0) {
  for (const Fire& f : g_rec.fires)
    if (f.pattern == pattern && f.step == step && nth-- == 0) return &f;
  return nullptr;
}

// From quantum qa (value va) to qb (value vb): a straight line, both ends exact.
// Returns the largest deviation.
float lineErr(const std::vector<float>& pos, long qa, float va, long qb, float vb) {
  float worst = 0.0f;
  for (long q = qa; q <= qb && q < long(pos.size()); ++q) {
    const float want = va + (vb - va) * float(q - qa) / float(qb - qa);
    const float e = std::fabs(pos[size_t(q)] - want);
    if (e > worst) worst = e;
  }
  return worst;
}
constexpr float kExact = 0.01f;   // float ramp arithmetic

// ---- keyframes, straight lines, silence, the wrap, swing ----------------------------------
void testKeyframes(int swing) {
  setup(swing);
  const int a = lvl(40), b = lvl(200);
  S(0, 0) = note(a);
  S(0, 4) = note();          // a note with no value of its own: the line passes it
  S(0, 5) = note(b);         // odd: swing delays it
  // 6..15 silent
  play();
  untilFires(2 * 16 + 1);
  const Fire* f0 = fireOf(0, 0), *f5 = fireOf(0, 5), *g0 = fireOf(0, 0, 1), *g5 = fireOf(0, 5, 1), *h0 = fireOf(0, 0, 2);
  it::check(f0 && f5 && g0 && g5 && h0, "swing %d: steps seen", swing);
  if (!(f0 && f5 && g0 && g5 && h0)) return;
  const std::vector<float>& p = g_rec.pos;
  it::check(p[size_t(f0->q)] == float(a) && p[size_t(f5->q)] == float(b) && p[size_t(g0->q)] == float(a) &&
                p[size_t(g5->q)] == float(b) && p[size_t(h0->q)] == float(a),
            "swing %d: exactly the note's value at each keyframe (%.3f %.3f %.3f %.3f)", swing, double(p[size_t(f0->q)]),
            double(p[size_t(f5->q)]), double(p[size_t(g0->q)]), double(p[size_t(g5->q)]));
  const float e1 = lineErr(p, f0->q, float(a), f5->q, float(b));
  const float e2 = lineErr(p, f5->q, float(b), g0->q, float(a));   // through silence and the wrap
  const float e3 = lineErr(p, g0->q, float(a), g5->q, float(b));
  it::check(e1 < kExact && e2 < kExact && e3 < kExact, "swing %d: straight lines between (%.4f %.4f %.4f)", swing,
            double(e1), double(e2), double(e3));
  if (swing) {
    setup(0);
    S(0, 0) = note(a);
    S(0, 5) = note(b);
    play();
    untilFires(6);
    const Fire* s5 = fireOf(0, 5);
    it::check(s5 && s5->q < f5->q, "swing delays the keyframe on an odd step (%ld vs %ld)", s5 ? s5->q : -1, f5->q);
  }
  // the page sees it: the published position and the motion bit
  const EngineStatus st = g_inst.engine().status();
  it::check(st.motion[kBass] == 1 && std::abs(int(st.macroPos[kBass][0]) - int(g_rec.pos.back() + 0.5f)) <= 3,
            "published: motion bit %d, position %d (%.1f)", st.motion[kBass], st.macroPos[kBass][0], double(g_rec.pos.back()));
}

// ---- one keyframe: constant; the base moves without being heard; macro 2 moves --------------
void testSingleAndBase() {
  setup();
  const int v = lvl(100);
  S(0, 3) = note(v);
  play();
  untilFires(20);
  float lo = 1e9f, hi = -1e9f;
  for (float x : g_rec.pos) { lo = std::fmin(lo, x); hi = std::fmax(hi, x); }
  it::check(lo == float(v) && hi == float(v), "single keyframe: constant from the first quantum (%.2f..%.2f)", double(lo), double(hi));
  // SetMacro of OPEN (has motion): the base only
  post(setMacroCommand(kBass, 0, 30));
  // SetMacro of BITE (no motion): moves as before
  post(setMacroCommand(kBass, 1, 200));
  quanta(400);
  it::check(g_inst.project().tracks[kBass].macro[0] == 30, "base: stored");
  it::check(g_rec.pos.back() == float(v), "base: not heard while the motion owns OPEN (%.2f)", double(g_rec.pos.back()));
  it::check(g_rec.pos1.back() == 200.0f, "a macro without motion moves (%.2f)", double(g_rec.pos1.back()));
  const EngineStatus st = g_inst.engine().status();
  it::check(st.motion[kBass] == 1 && st.macroPos[kBass][0] == v && st.macroPos[kBass][1] == 200, "published while still");
  // the values cleared (as the UI does it, SetStep): the motion ends, OPEN glides to its base
  Command c = cmd(CmdType::SetStep, 0, kBass, 3);
  c.step = note();
  post(c);
  quanta(1);
  const size_t at = g_rec.pos.size();
  quanta(400);
  it::check(g_rec.pos.back() == 30.0f, "cleared: OPEN at its base (%.2f)", double(g_rec.pos.back()));
  it::check(g_rec.pos[at] > 30.0f && g_rec.pos[at] < float(v), "cleared: it glides (%.2f)", double(g_rec.pos[at]));
  it::check(g_inst.engine().status().motion[kBass] == 0, "cleared: no motion bit");
  // stop rests where it is
  S(0, 0) = note(lvl(255));
  S(0, 8) = note(lvl(0));
  c.c = 0; c.step = S(0, 0);
  post(c);
  quanta(1);
  g_rec.fires.clear();
  untilFires(5);
  const float here = g_rec.pos.back();
  post(cmd(CmdType::Stop));
  quanta(300);
  it::check(std::fabs(g_rec.pos.back() - here) < 1.0f, "stop: rests where it was (%.2f vs %.2f)", double(g_rec.pos.back()),
            double(here));
  it::check(g_inst.engine().status().motion[kBass] == 0, "stop: the motion lets go");
  post(setMacroCommand(kBass, 0, 30));   // a move after stop glides to the base again
  quanta(400);
  it::check(g_rec.pos.back() == 30.0f, "after stop a move is heard (%.2f)", double(g_rec.pos.back()));
}

// ---- next pattern: queued, song slot, with and without keyframes, late ----------------------
void testNext() {
  const int a = lvl(40), b = lvl(200), c = lvl(120);
  for (int mode = 0; mode < 4; ++mode) {   // 0 queued, 1 queued (no keyframes), 2 song slot, 3 late queue
    setup();
    S(0, 0) = note(a);
    S(0, 8) = note(b);
    S(1, 2) = note();
    if (mode != 1) S(1, 4) = note(c);
    Project& p = g_inst.project();
    if (mode == 2) {
      p.song[0] = SongSlot{0, 1};
      p.song[1] = SongSlot{1, 1};
      p.song[2] = SongSlot{0, 0};
      post(cmd(CmdType::SetSongMode, 0, 0, 1));
    }
    play();
    untilFires(3);
    if (mode == 0 || mode == 1) post(cmd(CmdType::QueuePattern, 1));
    untilFires(11);   // master step 10: the last keyframe (8) has passed
    long late = -1;
    float lateV = 0.0f;
    if (mode == 3) {
      post(cmd(CmdType::QueuePattern, 1));
      quantum();
      late = long(g_rec.pos.size()) - 1;
      lateV = g_rec.pos[size_t(late)];
    }
    untilFires(16 + 6);
    const Fire* f8 = fireOf(0, 8), *n0 = fireOf(1, 0), *n4 = fireOf(1, 4);
    it::check(f8 && n0 && n4, "next %d: steps seen", mode);
    if (!(f8 && n0 && n4)) continue;
    const std::vector<float>& q = g_rec.pos;
    if (mode == 1) {
      it::check(lineErr(q, f8->q, float(b), n0->q, 128.0f) < kExact, "no keyframe next: a line to the base, at the boundary");
      it::check(lineErr(q, n0->q, 128.0f, n4->q, 128.0f) < kExact, "no keyframe next: then the base");
      it::check(g_inst.engine().status().motion[kBass] == 0, "no keyframe next: no motion there");
    } else if (mode == 3) {
      // the line toward this pattern's step 0 until the queue, then a new one from where it was
      const long qb = n4->q;
      it::check(lineErr(q, late, lateV, qb, float(c)) < kExact, "late: re-aimed from where it was (%.4f)",
                double(lineErr(q, late, lateV, qb, float(c))));
      it::check(std::fabs(q[size_t(late)] - q[size_t(late - 1)]) < 0.2f, "late: no jump at the queue");
      it::check(q[size_t(qb)] == float(c), "late: exactly on the note");
    } else {
      it::check(lineErr(q, f8->q, float(b), n4->q, float(c)) < kExact, "next %d: one line from the last note to the first of the next", mode);
      it::check(q[size_t(n4->q)] == float(c), "next %d: exactly on the note", mode);
    }
  }
}

// ---- tempo change mid-glide -------------------------------------------------------------
void testTempo() {
  setup();
  const int a = lvl(40), b = lvl(200);
  S(0, 0) = note(a);
  S(0, 8) = note(b);
  play();
  untilFires(11);
  post(cmd(CmdType::SetGlobal, kGTempo, 0, 90));
  quantum();
  const long qc = long(g_rec.pos.size()) - 1;
  untilFires(16 + 2);
  const Fire* g0 = fireOf(0, 0, 1);
  it::check(g0 != nullptr, "tempo: wrapped");
  if (!g0) return;
  const std::vector<float>& q = g_rec.pos;
  it::check(q[size_t(g0->q)] == float(a), "tempo: still exactly on the note (%.3f)", double(q[size_t(g0->q)]));
  it::check(lineErr(q, qc, q[size_t(qc)], g0->q, float(a)) < kExact, "tempo: a new straight line from the change");
  it::check(std::fabs(q[size_t(qc)] - q[size_t(qc - 1)]) < 0.2f, "tempo: no jump");
}

// ---- polymeter --------------------------------------------------------------------------
void testPolymeter() {
  setup();
  const int a = lvl(40), b = lvl(200);
  g_inst.project().patterns[0].tracks[kBass].length = 3;
  S(0, 0) = note(a);
  S(0, 2) = note(b);
  play();
  untilFires(2 * 16 + 1);
  int bad = 0, seen = 0;
  for (int pass = 0; pass < 2; ++pass)
    for (int m = 0; m < 16; ++m) {
      const int ts = m % 3;
      if (ts == 1) continue;
      const Fire* f = fireOf(0, m, pass);
      if (!f) { ++bad; continue; }
      ++seen;
      if (g_rec.pos[size_t(f->q)] != float(ts == 0 ? a : b)) ++bad;
    }
  it::check(seen == 22 && bad == 0, "polymeter: the track's own wrap (%d keyframes, %d wrong)", seen, bad);
  // master 15 (track step 0, a) -> boundary: track step 0 again (a): constant across it
  const Fire* f15 = fireOf(0, 15), *g0 = fireOf(0, 0, 1);
  if (f15 && g0) it::check(lineErr(g_rec.pos, f15->q, float(a), g0->q, float(a)) < kExact, "polymeter: across the boundary");
}

// ---- start: the first pass equals the later ones ----------------------------------------
void testStart() {
  setup(33);
  const int a = lvl(40), b = lvl(200);
  S(0, 4) = note(a);
  S(0, 11) = note(b);
  play();
  untilFires(3 * 16 + 1);
  float worst = 0.0f;
  for (int m = 0; m < 16; ++m) {
    const Fire* f1 = fireOf(0, m, 0), *f2 = fireOf(0, m, 1), *f3 = fireOf(0, m, 2);
    if (!f1 || !f2 || !f3) { worst = 1e9f; break; }
    worst = std::fmax(worst, std::fabs(g_rec.pos[size_t(f1->q)] - g_rec.pos[size_t(f2->q)]));
    worst = std::fmax(worst, std::fabs(g_rec.pos[size_t(f3->q)] - g_rec.pos[size_t(f2->q)]));
  }
  // within what one quantum of the line moves (160 over ~9 steps of ~345 quanta: 0.05)
  it::check(worst < 0.1f, "start: the first pass as every later one (%.4f)", double(worst));
  const float s0 = g_rec.pos[size_t(fireOf(0, 0)->q)];
  it::check(s0 > float(a) && s0 < float(b), "start: between the loop's last and first note (%.2f)", double(s0));
}

// ---- motion recording -------------------------------------------------------------------
void testLive() {
  setup();
  for (int s : {0, 4, 8, 12}) S(0, s) = note();
  S(0, 8) = note(lvl(10));
  post(cmd(CmdType::SetRecord, 0, 0, 1));
  play();
  untilFires(2);
  // the hand: a sweep of BITE (macro 2) and OPEN while recording
  for (int k = 0; k < 100; ++k) {   // 6000 quanta: past every note of the pattern
    post(setMacroCommand(kBass, 0, uint8_t(150 + k)));
    post(setMacroCommand(kBass, 1, uint8_t(100 + k)));
    quanta(60);
  }
  const EngineStatus st = g_inst.engine().status();
  it::check(st.live[kBass] == 3, "live: both moved macros are live (%d)", st.live[kBass]);
  it::check(std::fabs(g_rec.pos.back() - 249.0f) < 4.0f, "live: the hand is heard, not the old motion (%.2f)", double(g_rec.pos.back()));
  int written = 0;
  for (int s : {0, 4, 8, 12}) written += S(0, s).hasMacro(0) && S(0, s).hasMacro(1);
  it::check(written == 4, "live: every note passed took the values (%d)", written);
  it::check(!S(0, 1).hasExt(), "live: no values on empty steps");
  // still for a bar: not live any more; the recorded motion plays
  untilFires(g_rec.fires.size() + 16 + 4);
  it::check(g_inst.engine().status().live[kBass] == 0, "live: a still bar ends it");
  it::check(g_inst.engine().status().motion[kBass] == 3, "live: the recording is motion now");
  const Fire* f = nullptr;
  for (const Fire& x : g_rec.fires) if (x.step == 4) f = &x;
  it::check(f && g_rec.pos[size_t(f->q)] == float(S(0, 4).macroValue(0)), "live: the recorded value plays at its note");
  // record off ends it at once
  post(setMacroCommand(kBass, 0, 20));
  quanta(10);
  it::check(g_inst.engine().status().live[kBass] == 1, "live again");
  post(cmd(CmdType::SetRecord, 0, 0, 0));
  quanta(2);
  it::check(g_inst.engine().status().live[kBass] == 0, "record off: not live");
  post(cmd(CmdType::Stop));
  quanta(2);
}

}  // namespace

int main() {
  testKeyframes(0);
  testKeyframes(66);
  testSingleAndBase();
  testNext();
  testTempo();
  testPolymeter();
  testStart();
  testLive();
  return it::finish("itest_motion_notes");
}
