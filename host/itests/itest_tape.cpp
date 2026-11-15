// THE TAPE (pg/tape/tape.h) in the engine: position, recording, tricks, storage.
//
//   * an empty tape changes nothing: the demo song renders bit for bit as without a
//     tape, in int16 and in float;
//   * alignment: an impulse on every bar line, recorded from TRACK and from MIX, lands
//     on the tape where the bar is and plays back on the frame the sequencer fires the
//     bar (exactly at 120 BPM / 48 kHz, where a bar is a whole number of quanta; at
//     44.1 kHz, where it is not, within the sequencer's own rounding of a bar line to
//     its quantum, and on the exact grid);
//   * a loop wrap inside a take, overdub (sums), replace (the input alone), undo and
//     redo, erase of the loop region (and its undo);
//   * the bounce: MIX onto a free track, the instrument tracks muted, plays back what
//     the mix was, within the int16 rounding of the tape;
//   * the tricks on a tone: HALF an octave down, REVERSE backwards, BRAKE down to a stop
//     and spun back up; after each release the tape is where it would have been, and
//     no transition steps further than the tone itself does;
//   * sparse storage (silence costs no block), running out of memory during a take (a
//     clean end, what was recorded intact), the end of the tape;
//   * song mode: the tape runs along the song; a bounce of the song plays back;
//   * LINE: the platform's input with its round-trip latency lands where it was played;
//   * the tempo the tape was recorded at stays when the project's changes.
#include <cmath>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "pg/demo/demo.h"
#include "common/memfs.h"
#include "pg/tape/tape.h"
#include "pg/tape/tape_io.h"

using namespace pg;
using it::check;

namespace {

Instrument g_inst;
tape::Tape g_tape;
std::vector<uint8_t> g_mem;
float g_sr = 48000.0f;
Engine& eng() { return g_inst.engine(); }
Project& proj() { return g_inst.project(); }

// One sample of `amp` at each note-on (the first frame the voice renders), else silence.
struct ImpulseVoice final : Voice {
  float amp = 0.5f;
  bool pending = false;
  void init(float) override {}
  const ParamInfo* paramInfo(int) const override { return nullptr; }
  void setParams(const uint8_t*) override {}
  void noteOn(const NoteEvent&) override { pending = true; }
  void noteOff(uint8_t) override {}
  void choke() override { pending = false; }
  void render(float* L, float* R, int n) override {
    for (int i = 0; i < n; ++i) L[i] = R[i] = 0.0f;
    if (pending) { L[0] = R[0] = amp; pending = false; }
  }
  bool active() const override { return pending; }
  bool mono() const override { return true; }
};
ImpulseVoice g_imp;

void post(const Command& c) { check(eng().post(c), "post"); }
Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}
void tc(tape::Op op, int b = 0, int c = 0, int note = 0) { post(tape::tapeCommand(op, b, c, note)); }

// Renders n frames (64 at a time, the main thread's service() between blocks).
void run(long n, std::vector<int16_t>* out = nullptr, std::vector<float>* fout = nullptr) {
  int16_t buf[128];
  float fb[128];
  for (long done = 0; done < n; done += 64) {
    if (fout) {
      eng().processFloat(fb, 64);
      fout->insert(fout->end(), fb, fb + 128);
    } else {
      eng().process(buf, 64);
      if (out) out->insert(out->end(), buf, buf + 128);
    }
    g_tape.service();
  }
}
// Runs until the tape's view satisfies f (at most maxFrames).
template <class F>
bool runUntil(F f, long maxFrames = 48000L * 60) {
  for (long d = 0; d < maxFrames; d += 64) {
    if (f(g_tape.view())) return true;
    run(64);
  }
  return false;
}
bool idleNow() { return runUntil([](const tape::View&) { return g_tape.idle(); }); }

