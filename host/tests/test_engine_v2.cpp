// Contract v2 features of the engine, with stub voices:
//   Roll (transport-locked grid, free-running when stopped, stop, mute),
//   WriteTrack / WritePattern + writesDone, SetSongMode start slot,
//   status songRepeat / masterStep, record legato -> kStepSlide and
//   NoteOff -> gate, setMonoOutput (L == R, = (L+R)/2 of stereo, Voice::mono()
//   read as L only, pan ignored), the master's 20 Hz rumble high-pass,
//   globals table formats, sanitizeProject.
#include <cmath>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;

static Command noteOnCmd(int track, int note, int vel, int flags = 0) {
  Command c = cmd(CmdType::NoteOn, track, note, vel);
  c.step.flags = uint8_t(flags);
  return c;
}

static Command rollCmd(int track, int c, int note = 60, int vel = 100) {
  Command k = cmd(CmdType::Roll, track, 0, c);
  k.step.note = uint8_t(note);
  k.step.vel = uint8_t(vel);
  return k;
}

static void testRoll() {
  // Playing: 1/16 roll on an empty kick track hits on every 16th grid line.
  rig.init(44100.0f, 120);
  const double L = rig.stepLen();
  rig.post(CmdType::Play);
  rig.runSamples(long(2.3 * L));
  rig.post(rollCmd(kTrackKick, 2, 48, 90));
  rig.runSamples(long(8.0 * L));
  auto ons = rig.v[kTrackKick].ons();
  CHECKF(ons.size() >= 7 && ons.size() <= 9, "1/16 roll: %zu hits in 8 steps", ons.size());
  bool onGrid = true;
  for (const Call& c : ons) {
    const double ph = std::fmod(double(c.t), L);
    onGrid &= ph < 17.0 || L - ph < 17.0;
  }
  CHECK(onGrid);
  if (!ons.empty()) CHECK(ons[0].ev.note == 48 && ons[0].ev.vel == 90 && ons[0].t >= long(3.0 * L) - 16);
  // 1/32: two per step; 1/16T: three per two steps; 1/8: one per two steps (on even steps).
  struct R { int c; double perStep; };
  for (R r : {R{4, 2.0}, R{3, 1.5}, R{1, 0.5}}) {
    rig.v[kTrackKick].clearLog();
    rig.post(rollCmd(kTrackKick, r.c));
    rig.runSamples(long(16.0 * L));
    const size_t n = rig.v[kTrackKick].ons().size();
    CHECKF(std::fabs(double(n) - 16.0 * r.perStep) <= 1.5, "roll c=%d: %zu hits in 16 steps, expected %.0f", r.c, n, 16.0 * r.perStep);
    if (r.c == 1) {
      bool even = true;
      for (const Call& c : rig.v[kTrackKick].ons()) {
        const double s = double(c.t) / L;
        even &= std::fabs(s - 2.0 * std::round(s / 2.0)) < 0.01;
      }
      CHECKF(even, "1/8 roll must sit on even 16ths");
    }
  }
  // The rolling track's own steps are silent; others are not.
  rig.step(0, kTrackKick, 0) = mkStep(30);
  rig.step(0, kTrackSnare, 0) = mkStep(60);
  rig.v[kTrackKick].clearLog(); rig.v[kTrackSnare].clearLog();
  rig.post(rollCmd(kTrackKick, 1, 70));
  rig.runSamples(long(16.0 * L));
  bool seqNote = false;
  for (const Call& c : rig.v[kTrackKick].ons()) seqNote |= c.ev.note == 30;
  CHECK(!seqNote);
  CHECK(rig.v[kTrackSnare].ons().size() >= 1);
  // Stop the roll: the pattern comes back.
  rig.post(rollCmd(kTrackKick, 0));
  rig.v[kTrackKick].clearLog();
  rig.runSamples(long(16.0 * L));
  auto back = rig.v[kTrackKick].ons();
  CHECKF(back.size() == 1 && back[0].ev.note == 30, "after roll stop: %zu kick hits", back.size());
  // Muted track rolls silently; out-of-range c and track are ignored.
  rig.post(CmdType::SetMute, kTrackKick, 0, 1);
  rig.post(rollCmd(kTrackKick, 2));
  rig.post(rollCmd(kTrackSnare, 9));
  rig.post(rollCmd(12, 2));
  rig.v[kTrackKick].clearLog(); rig.v[kTrackSnare].clearLog();
  rig.runSamples(long(8.0 * L));
  CHECK(rig.v[kTrackKick].ons().empty());
  CHECK(rig.v[kTrackSnare].ons().size() <= 1);

  // Stopped: free-running from the command, first hit at once; synth notes
  // get their note-offs even with the transport stopped.
  rig.init(44100.0f, 120);
  rig.run(7);
  const long t0 = rig.time;
  rig.post(rollCmd(kTrackBass, 2, 40));
  rig.runSamples(long(4.0 * L));
  ons = rig.v[kTrackBass].ons();
  CHECKF(ons.size() >= 4 && ons.size() <= 5, "stopped roll: %zu hits in 4 steps", ons.size());
  if (!ons.empty()) CHECK(ons[0].t == t0);
  CHECKF(rig.v[kTrackBass].offs().size() + 1 >= ons.size(), "stopped roll: %zu offs for %zu ons", rig.v[kTrackBass].offs().size(), ons.size());
  rig.post(rollCmd(kTrackBass, 0));
  rig.run(2);
  CHECK(rig.v[kTrackBass].offs().size() == rig.v[kTrackBass].ons().size());
}

