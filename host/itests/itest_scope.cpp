// The instrument page's picture: the engine's scope tap (engine.h "SCOPE TAP")
// and the UI's Scope (lib/pg/src/pg/ui/scope.h), with the real voices.
//   1. off by default: notes play, nothing is written (the ring's end stays put);
//   2. the ring delivers the tapped track's own voice: sample for sample it is the
//      stem tap's dry signal of that track divided by the track's (constant) gain,
//      every sample at decimation 1, the mean of each 4 at 4;
//   3. a note-on of the tapped track marks the ring (mark = end at the note,
//      hits counts, note is the note);
//   4. switching track is clean: readScope reports nothing written before the
//      switch, and what it reports is the new track's signal, not the old one's;
//   5. the tap never changes the audio: a demo rendered with the bass, the kick
//      and the lead tapped (and switched mid-render) is bit-identical to one
//      rendered with the tap off;
//   6. Scope on a steady tone (bass and lead held notes, several registers):
//      successive frames are nearly identical (the trigger holds the picture
//      still), and the picture uses the box (auto-gain);
//   7. Scope on drums: a kick hit draws from its note-on and fills the width as
//      it plays; a quiet closed hat still fills most of the box's height;
//      silence settles to the calm line;
//   8. the UI: the tap runs only while the instrument page is shown (GROOVE:
//      nothing written), it follows the selected track, and a frame in which
//      only the picture moved pushes only the picture's band (dirtyRows).
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/ui_harness.h"
#include "itests/itest_util.h"
#include "pg/demo/demo.h"
#include "pg/engine/stem_tap.h"
#include "pg/ui/scope.h"

using namespace pg;

namespace {

Instrument& inst() { return uih::Rig::instrument(); }
constexpr float kSr = 44100.0f;

Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}

void render(int frames) {
  static int16_t buf[2 * 512];
  while (frames > 0) {
    const int n = frames > 512 ? 512 : frames;
    inst().process(buf, n);
    frames -= n;
  }
}

// Renders with the stem tap capturing every track's dry signal into L buffers.
struct Stems {
  static constexpr long kCap = 1 << 16;
  std::vector<float> L[kNumTracks], R[kNumTracks];
  engine_detail::StemTap tap;
  Stems() {
    for (int t = 0; t < kNumTracks; ++t) {
      L[t].assign(kCap, 0.0f);
      R[t].assign(kCap, 0.0f);
      tap.track[t][0] = L[t].data();
      tap.track[t][1] = R[t].data();
    }
    tap.capacity = kCap;
  }
};

// 2: scope samples [end - n, end) against the stems at frames: ring sample j was
// stem frame f0 + j * d for the samples written since the tap started at f0
// (the voice sounds throughout, so the ring advances every quantum).
void checkAgainstStem(const char* what, const float* x, int n, const ScopeInfo& in, const std::vector<float>& stem,
                      long f0, int d) {
  // the track's gain: the ratio at the largest sample
  int big = 0;
  for (int i = 0; i < n; ++i) if (std::fabs(x[i]) > std::fabs(x[big])) big = i;
  const long fb = f0 + long(in.end - uint32_t(n) + uint32_t(big)) * d;
  it::check(std::fabs(x[big]) > 1.0e-3f, "%s: the tapped voice sounds (peak %g)", what, double(x[big]));
  float sb = 0.0f;
  for (int k = 0; k < d; ++k) sb += stem[size_t(fb + k)];
  const float g = sb / float(d) / x[big];
  it::check(g > 0.05f && g < 4.0f, "%s: a plausible track gain (%g)", what, double(g));
  int bad = 0;
  for (int i = 0; i < n; ++i) {
    const long f = f0 + long(in.end - uint32_t(n) + uint32_t(i)) * d;
    float want = 0.0f;   // the mean of the d frames it stands for
    for (int k = 0; k < d; ++k) want += stem[size_t(f + k)] / g;
    want *= 1.0f / float(d);
    if (std::fabs(x[i] - want) > 1.0e-5f + 1.0e-4f * std::fabs(want)) ++bad;
  }
  it::check(bad == 0, "%s: %d of %d samples differ from the voice's own signal", what, bad, n);
}