// A fresh instrument at sr with an empty project at bpm; the tape (seconds of memory)
// installed unless noTape; track 0 plays the impulse voice when impulse.
void fresh(float sr, int bpm, bool impulse, uint32_t seconds = 120, bool noTape = false, size_t bytes = 0) {
  eng().setTape(nullptr);
  g_sr = sr;
  g_inst.init(sr, false);
  if (impulse) eng().setVoice(0, &g_imp);
  eng().suspend();
  eng().initProject(proj());
  proj().globals[kGTempo] = uint8_t(bpm);
  eng().sanitizeProject(proj());
  eng().resume();
  if (!noTape) {
    g_mem.assign(bytes ? bytes : tape::Tape::bytesFor(seconds, uint32_t(sr)), 0);
    check(g_tape.init(g_mem.data(), g_mem.size(), uint32_t(sr)), "tape init");
    eng().setTape(&g_tape);
  }
  run(64);
}
void stepOn(int t, int i, int note = 60, int vel = 100) {
  proj().tracks[t].params[kMixLevel] = 100;
  Step& s = proj().patterns[0].tracks[t].steps[i];
  s = kDefaultStep;
  s.setFlags(kStepOn);
  s.note = uint8_t(note);
  s.vel = uint8_t(vel);
}
long barFrames(float sr, int bpm) { return long(std::lround(double(sr) * 60.0 * 4.0 / bpm)); }
int16_t tapeAt(int t, uint32_t a) {
  int16_t v = 0;
  g_tape.read(t, a, &v, 1);
  return v;
}
// Frames where |L| first exceeds thr after each start of a run of silence.
std::vector<long> onsets(const std::vector<int16_t>& v, int thr) {
  std::vector<long> r;
  bool quiet = true;
  long q = 1000;
  for (size_t i = 0; i < v.size() / 2; ++i) {
    const int x = std::abs(int(v[2 * i]));
    if (x > thr && quiet && q > 200) r.push_back(long(i));
    if (x > thr) { quiet = false; q = 0; } else if (++q > 200) quiet = true;
  }
  return r;
}
int maxDiff(const std::vector<int16_t>& a, const std::vector<int16_t>& b, size_t from = 0) {
  int m = 0;
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = from; i < n; ++i) { const int d = std::abs(int(a[i]) - int(b[i])); if (d > m) m = d; }
  return m;
}
double rms(const std::vector<int16_t>& a) {
  double s = 0;
  for (int16_t x : a) s += double(x) * x;
  return a.empty() ? 0 : std::sqrt(s / double(a.size()));
}
// Records one take: armed while stopped (a count-in bar, then Play from the top) or
// while playing (the next bar line); ended after `bars` bar lines.
void take(int track, int source, int srcTrack, int bars, bool replace = false) {
  g_tape.prepareTake();
  tc(tape::kOpArm, track, source | (replace ? tape::kArmReplace : 0), srcTrack);
  check(runUntil([](const tape::View& v) { return v.rec == tape::Rec::Rec; }), "take starts");
  // End in the middle of the last bar: it ends on the bar line after it
  run(long(bars - 1) * barFrames(g_sr, proj().globals[kGTempo]) + barFrames(g_sr, proj().globals[kGTempo]) / 2);
  tc(tape::kOpEnd);
  check(idleNow(), "take merged");
}

// ---- an empty tape changes nothing ---------------------------------------------------
void testEmptyIdentical() {
  for (int fl = 0; fl < 2; ++fl) {
    std::vector<int16_t> a, b;
    std::vector<float> fa, fb;
    for (int pass = 0; pass < 2; ++pass) {
      fresh(48000.0f, 124, false, 30, pass == 0);
      eng().suspend();
      demo::build(eng(), proj(), 0);
      eng().sanitizeProject(proj());
      eng().resume();
      post(cmd(CmdType::Play));
      if (fl) run(48000 * 12, nullptr, pass ? &fb : &fa);
      else run(48000 * 12, pass ? &b : &a);
    }
    if (fl) check(fa == fb && !fa.empty(), "empty tape: float output bit-identical");
    else check(a == b && !a.empty(), "empty tape: int16 output bit-identical");
  }
}