static void testWrites() {
  rig.init();
  static TrackPattern tp;
  static Pattern pat;
  std::memset(&tp, 0xEE, sizeof tp);              // garbage: must be sanitised on the way in
  tp.length = 7;
  tp.steps[3] = mkStep(64);
  const uint8_t w0 = rig.eng.status().writesDone;
  Command c = cmd(CmdType::WriteTrack, 2, kTrackBass);
  c.ptr = &tp;
  rig.post(c);
  rig.run(2);
  CHECK(uint8_t(rig.eng.status().writesDone - w0) == 1);
  const TrackPattern& d = rig.proj.patterns[2].tracks[kTrackBass];
  CHECK(d.length == 7 && d.steps[3].note == 64 && (d.steps[3].flags & kStepOn));
  CHECK(d.steps[0].note <= 127 && d.steps[0].prob <= 100 && d.steps[0].chord == 0 && d.steps[0].vel <= 127);
  std::memset(&pat, 0, sizeof pat);
  pat.length = 12;
  for (int t = 0; t < kNumTracks; ++t) { pat.tracks[t].length = 12; pat.tracks[t].steps[0] = mkStep(50 + t); }
  c = cmd(CmdType::WritePattern, 5);
  c.ptr = &pat;
  rig.post(c);
  // Rejected writes (bad address, null pointer) still release the buffer.
  c = cmd(CmdType::WritePattern, 16); c.ptr = &pat; rig.post(c);
  c = cmd(CmdType::WriteTrack, 0, 8); c.ptr = &tp; rig.post(c);
  c = cmd(CmdType::WriteTrack, 0, 0); c.ptr = nullptr; rig.post(c);
  rig.run(6);                                      // pattern writes are drained one per quantum
  CHECKF(uint8_t(rig.eng.status().writesDone - w0) == 5, "writesDone advanced %d", uint8_t(rig.eng.status().writesDone - w0));
  CHECK(rig.proj.patterns[5].length == 12 && rig.proj.patterns[5].tracks[kTrackLead].steps[0].note == 57);
  CHECK(rig.proj.patterns[5].tracks[0].steps[1].vel >= 1);                 // vel 0 sanitised
}