void testTap() {
  std::printf("the tap: off by default, the voice's signal, marks, switching\n");
  inst().init(kSr, false);
  Engine& e = inst().engine();
  float x[kScopeReadMax];
  ScopeInfo in;
  // 1. off: a bass note plays, nothing is written
  e.post(cmd(CmdType::NoteOn, kTrackBass, 45, 100));
  render(4096);
  int n = e.readScope(x, kScopeReadMax, &in);
  it::check(n == 0 && in.end == 0 && in.track == 0xFF, "off by default: nothing written (n %d end %u)", n, in.end);
  // 2. on, decimation 1: the bass's own signal
  Stems st;
  engine_detail::setStemTap(&st.tap);
  e.setScope(kTrackBass, 1);
  render(16);   // the request is picked up at the next quantum
  const long f0 = st.tap.pos - 16;   // the first quantum tapped
  render(4096 - 16);
  n = e.readScope(x, kScopeReadMax, &in);
  it::check(in.track == kTrackBass && in.decimation == 1 && in.sampleRate == kSr, "the tap reports its settings");
  it::check(n == kScopeReadMax && in.end - in.since == 4096u, "decimation 1: a sample per frame (end %u since %u)",
            in.end, in.since);
  checkAgainstStem("bass d1", x, n, in, st.L[kTrackBass], f0 - long(in.since), 1);
  // decimation 4: every 4th
  st.tap.pos = 0;
  e.setScope(kTrackBass, 4);
  render(16);
  const long g0 = st.tap.pos - 16;
  render(4096 - 16);
  n = e.readScope(x, kScopeReadMax, &in);
  it::check(in.decimation == 4 && in.end - in.since == 1024u, "decimation 4: the mean of each 4 frames (%u)",
            in.end - in.since);
  checkAgainstStem("bass d4", x, n, in, st.L[kTrackBass], g0 - long(in.since) * 4, 4);
  // 3. a note-on marks the ring
  const uint32_t hits0 = in.hits;
  e.post(cmd(CmdType::NoteOn, kTrackBass, 52, 100));
  render(16);
  ScopeInfo a;
  e.readScope(x, 0, &a);
  it::check(a.hits == hits0 + 1 && a.note == 52, "a note-on counts (hits %u -> %u) with its note (%d)", hits0, a.hits,
            a.note);
  it::check(a.mark + 4 == a.end, "the mark is where the note starts (mark %u, end %u)", a.mark, a.end);
  e.post(cmd(CmdType::NoteOff, kTrackBass, 52));
  e.post(cmd(CmdType::NoteOff, kTrackBass, 45));
  // 4. switch to the lead: nothing from before, then the lead's signal
  e.post(cmd(CmdType::NoteOn, kTrackLead, 69, 100));
  render(512);
  st.tap.pos = 0;
  e.setScope(kTrackLead, 1);
  n = e.readScope(x, kScopeReadMax, &in);   // before the engine took it: still the bass, nothing to read
  it::check(in.track == kTrackBass && n == 0, "right after the switch nothing is reported (n %d)", n);
  render(16);
  n = e.readScope(x, kScopeReadMax, &in);
  it::check(n == 16 && in.since + 16 == in.end && in.hits == 0, "one quantum after: only the new samples (n %d)", n);
  render(1024 - 16);
  n = e.readScope(x, kScopeReadMax, &in);
  checkAgainstStem("lead after a switch", x, n, in, st.L[kTrackLead], 0 - long(in.since), 1);
  // off again: nothing more is written
  e.setScope(-1, 1);
  render(32);
  e.readScope(x, 0, &a);
  render(2048);
  ScopeInfo b;
  n = e.readScope(x, kScopeReadMax, &b);
  it::check(b.end == a.end && n == 0 && b.track == 0xFF, "off: the ring stops (end %u -> %u)", a.end, b.end);
  engine_detail::setStemTap(nullptr);
}

// 5. bit-identical audio with the tap on and off
uint64_t renderDemo(bool tap) {
  inst().init(kSr, false);
  Engine& e = inst().engine();
  e.suspend();
  demo::build(e, inst().project(), 0);
  e.sanitizeProject(inst().project());
  e.resume();
  e.post(cmd(CmdType::Play));
  uint64_t h = 1469598103934665603ull;
  static int16_t buf[2 * 256];
  const int tracks[4] = {kTrackBass, kTrackKick, kTrackLead, kTrackHatClosed};
  for (int blk = 0; blk < 600; ++blk) {
    if (tap && blk % 150 == 0) e.setScope(tracks[(blk / 150) % 4], 1 + (blk / 150) * 7);
    inst().process(buf, 256);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
    for (size_t i = 0; i < sizeof(buf); ++i) h = (h ^ p[i]) * 1099511628211ull;
    if (tap) {
      float x[kScopeReadMax];
      ScopeInfo in;
      e.readScope(x, kScopeReadMax, &in);
    }
  }
  e.setScope(-1, 1);
  return h;
}

void testAudioUnchanged() {
  std::printf("the tap does not change the audio\n");
  const uint64_t off = renderDemo(false), on = renderDemo(true);
  it::check(off == on, "demo render with the tap on/off: %016llx vs %016llx", (unsigned long long)off,
            (unsigned long long)on);
}