// ---- alignment -------------------------------------------------------------------------
void testAlignment(float sr, int source) {
  const int bpm = 120;
  fresh(sr, bpm, true);
  stepOn(0, 0);   // an impulse on every bar line
  tc(tape::kOpLoop, 0, 2);
  const long bar = barFrames(sr, bpm);
  post(cmd(CmdType::Play));
  run(bar / 3);
  take(1, source, 0, 2);   // armed in bar 1: starts on bar 2's line (loop bar 0), ends after 2 bars
  check(!g_tape.trackEmpty(1), "%s: the take is on tape", source == tape::kSrcTrack ? "TRACK" : "MIX");
  // the tape: an impulse at the start of each loop bar (exactly where the grid puts it)
  const uint64_t stepFP = ((uint64_t(sr) * 256u * 15u) << 24) / uint64_t(bpm);
  for (int b = 0; b < 2; ++b) {
    const uint32_t a = uint32_t((uint64_t(b) * 16u * stepFP + (uint64_t(1) << 31)) >> 32);
    int best = 0;
    long at = -1;
    for (int d = -20; d <= 20; ++d) {
      const int v = std::abs(int(tapeAt(1, uint32_t(int64_t(a) + d))));
      if (v > best) { best = v; at = d; }
    }
    if (source == tape::kSrcTrack)
      check(best == 8192, "TRACK %g Hz: bar %d's impulse is 0.5 on tape (%d)", double(sr), b, best);
    else
      check(best > 1000, "MIX %g Hz: bar %d's impulse on tape (%d)", double(sr), b, best);
    // the sequencer fires a bar line on the quantum edge nearest its exact time: the
    // impulse is that far from the exact grid (exactly on it when a bar is whole quanta)
    const bool whole = (bar % kQuantum) == 0;
    if (whole) check(at == 0, "%g Hz: bar %d's impulse exactly on the bar (%ld)", double(sr), b, at);
    else check(std::labs(at) <= kQuantum / 2, "%g Hz: bar %d's impulse within the grid's rounding (%ld)", double(sr), b, at);
  }
  // playback lines up with the sequencer: live (tape track muted) and tape (track 0 muted)
  std::vector<int16_t> live, taped;
  post(cmd(CmdType::Stop));
  run(4800);
  tc(tape::kOpParam, 1, 1, tape::kPMute);
  post(cmd(CmdType::Play));
  run(bar * 4, &live);
  post(cmd(CmdType::Stop));
  run(4800);
  tc(tape::kOpParam, 1, 0, tape::kPMute);
  post(cmd(CmdType::SetMute, 0, 0, 1));
  run(4800);
  post(cmd(CmdType::Play));
  run(bar * 4, &taped);
  const std::vector<long> ol = onsets(live, 100), ot = onsets(taped, 100);
  if (std::getenv("TAPE_DEBUG")) {
    for (long x : ol) std::printf("live %ld\n", x);
    for (long x : ot) std::printf("taped %ld\n", x);
  }
  check(ol.size() >= 4 && ot.size() == ol.size(), "%g Hz src %d: 4 bar impulses live (%zu) and on tape (%zu)", double(sr),
        source, ol.size(), ot.size());
  for (size_t i = 0; i < ol.size() && i < ot.size(); ++i) {
    const long d = ot[i] - ol[i];
    if ((bar % kQuantum) == 0) check(d == 0, "%g Hz src %d bar %zu: on tape at the live frame (%+ld)", double(sr), source, i, d);
    else check(std::labs(d) <= kQuantum, "%g Hz src %d bar %zu: within a quantum of the live frame (%+ld)", double(sr), source, i, d);
  }
  if ((bar % kQuantum) == 0)
    check(maxDiff(live, taped) <= 2, "%g Hz src %d: tape playback = live within rounding (%d)", double(sr), source,
          maxDiff(live, taped));
}

// ---- overdub, replace, undo, erase, loop wrap -----------------------------------------
void testOverdubReplaceUndo() {
  const float sr = 48000.0f;
  fresh(sr, 120, true);
  stepOn(0, 0);
  stepOn(0, 8);
  tc(tape::kOpLoop, 0, 2);
  post(cmd(CmdType::Play));
  run(2000);
  // take 1: armed in bar 0 -> starts at loop bar 1, runs 2 bars: bar 1, then (wrapped) bar 0
  g_tape.prepareTake();
  tc(tape::kOpArm, 0, tape::kSrcTrack, 0);
  check(runUntil([](const tape::View& v) { return v.rec == tape::Rec::Rec; }), "wrap take starts");
  const uint32_t start = g_tape.view().takeStart;
  const uint32_t barA = uint32_t(barFrames(sr, 120));
  check(start == barA, "the take starts at loop bar 1 (%u)", start);
  runUntil([](const tape::View& v) { return v.addr >= 16 && v.addr < 1000; });   // across the loop's end
  if (std::getenv("TAPE_DEBUG")) std::printf("end at addr %u rec %d\n", g_tape.view().addr, int(g_tape.view().rec));
  tc(tape::kOpEnd);
  check(idleNow(), "wrap take merged");
  if (std::getenv("TAPE_DEBUG")) {
    std::vector<int16_t> all(size_t(4 * barFrames(sr, 120)));
    g_tape.read(0, 0, all.data(), uint32_t(all.size()));
    for (size_t i = 0; i < all.size(); ++i) if (all[i]) std::printf("wrap tape[%zu] = %d\n", i, all[i]);
    std::printf("why %s end %u recorded %u\n", tape::endWhyName(g_tape.view().why), g_tape.trackEnd(0), g_tape.view().recorded);
  }
  const uint32_t half = barA / 2;
  check(tapeAt(0, 0) == 8192 && tapeAt(0, half) == 8192 && tapeAt(0, barA) == 8192 && tapeAt(0, barA + half) == 8192,
        "the take wrapped: all four impulses once (%d %d %d %d)", tapeAt(0, 0), tapeAt(0, half), tapeAt(0, barA),
        tapeAt(0, barA + half));
  check(g_tape.trackEnd(0) <= ((2 * barA + tape::kBlock - 1) / tape::kBlock) * tape::kBlock,
        "nothing written past the loop");
  // take 2: overdub the same, 2 bars -> sums
  take(0, tape::kSrcTrack, 0, 2);
  check(tapeAt(0, 0) == 16384 && tapeAt(0, barA) == 16384, "overdub sums (%d %d)", tapeAt(0, 0), tapeAt(0, barA));
  // take 3: replace -> the input alone again
  take(0, tape::kSrcTrack, 0, 2, true);
  check(tapeAt(0, 0) == 8192 && tapeAt(0, barA + half) == 8192, "replace: the input (%d %d)", tapeAt(0, 0),
        tapeAt(0, barA + half));
  check(g_tape.canUndo(), "undo available");
  check(g_tape.undo(), "undo");
  check(tapeAt(0, 0) == 16384, "undo: the overdub is back (%d)", tapeAt(0, 0));
  check(g_tape.undo(), "redo");
  check(tapeAt(0, 0) == 8192, "redo: the replace again (%d)", tapeAt(0, 0));
  // the guard: an overdub over loud audio does not wrap
  g_imp.amp = 1.9f;
  take(0, tape::kSrcTrack, 0, 2);
  take(0, tape::kSrcTrack, 0, 2);
  const int v = tapeAt(0, 0);
  check(v > 24576 && v < 32767, "the overdub guard: a soft ceiling, no wrap (%d)", v);
  g_imp.amp = 0.5f;
  // erase the loop region, and undo it
  post(cmd(CmdType::Stop));
  run(4800);
  check(g_tape.erase(0, 0, 2 * barA), "erase the loop");
  check(tapeAt(0, 0) == 0 && tapeAt(0, barA) == 0 && g_tape.trackEmpty(0), "erased");
  check(g_tape.undo() && tapeAt(0, 0) == v, "erase undone (%d)", tapeAt(0, 0));
  // erase a part of a block: the rest stays
  check(g_tape.erase(0, 0, half + 10), "erase a part");
  check(tapeAt(0, 0) == 0 && tapeAt(0, half) == 0 && tapeAt(0, barA) == v, "the part erased, the rest kept");
}