static void testSongStartAndStatus() {
  rig.init(44100.0f, 120);
  const double L = rig.stepLen();
  for (int p = 0; p < 4; ++p) rig.proj.patterns[p].length = 4;
  rig.post(CmdType::SetSongSlot, 0, 0, 1);
  rig.post(CmdType::SetSongSlot, 1, 1, 1);
  rig.post(CmdType::SetSongSlot, 2, 2, 3);
  rig.post(CmdType::SetSongSlot, 3, 3, 0);
  rig.post(CmdType::SetSongMode, 2, 0, 1);       // start from slot 2
  rig.run(2);
  EngineStatus st = rig.eng.status();
  CHECK(st.songSlot == 2 && st.pattern == 2 && st.songRepeat == 0);
  rig.post(CmdType::Play);
  rig.runSamples(long(1.5 * L));
  st = rig.eng.status();
  CHECK(st.pattern == 2 && st.masterStep == 1);
  rig.runSamples(long(4.0 * L));
  st = rig.eng.status();
  CHECKF(st.pattern == 2 && st.songRepeat == 1 && st.masterStep == 1, "pattern %d rep %d step %d", st.pattern, st.songRepeat, st.masterStep);
  rig.runSamples(long(8.0 * L));
  st = rig.eng.status();
  CHECKF(st.pattern == 0 && st.songSlot == 0, "after slot 2 x3 the song loops to slot 0 (pattern %d slot %d)", st.pattern, st.songSlot);
  // Play from the top restarts at the start slot.
  rig.post(CmdType::Play);
  rig.run(3);
  st = rig.eng.status();
  CHECK(st.songSlot == 2 && st.pattern == 2);
  // Out-of-range start slot means 0.
  rig.post(CmdType::Stop);
  rig.post(CmdType::SetSongMode, 77, 0, 1);
  rig.run(2);
  CHECK(rig.eng.status().songSlot == 0);
}

static void testRecordLegatoGate() {
  rig.init(44100.0f, 120);
  const double L = rig.stepLen();
  rig.post(CmdType::SetRecord, 0, 0, 1);
  rig.post(CmdType::Play);
  // Note A on step 2, held into note B on step 4 (legato), B released after 1.5 steps.
  rig.runSamples(long(2.0 * L) + 32);
  rig.post(noteOnCmd(kTrackBass, 45, 100));
  rig.runSamples(long(4.0 * L) + 32 - rig.time);
  rig.post(noteOnCmd(kTrackBass, 48, 100));
  rig.runSamples(long(4.6 * L) - rig.time);
  rig.post(cmd(CmdType::NoteOff, kTrackBass, 45));          // A released late: changes nothing
  rig.runSamples(long(5.5 * L) + 32 - rig.time);
  rig.post(cmd(CmdType::NoteOff, kTrackBass, 48));
  rig.run(2);
  const Step& a = rig.step(0, kTrackBass, 2);
  const Step& b = rig.step(0, kTrackBass, 4);
  CHECKF((a.flags & kStepSlide) && a.note == 45 && a.gate == kDefaultStep.gate, "legato: step A flags %02x gate %d", a.flags, a.gate);
  CHECKF(!(b.flags & kStepSlide) && b.note == 48 && b.gate == 6, "step B flags %02x gate %d (expected 6 quarter-steps)", b.flags, b.gate);
  // Short tap: min 1 quarter-step. Detached notes: no slide.
  rig.runSamples(long(8.0 * L) + 32 - rig.time);
  rig.post(noteOnCmd(kTrackBass, 50, 100));
  rig.run(1);
  rig.post(cmd(CmdType::NoteOff, kTrackBass, 50));
  rig.runSamples(long(10.0 * L) + 32 - rig.time);
  rig.post(noteOnCmd(kTrackBass, 52, 100));
  rig.run(3);
  CHECKF(rig.step(0, kTrackBass, 8).gate == 1 && !(rig.step(0, kTrackBass, 8).flags & kStepSlide), "tap: gate %d", rig.step(0, kTrackBass, 8).gate);
  // Drums: NoteOff never touches the step.
  rig.post(noteOnCmd(kTrackSnare, 60, 100));
  rig.runSamples(long(3.0 * L));
  rig.post(cmd(CmdType::NoteOff, kTrackSnare, 60));
  rig.run(2);
  bool snareGateChanged = false;
  for (int s = 0; s < 16; ++s) snareGateChanged |= rig.step(0, kTrackSnare, s).gate != kDefaultStep.gate;
  CHECK(!snareGateChanged);
}