// Runs the engine for one UI frame (33 ms) and updates the Scope.
bool frame(ui::Scope& s, int track, uint32_t* now) {
  render(1456);   // 91 quanta, about 33 ms
  *now += 33;
  return s.update(inst().engine(), track, *now);
}

float frameDiff(const int8_t* a, const int8_t* b) {
  float d = 0.0f;
  for (int c = 0; c < ui::Scope::kCols; ++c) d += std::fabs(float(a[c]) - float(b[c]));
  return d / float(ui::Scope::kCols) / 127.0f;
}

void steady(int track, int note, const char* what) {
  inst().init(kSr, false);
  ui::Scope s;
  uint32_t now = 1000;
  if (track == kTrackLead) {   // one plain saw: the default's detuned second oscillator beats
    inst().engine().post(cmd(CmdType::SetTrackParam, track, 6, 0));    // DET
    inst().engine().post(cmd(CmdType::SetTrackParam, track, 18, 0));   // MIX: oscillator 1 only
  }
  inst().engine().post(cmd(CmdType::NoteOn, track, note, 100));
  for (int i = 0; i < 20; ++i) frame(s, track, &now);   // settle: decimation, gain
  int8_t prev[ui::Scope::kCols];
  std::memcpy(prev, s.trace(), sizeof(prev));
  float worst = 0.0f;
  int peak = 0;
  for (int i = 0; i < 30; ++i) {
    frame(s, track, &now);
    const float d = frameDiff(prev, s.trace());
    worst = d > worst ? d : worst;
    std::memcpy(prev, s.trace(), sizeof(prev));
    for (int c = 0; c < ui::Scope::kCols; ++c) peak = std::abs(s.trace()[c]) > peak ? std::abs(s.trace()[c]) : peak;
  }
  std::printf("  %-22s note %3d: worst frame-to-frame change %.3f of full height, peak %d/127\n", what, note,
              double(worst), peak);
  it::check(s.filled() == ui::Scope::kCols && s.level() > 0.99f, "%s %d: a whole, live picture", what, note);
  it::check(worst < 0.05f, "%s %d: successive frames nearly identical (worst mean change %.3f)", what, note,
            double(worst));
  it::check(peak > 80 && peak <= 127, "%s %d: the picture fills the box (peak %d)", what, note, peak);
  // the window shows about kCycles periods: count rising zero crossings of the trace
  int ups = 0;
  for (int c = 1; c < ui::Scope::kCols; ++c) ups += (s.trace()[c - 1] < 0 && s.trace()[c] >= 0) ? 1 : 0;
  it::check(ups >= 1 && ups <= 8, "%s %d: a few cycles across the width (%d rising crossings)", what, note, ups);
  // silence: the note off, the picture settles to the line
  inst().engine().post(cmd(CmdType::NoteOff, track, note));
  for (int i = 0; i < 40; ++i) frame(s, track, &now);
  it::check(s.level() == 0.0f, "%s %d: silence settles to the calm line (level %g)", what, note, double(s.level()));
  s.stop(inst().engine());
}

// The UI's cost per frame (host): Scope::update on a lead chord, the engine not counted.
void timeUpdate() {
  inst().init(kSr, false);
  ui::Scope s;
  uint32_t now = 1000;
  inst().engine().post(cmd(CmdType::NoteOn, kTrackLead, 60, 100));
  frame(s, kTrackLead, &now);
  double us = 0.0;
  for (int i = 0; i < 200; ++i) {
    render(1456);
    now += 33;
    const auto t0 = std::chrono::steady_clock::now();
    s.update(inst().engine(), kTrackLead, now);
    us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  }
  std::printf("  Scope::update on this host: %.1f us per frame (lead chord)\n", us / 200.0);
  s.stop(inst().engine());
}

void testSteady() {
  std::printf("a steady tone stands still\n");
  for (int note : {28, 36, 48, 60}) steady(kTrackBass, note, "bass (CLASSIC)");
  for (int note : {48, 60, 72, 84}) steady(kTrackLead, note, "lead (CLASSIC, one saw)");
}