// ---- the bounce --------------------------------------------------------------------------
void groove() {
  for (int i : {0, 4, 8, 12}) stepOn(kTrackKick, i);
  for (int i : {2, 6, 10, 14}) stepOn(kTrackHatClosed, i);
  stepOn(kTrackSnare, 4);
  stepOn(kTrackSnare, 12);
  for (int i : {0, 3, 6, 10, 13}) stepOn(kTrackBass, i, 33 + (i % 5));
  stepOn(kTrackLead, 2, 57);
  stepOn(kTrackLead, 10, 60);
  proj().tracks[kTrackLead].params[kMixReverbSend] = 60;
  proj().tracks[kTrackSnare].params[kMixDelaySend] = 50;
}
void testBounce(bool mono) {
  const float sr = 48000.0f;
  fresh(sr, 120, false);
  if (mono) {
    eng().suspend();
    eng().setMonoOutput(true);
    eng().resume();
  }
  groove();
  tc(tape::kOpLoop, 0, 1);
  const long bar = barFrames(sr, 120);
  run(64);
  // from stop: a count-in bar, then the first bar from the top (the FX silent before it);
  // what is heard while it records is the reference (the hats' noise is never the same twice)
  std::vector<int16_t> heard, bounce;
  g_tape.prepareTake();
  tc(tape::kOpArm, 2, tape::kSrcMix, 0);
  run(bar, &heard);   // the count-in: one bar of silence
  check(rms(heard) == 0.0, "the count-in is silent");
  heard.clear();
  run(bar / 2, &heard);
  check(g_tape.view().rec == tape::Rec::Rec, "recording after the count-in");
  tc(tape::kOpEnd);
  run(bar / 2, &heard);
  check(idleNow(), "the bounce merged");
  check(g_tape.view().why == tape::kEndBar, "the bounce ended on the bar line");
  post(cmd(CmdType::Stop));
  run(48000 * 8);   // the tails ring out
  for (int t = 0; t < kNumTracks; ++t) post(cmd(CmdType::SetMute, t, 0, 1));
  run(4800);
  post(cmd(CmdType::Play));
  run(bar, &bounce);
  check(rms(heard) > 500.0, "the groove is loud enough to mean something (%.0f)", rms(heard));
  if (mono) {
    const int d = maxDiff(heard, bounce);
    check(d <= 3, "mono bounce = the mix within the tape's rounding (max %d LSB)", d);
  } else {
    // the tape is mono: its playback is the mix's (L + R) / 2 on both sides
    int d = 0;
    for (size_t i = 0; i + 1 < heard.size() && i + 1 < bounce.size(); i += 2) {
      const int m = (int(heard[i]) + int(heard[i + 1])) / 2;
      d = std::max(d, std::abs(m - int(bounce[i])));
      d = std::max(d, std::abs(int(bounce[i]) - int(bounce[i + 1])));
    }
    check(d <= 4, "stereo bounce = the mix's mid on both sides within rounding (max %d LSB)", d);
  }
}