// Mono device mode with stub voices: L == R, equals (L+R)/2 of stereo for
// centred tracks, pan ignored, mono voices read as L only.
class LOnlyVoice : public StubVoice {
 public:
  bool mono() const override { return true; }
  void render(float* l, float* r, int frames) override {
    StubVoice::render(l, r, frames);
    for (int i = 0; i < frames; ++i) r[i] = 1000.0f;       // garbage the mixer must not read
  }
};
static LOnlyVoice g_lonly;

static void testMono() {
  std::vector<int16_t> out[2];
  for (int m = 0; m < 2; ++m) {
    rig.init();
    rig.neutralMix();
    rig.capture = true;
    for (int t : {int(kTrackSnare), int(kTrackLead)}) { rig.v[t].mode = StubVoice::Noise; rig.v[t].amp = 0.2f; }
    rig.v[kTrackLead].seed = 999u;
    rig.proj.tracks[kTrackSnare].params[kMixDelaySend] = 90;
    rig.proj.tracks[kTrackLead].params[kMixReverbSend] = 100;
    rig.eng.setMonoOutput(m == 1);
    rig.runSeconds(1.0);
    out[m] = rig.out;
  }
  double maxErr = 0.0;
  long lr = 0;
  for (size_t f = 0; f + 1 < out[1].size(); f += 2) {
    if (out[1][f] != out[1][f + 1]) ++lr;
    maxErr = std::fmax(maxErr, std::fabs(double(out[1][f]) - 0.5 * (double(out[0][f]) + double(out[0][f + 1]))));
  }
  std::printf("  mono vs stereo (L+R)/2 with delay + reverb: max error %.1f LSB, L!=R %ld\n", maxErr, lr);
  CHECK(lr == 0);
  CHECKF(maxErr <= 2.0, "mono error %.1f LSB", maxErr);

  // Pan is ignored in mono; a hard-left source plays at its centred level.
  rig.init();
  rig.neutralMix();
  rig.capture = true;
  rig.v[kTrackSnare].mode = StubVoice::Tone;
  rig.v[kTrackSnare].freq = 1000.0f;
  rig.v[kTrackSnare].amp = 0.25f;
  rig.proj.tracks[kTrackSnare].params[kMixPan] = 0;
  rig.eng.setMonoOutput(true);
  rig.runSeconds(0.3);
  CHECKF(std::fabs(peakOf(rig.out, 4410, rig.frames(), 0) - 0.25) < 0.01, "mono pan ignored: peak %.3f", peakOf(rig.out, 4410, rig.frames(), 0));

  // Voice::mono(): R is never read (stereo and mono mode).
  for (int m = 0; m < 2; ++m) {
    rig.init();
    rig.neutralMix();
    rig.capture = true;
    g_lonly = LOnlyVoice();
    g_lonly.mode = StubVoice::Dc;
    g_lonly.amp = 0.3f;
    rig.eng.setVoice(kTrackPerc, &g_lonly);
    rig.eng.setMonoOutput(m == 1);
    rig.runSeconds(0.1);
    CHECKF(std::fabs(double(rig.sample(rig.frames() - 1, 1)) - 0.3) < 0.002 && double(rig.eng.status().peakR) < 0.5,
           "mono() voice: R read (%.3f) mode %d", double(rig.sample(rig.frames() - 1, 1)), m);
    rig.eng.setVoice(kTrackPerc, &rig.v[kTrackPerc]);
  }
}