void testDrums() {
  std::printf("drums: the hit from its note-on\n");
  inst().init(kSr, false);
  ui::Scope s;
  uint32_t now = 1000;
  frame(s, kTrackKick, &now);
  render(32);   // the engine takes the tap at its next quantum
  inst().engine().post(cmd(CmdType::NoteOn, kTrackKick, 60, 120));
  render(16 * 30);   // ~11 ms: the hit has started
  now += 11;
  s.update(inst().engine(), kTrackKick, now);
  const int early = s.filled();
  it::check(early > 0 && early < ui::Scope::kCols / 2, "kick: the picture grows from the left (%d columns)", early);
  int peakEarly = 0;
  for (int c = 0; c < 8; ++c) peakEarly = std::abs(s.trace()[c]) > peakEarly ? std::abs(s.trace()[c]) : peakEarly;
  it::check(peakEarly > 40, "kick: the hit's start is at the left edge (%d)", peakEarly);
  for (int i = 0; i < 12; ++i) frame(s, kTrackKick, &now);
  it::check(s.filled() == ui::Scope::kCols, "kick: the whole window after it played (%d)", s.filled());
  for (int i = 0; i < 60; ++i) frame(s, kTrackKick, &now);
  it::check(s.level() == 0.0f, "kick: the picture fades once the drum is quiet (level %g)", double(s.level()));
  s.stop(inst().engine());
  // the closed hat: quiet, still a clear shape
  ui::Scope h;
  frame(h, kTrackHatClosed, &now);
  render(32);
  inst().engine().post(cmd(CmdType::NoteOn, kTrackHatClosed, 60, 100));
  for (int i = 0; i < 4; ++i) frame(h, kTrackHatClosed, &now);
  int peak = 0;
  for (int c = 0; c < ui::Scope::kCols; ++c) peak = std::abs(h.trace()[c]) > peak ? std::abs(h.trace()[c]) : peak;
  it::check(h.filled() > ui::Scope::kCols / 2 && peak > 80, "hat: a clear shape (filled %d, peak %d)", h.filled(),
            peak);
  h.stop(inst().engine());
}

// 8. through the UI
void testUi() {
  std::printf("the UI: only on the instrument page, the selected track, a compact band\n");
  static uih::Rig rig;
  rig.devMode = false;
  rig.start(false);
  rig.ui.setGhostFilter(false);
  auto tapKey = [&](const char* k) { rig.tap(ui::keyFromName(k), 40); rig.run(20); };
  // a new project with its instruments; a bass line posted into the pattern
  rig.down(ui::keyFromName("ctl"));
  tapKey("n");
  rig.up(ui::keyFromName("ctl"));
  rig.run(1600);
  for (int i = 0; i < 16; ++i) {
    Command c = cmd(CmdType::SetStep, rig.ui.editPattern(), kTrackBass, i);
    c.step = kDefaultStep;
    c.step.flags = kStepOn;
    c.step.note = uint8_t(36 + (i % 3) * 5);
    rig.eng.post(c);
  }
  rig.down(ui::keyFromName("shift"));
  tapKey("j");
  rig.up(ui::keyFromName("shift"));
  tapKey("spc");
  rig.run(500);
  float x[kScopeReadMax];
  ScopeInfo in, in2;
  rig.eng.readScope(x, 0, &in);
  rig.run(500);
  rig.eng.readScope(x, 0, &in2);
  it::check(in.track == 0xFF && in2.end == in.end, "GROOVE: the engine taps nothing (track %d, end %u -> %u)",
            in.track, in.end, in2.end);
  tapKey("9");
  rig.run(600);
  rig.eng.readScope(x, 0, &in);
  it::check(in.track == kTrackBass && in.end > 0, "SOUND: the selected track is tapped (%d, end %u)", in.track,
            in.end);
  it::check(rig.ui.scope().track() == kTrackBass && rig.ui.scope().filled() > 0, "the page has a picture");
  // frames where only the picture moved push only its band
  int band = 0, full = 0, other = 0;
  for (int i = 0; i < 60; ++i) {
    rig.run(33);
    int y0, y1;
    rig.ui.dirtyRows(&y0, &y1);
    if (y0 == ui::kScopeY0 && y1 == ui::kScopeY1) ++band;
    else if (y0 == 0 && y1 == kScreenH - 1) ++full;
    else ++other;
  }
  std::printf("  60 frames playing: %d picture-band pushes, %d full, %d other\n", band, full, other);
  it::check(band > 10, "frames that only moved the picture push its band (%d of 60)", band);
  // the lead selected: the tap follows; GROOVE again: the tap stops
  rig.down(ui::keyFromName("shift"));
  tapKey("k");
  rig.up(ui::keyFromName("shift"));
  rig.run(100);
  rig.eng.readScope(x, 0, &in);
  it::check(in.track == kTrackLead, "a new selection is tapped (%d)", in.track);
  tapKey("`");
  rig.run(100);
  rig.eng.readScope(x, 0, &in);
  const uint32_t end = in.end;
  rig.run(500);
  rig.eng.readScope(x, 0, &in);
  it::check(in.track == 0xFF && in.end == end, "back in GROOVE: the tap is off (track %d)", in.track);
  tapKey("spc");
}

}  // namespace

int main() {
  testTap();
  testAudioUnchanged();
  testSteady();
  timeUpdate();
  testDrums();
  testUi();
  return it::finish("itest_scope");
}