// ---- tricks --------------------------------------------------------------------------------
// The tape holds a 300 Hz tone on track 0 (written as a loaded tape); a 4-bar loop.
struct ToneRun {
  std::vector<int16_t> out;
};
int maxStep(const std::vector<int16_t>& v, size_t a, size_t b) {
  int m = 0;
  for (size_t i = a + 1; i < b && i < v.size() / 2; ++i) {
    const int d = std::abs(int(v[2 * i]) - int(v[2 * (i - 1)]));
    if (d > m) m = d;
  }
  return m;
}
double freqOf(const std::vector<int16_t>& v, size_t a, size_t b, float sr) {
  int z = 0;
  long first = -1, last = -1;
  for (size_t i = a + 1; i < b; ++i)
    if (v[2 * (i - 1)] < 0 && v[2 * i] >= 0) {
      if (first < 0) first = long(i);
      last = long(i);
      ++z;
    }
  return z > 1 ? double(z - 1) * double(sr) / double(last - first) : 0.0;
}
void testTricks() {
  const float sr = 48000.0f;
  fresh(sr, 120, false);
  const long bar = barFrames(sr, 120);
  std::vector<int16_t> tone(size_t(bar * 4));
  for (size_t i = 0; i < tone.size(); ++i) tone[i] = int16_t(std::lround(6000.0 * std::sin(2.0 * M_PI * 300.0 * double(i) / double(sr))));
  check(g_tape.write(0, 0, tone.data(), uint32_t(tone.size())), "tone written");
  g_tape.setBpm(120);
  tc(tape::kOpLoop, 0, 4);
  run(64);
  // the reference: no trick
  std::vector<int16_t> ref, tr;
  post(cmd(CmdType::Play));
  run(bar * 3, &ref);
  post(cmd(CmdType::Stop));
  run(4800);
  const int natural = maxStep(ref, 1000, size_t(bar));
  struct T { uint8_t trick; const char* name; double want; };
  const T kT[] = {{tape::kTrickHalf, "HALF", 150.0}, {tape::kTrickReverse, "REVERSE", 300.0}, {tape::kTrickBrake, "BRAKE", -1.0}};
  for (const T& t : kT) {
    tr.clear();
    post(cmd(CmdType::Play));
    run(bar / 2, &tr);
    tc(tape::kOpTrick, 0, t.trick);
    run(bar / 2, &tr);
    tc(tape::kOpTrick, 0, tape::kTrickNone);
    run(bar * 2, &tr);
    post(cmd(CmdType::Stop));
    run(4800);
    const size_t p = size_t(bar / 2), r = size_t(bar);
    if (t.want > 0) {
      const double f = freqOf(tr, p + 2000, r - 100, sr);
      check(std::fabs(f - t.want) < 2.0, "%s: %.1f Hz while held (want %.0f)", t.name, f, t.want);
    } else {
      // BRAKE: falling pitch, then stopped (silence)
      const double f1 = freqOf(tr, p + 500, p + 5000, sr), f2 = freqOf(tr, p + 12000, p + 20000, sr);
      check(f1 > f2 && f2 > 0.0 && f1 < 300.5, "BRAKE: the pitch falls (%.1f -> %.1f Hz)", f1, f2);
      int late = 0;
      for (size_t i = p + size_t(0.7f * sr); i < r; ++i) late = std::max(late, std::abs(int(tr[2 * i])));
      check(late < 30, "BRAKE: stopped after 0.6 s (%d)", late);
    }
    // back in sync: after the release's transition the output is the reference's
    const size_t after = r + size_t(0.3f * sr) + 512;
    int d = 0;
    for (size_t i = after; i < tr.size() / 2 && i < ref.size() / 2; ++i) d = std::max(d, std::abs(int(tr[2 * i]) - int(ref[2 * i])));
    check(d <= 2, "%s: back where it would have been after the release (max %d)", t.name, d);
    const int sp = maxStep(tr, p - 200, p + 600), sr2 = maxStep(tr, r - 200, after);
    check(sp <= natural * 3 / 2 + 2 && sr2 <= natural * 3 / 2 + 2, "%s: no click at the transitions (%d, %d; the tone %d)",
          t.name, sp, sr2, natural);
  }
}