// The master's 20 Hz rumble / DC high-pass (bypassed in the other rigs).
static void testRumbleFilter() {
  rig.init();
  rig.neutralMix();
  engine_detail::setRumbleFilterBypass(false);
  rig.capture = true;
  rig.v[kTrackSnare].mode = StubVoice::Dc;
  rig.v[kTrackSnare].amp = 0.4f;
  rig.runSeconds(1.0);
  const double dcEnd = std::fabs(double(rig.sample(rig.frames() - 1, 0)));
  CHECKF(dcEnd < 0.001, "DC not removed: %.4f after 1 s", dcEnd);
  for (float f : {10.0f, 45.0f, 100.0f, 1000.0f}) {
    rig.init();
    rig.neutralMix();
    engine_detail::setRumbleFilterBypass(false);
    rig.capture = true;
    rig.v[kTrackSnare].mode = StubVoice::Tone;
    rig.v[kTrackSnare].freq = f;
    rig.v[kTrackSnare].amp = 0.4f;
    rig.runSeconds(1.5);
    const double g = dB(rmsOf(rig.out, rig.frames() / 3, rig.frames(), 0) / (0.4 / std::sqrt(2.0)));
    std::printf("  rumble HP: %6.0f Hz -> %+6.2f dB\n", double(f), g);
    if (f >= 100.0f) CHECKF(std::fabs(g) < 0.1, "%.0f Hz: %.2f dB", double(f), g);
    if (f == 45.0f) CHECKF(g > -0.35 && g < 0.05, "45 Hz (kick): %.2f dB", g);
    if (f == 10.0f) CHECKF(g < -10.0, "10 Hz not attenuated: %.2f dB", g);
  }
  engine_detail::setRumbleFilterBypass(true);
}

static void testTables() {
  CHECK(Engine::globalParamInfo(kGSwing)->fmt == ParamFormat::Swing);
  CHECK(Engine::globalParamInfo(kGMasterFilter)->fmt == ParamFormat::DjFilter);
  for (int i = 0; i < kGNumUsed; ++i) {
    const ParamInfo* p = Engine::globalParamInfo(i);
    if (p->fmt != ParamFormat::Enum) continue;
    for (int v = p->min; v <= p->max; ++v)
      CHECKF(p->enumNames[v - p->min] && std::strlen(p->enumNames[v - p->min]) <= 5, "global %s enum name too long", p->name);
  }
  // sanitizeProject: magic / name / out-of-range fields, valid untouched.
  rig.init();
  static Project p, q;
  rig.eng.initProject(p);
  q = p;
  rig.eng.sanitizeProject(q);
  CHECK(std::memcmp(&p, &q, sizeof p) == 0);
  std::memset(q.name, 'X', sizeof q.name);
  q.magic[0] = 0;
  q.patterns[3].tracks[2].steps[9].lockParam = 31;          // reserved mixer slot: no such param
  q.patterns[3].tracks[2].steps[9].chord = 3;                // not the lead
  q.patterns[3].tracks[7].steps[9].flags = 0xFF;
  q.tracks[1].params[20] = 99;                               // stub voice has no param 20
  rig.eng.sanitizeProject(q);
  CHECK(q.name[15] == 0 && q.magic[0] == 'P');
  CHECK(q.patterns[3].tracks[2].steps[9].lockParam == 0 && q.patterns[3].tracks[2].steps[9].chord == 0);
  CHECK(q.patterns[3].tracks[7].steps[9].flags == (kStepOn | kStepAccent | kStepSlide | kStepRatchetMask));
  CHECK(q.tracks[1].params[20] == 0);
}

int main() {
  std::printf("== roll ==\n");
  testRoll();
  std::printf("== WriteTrack / WritePattern ==\n");
  testWrites();
  std::printf("== song start slot, status ==\n");
  testSongStartAndStatus();
  std::printf("== record legato / gate ==\n");
  testRecordLegatoGate();
  std::printf("== mono output ==\n");
  testMono();
  std::printf("== rumble filter ==\n");
  testRumbleFilter();
  std::printf("== tables / sanitize ==\n");
  testTables();
  return finish("test_engine_v2");
}