// ---- sparse storage, out of memory, end of the tape --------------------------------------
void testStorage() {
  const float sr = 48000.0f;
  fresh(sr, 120, true);
  const uint32_t free0 = g_tape.freeBlocks();
  // silence costs nothing: a take of nothing leaves no block in use
  tc(tape::kOpLoop, 0, 4);
  take(0, tape::kSrcTrack, 3, 2);   // track 3 has no steps: silence
  run(4800);
  check(g_tape.trackEmpty(0), "a silent take uses no block");
  check(g_tape.freeBlocks() == free0, "all blocks free again (%u of %u)", g_tape.freeBlocks(), free0);
  // a tiny tape: 8 blocks (2.7 s at 48 kHz), a 32-bar loop of MIX at 120 BPM (64 s)
  fresh(sr, 120, true, 0, false, tape::Tape::bytesFor(3, 48000));
  stepOn(0, 0);
  stepOn(0, 4);
  check(g_tape.blocks() >= 8 && g_tape.blocks() < 12, "a small tape (%u blocks)", g_tape.blocks());
  tc(tape::kOpLoop, 0, 32);
  g_tape.prepareTake();
  tc(tape::kOpArm, 0, tape::kSrcTrack, 0);
  check(runUntil([](const tape::View& v) { return v.rec == tape::Rec::Rec; }), "the take starts");
  check(runUntil([](const tape::View& v) { return v.rec != tape::Rec::Rec; }), "the take ends by itself");
  check(idleNow(), "merged after running out");
  check(g_tape.view().why == tape::kEndFull, "why: MEMORY FULL (%s)", tape::endWhyName(g_tape.view().why));
  const long bar = barFrames(sr, 120);
  check(tapeAt(0, 0) == 8192 && tapeAt(0, uint32_t(bar / 4)) == 8192, "what was recorded is intact");
  check(g_tape.undo() && g_tape.trackEmpty(0), "and undoable");
  post(cmd(CmdType::Stop));
  run(64);
}

// ---- song mode ----------------------------------------------------------------------------
void testSong() {
  const float sr = 48000.0f;
  fresh(sr, 120, true);
  stepOn(0, 0);
  // pattern 1: 32 steps with impulses on 0 and 16 (two bars)
  proj().patterns[1].length = 32;
  for (int t = 0; t < kNumTracks; ++t) proj().patterns[1].tracks[t].length = 32;
  {
    Step& s = proj().patterns[1].tracks[0].steps[16];
    s = kDefaultStep;
    s.setFlags(kStepOn);
    Step& s0 = proj().patterns[1].tracks[0].steps[0];
    s0 = kDefaultStep;
    s0.setFlags(kStepOn);
  }
  proj().song[0] = SongSlot{0, 2};
  proj().song[1] = SongSlot{1, 1};
  proj().song[2] = SongSlot{0, 0};
  post(cmd(CmdType::SetSongMode, 0, 0, 1));
  run(64);
  // the whole song from stop (count-in, then the top): 4 bars
  g_tape.prepareTake();
  tc(tape::kOpArm, 0, tape::kSrcTrack, 0);
  check(runUntil([](const tape::View& v) { return v.rec == tape::Rec::Rec; }), "song take starts");
  check(g_tape.view().song, "song mode");
  const long bar = barFrames(sr, 120);
  run(bar * 3 + bar / 2);
  tc(tape::kOpEnd);
  check(idleNow(), "song take merged");
  if (std::getenv("TAPE_DEBUG")) {
    std::vector<int16_t> all(size_t(6 * bar));
    g_tape.read(0, 0, all.data(), uint32_t(all.size()));
    for (size_t i = 0; i < all.size(); ++i) if (all[i]) std::printf("song tape[%zu] = %d\n", i, all[i]);
    std::printf("why %s end %u\n", tape::endWhyName(g_tape.view().why), g_tape.trackEnd(0));
  }
  for (int b = 0; b < 4; ++b) check(tapeAt(0, uint32_t(b * bar)) == 8192, "song bar %d on tape at its song position", b);
  check(tapeAt(0, uint32_t(4 * bar)) == 0, "nothing at bar 4");
  // the song wraps: the tape goes back to its start with it
  post(cmd(CmdType::Stop));
  run(4800);
  post(cmd(CmdType::SetMute, 0, 0, 1));
  run(4800);
  std::vector<int16_t> out;
  post(cmd(CmdType::Play));
  run(bar * 8, &out);
  const std::vector<long> o = onsets(out, 100);
  check(o.size() == 8, "8 bars of the song played from the tape (%zu)", o.size());
  for (size_t i = 0; i < o.size(); ++i) check(o[i] == long(i) * bar, "song bar %zu from the tape at %ld (want %ld)", i, o[i], long(i) * bar);
}

// ---- LINE ------------------------------------------------------------------------------
// The input is the engine's own track 0 (the impulses) heard `lat` frames late, as a
// microphone in front of the speaker would hear it after the platform's round trip.
void testLine() {
  const float sr = 48000.0f;
  fresh(sr, 120, true);
  stepOn(0, 0);
  stepOn(0, 8);
  tc(tape::kOpLoop, 0, 1);
  const int lat = 37 * 16;   // frames of the delay line below
  // the round trip: the delay line plus the block it arrives after (the test feeds the
  // input after each 64-frame process() call, as a capture callback would)
  const int trip = lat + 64;
  Command l = tape::tapeCommand(tape::kOpLatency, 0, 0, trip & 0xFF);
  l.step.vel = uint8_t(trip >> 8);
  post(l);
  post(cmd(CmdType::Play));
  run(2000);
  g_tape.prepareTake();
  tc(tape::kOpArm, 1, tape::kSrcLine, 0);
  // the platform's input thread: here, the output delayed by lat frames, 64 at a time
  std::vector<int16_t> delay(size_t(lat), 0);
  int16_t buf[128];
  bool started = false;
  long frames = 0;
  for (long f = 0; f < barFrames(sr, 120) * 4 && !(started && g_tape.idle()); f += 64) {
    eng().process(buf, 64);
    g_tape.service();
    int16_t in[64];
    for (int i = 0; i < 64; ++i) {
      delay.push_back(buf[2 * i]);
      in[i] = delay.front();
      delay.erase(delay.begin());
    }
    g_tape.lineIn(in, 64);
    if (g_tape.view().rec == tape::Rec::Rec) started = true;
    if (started && g_tape.view().rec == tape::Rec::Rec && frames == 0) {
      frames = 1;
      tc(tape::kOpEnd);
    }
  }
  check(started && idleNow(), "LINE take done");
  check(!g_tape.trackEmpty(1), "LINE: on tape");
  if (std::getenv("TAPE_DEBUG")) {
    std::vector<int16_t> all(size_t(barFrames(sr, 120)));
    g_tape.read(1, 0, all.data(), uint32_t(all.size()));
    int shown = 0;
    for (size_t i = 0; i < all.size() && shown < 20; ++i) if (std::abs(all[i]) > 500) { std::printf("line tape[%zu] = %d\n", i, all[i]); ++shown; }
  }
  int best = 0;
  long at = -99;
  for (int d = -40; d <= 40; ++d) {
    const int v = std::abs(int(tapeAt(1, uint32_t(int64_t(barFrames(sr, 120) / 2) + d))));
    if (v > best) { best = v; at = d; }
  }
  check(best > 1000 && std::labs(at) <= 1, "LINE: the step 9 impulse where it was played (offset %ld, %d)", at, best);
  check(g_tape.lineUnderruns() == 0, "LINE: no underrun (%u)", g_tape.lineUnderruns());
}

// ---- tempo ------------------------------------------------------------------------------
void testTempo() {
  const float sr = 48000.0f;
  fresh(sr, 120, true);
  stepOn(0, 0);
  tc(tape::kOpLoop, 0, 1);
  take(0, tape::kSrcTrack, 0, 1);
  check(g_tape.bpm() == 120, "the tape's tempo is the first take's (%u)", g_tape.bpm());
  post(cmd(CmdType::SetGlobal, kGTempo, 0, 100));
  run(64);
  check(g_tape.bpm() == 120, "a tempo change does not move it");
  // at 100 BPM each bar plays from its own start: the bar's impulse on the bar line
  post(cmd(CmdType::Stop));
  run(4800);
  post(cmd(CmdType::SetMute, 0, 0, 1));
  run(4800);
  std::vector<int16_t> out;
  post(cmd(CmdType::Play));
  run(barFrames(sr, 100) * 3, &out);
  const std::vector<long> o = onsets(out, 100);
  check(o.size() == 3, "3 bars at 100 BPM (%zu)", o.size());
  for (size_t i = 0; i < o.size(); ++i)
    check(std::labs(o[i] - long(i) * barFrames(sr, 100)) <= 1, "bar %zu at 100 BPM: on its bar line (%ld)", i,
          o[i] - long(i) * barFrames(sr, 100));
  check(g_tape.undo() && g_tape.bpm() == 0, "undoing the first take forgets the tempo");
}

// ---- files ------------------------------------------------------------------------------
// A tape with two tracks of audio and set values: saved in small pieces (many step()
// calls), loaded back into a fresh tape sample for sample with its settings; a project
// folder without a tape loads an empty tape; a tape of another rate is refused.
void testFiles() {
  const float sr = 48000.0f;
  fresh(sr, 120, false);
  std::vector<int16_t> a(200000), b(70000);
  for (size_t i = 0; i < a.size(); ++i) a[i] = int16_t(int((i * 37) % 20000) - 10000);
  for (size_t i = 0; i < b.size(); ++i) b[i] = int16_t(i % 300 == 0 ? 12000 : 0);
  check(g_tape.write(0, 1000, a.data(), uint32_t(a.size())) && g_tape.write(2, 500000, b.data(), uint32_t(b.size())),
        "files: tape written");
  g_tape.setBpm(97);
  g_tape.setLoop(8);
  g_tape.setParam(2, tape::kPPan, 20);
  g_tape.setParam(0, tape::kPMute, 1);
  pgtest::MemFs fs;
  tape::TapeFiles io;
  check(io.startSave(&fs, "/int/pg/SONG1", &g_tape), "files: save starts");
  int steps = 0;
  tape::TapeFiles::State st = tape::TapeFiles::State::Saving;
  while (st == tape::TapeFiles::State::Saving && steps < 10000) { st = io.step(5000); ++steps; }
  check(st == tape::TapeFiles::State::Done && steps > 50, "files: saved in pieces (%d steps)", steps);
  check(fs.exists("/int/pg/SONG1/tape.txt") && fs.exists("/int/pg/SONG1/track1.wav") &&
        fs.exists("/int/pg/SONG1/track3.wav") && !fs.exists("/int/pg/SONG1/track2.wav"), "files: tape.txt, track1, track3");
  // into another tape
  std::vector<uint8_t> mem2(tape::Tape::bytesFor(60, 48000));
  static tape::Tape t2;
  check(t2.init(mem2.data(), mem2.size(), 48000), "files: second tape");
  check(io.startLoad(&fs, "/int/pg/SONG1", &t2), "files: load starts");
  steps = 0;
  st = tape::TapeFiles::State::Loading;
  while (st == tape::TapeFiles::State::Loading && steps < 10000) { st = io.step(5000); ++steps; }
  check(st == tape::TapeFiles::State::Done && io.loaded(), "files: loaded (%s)", io.why());
  std::vector<int16_t> ra(a.size() + 2000), rb(b.size());
  t2.read(0, 0, ra.data(), uint32_t(ra.size()));
  t2.read(2, 500000, rb.data(), uint32_t(rb.size()));
  const bool same = std::memcmp(ra.data() + 1000, a.data(), a.size() * 2) == 0 && ra[999] == 0;
  check(same, "files: track 1 sample for sample");
  // the trailing silence of track 3 is not written: its last sound ends it
  size_t lastNz = 0;
  for (size_t i = 0; i < b.size(); ++i)
    if (b[i]) lastNz = i;
  check(std::memcmp(rb.data(), b.data(), (lastNz + 1) * 2) == 0, "files: track 3 sample for sample");
  check(t2.trackEmpty(1) && t2.trackEmpty(3), "files: the empty tracks stay empty");
  check(t2.bpm() == 97 && t2.loopBars() == 8 && t2.param(2, tape::kPPan) == 20 && t2.param(0, tape::kPMute) == 1,
        "files: the settings");
  // a project without a tape folder: empty
  check(io.startLoad(&fs, "/int/pg/OTHER", &t2) && io.step() == tape::TapeFiles::State::Done && t2.empty() && !io.loaded(),
        "files: no folder, an empty tape");
  // another rate: refused, empty
  std::vector<uint8_t> mem3(tape::Tape::bytesFor(60, 44100));
  static tape::Tape t3;
  t3.init(mem3.data(), mem3.size(), 44100);
  check(!io.startLoad(&fs, "/int/pg/SONG1", &t3) && std::strcmp(io.why(), "TAPE IS 48K: NOT LOADED") == 0 && t3.empty(),
        "files: a 48 kHz tape is refused at 44.1 kHz (%s)", io.why());
  io.step();
  // saving an empty tape over it removes the tracks' files
  check(io.startSave(&fs, "/int/pg/SONG1", &t3), "files: an empty save");
  while (io.step() == tape::TapeFiles::State::Saving) {
  }
  check(!fs.exists("/int/pg/SONG1/track1.wav") && fs.exists("/int/pg/SONG1/tape.txt"), "files: the empty tape's save");
}

}  // namespace

int main() {
  testEmptyIdentical();
  testAlignment(48000.0f, tape::kSrcTrack);
  testAlignment(48000.0f, tape::kSrcMix);
  testAlignment(44100.0f, tape::kSrcTrack);
  testAlignment(44100.0f, tape::kSrcMix);
  testOverdubReplaceUndo();
  testBounce(false);
  testBounce(true);
  testTricks();
  testStorage();
  testSong();
  testLine();
  testTempo();
  testFiles();
  eng().setTape(nullptr);
  return it::finish("itest_tape");
}
