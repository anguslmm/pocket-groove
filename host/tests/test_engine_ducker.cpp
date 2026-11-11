// Sidechain ducker: envelope shape and timing measured on a constant (DC)
// stub voice, trigger sources, per-track amount, FX-return ducking, renders.
#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;
static const int kProbe = kTrackBass;     // DC stub; output / 0.5 = the track's gain

struct Env {
  std::vector<float> g;                   // gain per sample, starting at the first trigger
  float sr;
  float at(double sec) const { return g[size_t(sec * double(sr))]; }
  float minIn(double a, double b) const { float m = 9; for (size_t i = size_t(a * double(sr)); i < size_t(b * double(sr)); ++i) m = std::fmin(m, g[i]); return m; }
  float maxIn(double a, double b) const { float m = -9; for (size_t i = size_t(a * double(sr)); i < size_t(b * double(sr)); ++i) m = std::fmax(m, g[i]); return m; }
  float maxStep() const { float m = 0; for (size_t i = 1; i < g.size(); ++i) m = std::fmax(m, std::fabs(g[i] - g[i - 1])); return m; }
};

struct Setup {
  float sr = 44100.0f;
  int bpm = 124;
  int source = 1, depth = 100, hold = 49, release = 78, curve = 64;
  int amount = 127;
  bool kickMuted = false;
  int kickEvery = 4;                      // kick step spacing; 0 = no kick steps
  int kickProb = 100;
  int kickRatchet = 1;
};

// Runs 0.25 s stopped, then plays; returns the gain trace from the Play instant.
static Env measure(const Setup& s, double seconds = 1.0) {
  rig.init(s.sr, s.bpm);
  rig.neutralMix();
  rig.v[kProbe].mode = StubVoice::Dc;
  rig.v[kProbe].amp = 0.5f;
  rig.proj.tracks[kProbe].params[kMixDuck] = uint8_t(s.amount);
  rig.proj.tracks[kTrackKick].mute = s.kickMuted;
  rig.proj.globals[kGDuckSource] = uint8_t(s.source);
  rig.proj.globals[kGDuckDepth] = uint8_t(s.depth);
  rig.proj.globals[kGDuckHold] = uint8_t(s.hold);
  rig.proj.globals[kGDuckRelease] = uint8_t(s.release);
  rig.proj.globals[kGDuckCurve] = uint8_t(s.curve);
  if (s.kickEvery > 0)
    for (int i = 0; i < 16; i += s.kickEvery)
      rig.step(0, kTrackKick, i) = mkStep(60, kStepOn | ((s.kickRatchet - 1) << kStepRatchetShift), 100, 1, s.kickProb);
  rig.eng.init(s.sr, &rig.proj);
  rig.runSeconds(0.25);
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.runSeconds(seconds);
  rig.capture = false;
  Env e;
  e.sr = s.sr;
  e.g.resize(size_t(rig.frames()));
  for (long f = 0; f < rig.frames(); ++f) e.g[size_t(f)] = rig.sample(f, 0) / 0.5f;
  return e;
}

static void testShape(float sr) {
  Setup s;
  s.sr = sr;
  const Env e = measure(s, 1.2);
  const double floorG = 1.0 - 100.0 / 127.0;
  const double hold = 0.2 * (49.0 / 127.0) * (49.0 / 127.0);
  const double rel = 0.02 * std::pow(50.0, 78.0 / 127.0);
  const double beat = 60.0 / 124.0;
  // Before the trigger: unity. The drop is fast (floor within 3 ms) and click-free.
  CHECKF(std::fabs(double(e.g[0]) - 1.0) < 0.01, "gain at the trigger instant %.4f", double(e.g[0]));
  double tFloor = -1;
  for (size_t i = 0; i < e.g.size(); ++i) if (double(e.g[i]) <= floorG + 0.01) { tFloor = double(i) / double(sr); break; }
  CHECKF(tFloor > 0.0008 && tFloor < 0.003, "attack reaches the floor after %.2f ms (want 1..3 ms)", tFloor * 1000.0);
  for (size_t i = 1; i < size_t(tFloor * double(sr)); ++i) CHECK(e.g[i] <= e.g[i - 1] + 1e-4f);
  CHECKF(e.maxStep() < 0.02f, "largest per-sample gain step %.4f", double(e.maxStep()));
  // Hold: stays at the floor.
  CHECKF(std::fabs(double(e.minIn(0.0, hold)) - floorG) < 0.004, "floor %.4f, expected %.4f", double(e.minIn(0.0, hold)), floorG);
  CHECKF(double(e.maxIn(0.0035, tFloor + hold - 0.001)) < floorG + 0.004, "gain rose during hold: %.4f", double(e.maxIn(0.0035, tFloor + hold - 0.001)));
  // Release: linear by default, monotonic, complete at attack + hold + release.
  const double t0 = tFloor + hold;
  for (double x : {0.25, 0.5, 0.75}) {
    const double expect = floorG + (1.0 - floorG) * x;
    CHECKF(std::fabs(double(e.at(t0 + x * rel)) - expect) < 0.03, "release at %.0f %%: gain %.3f, expected %.3f", x * 100.0, double(e.at(t0 + x * rel)), expect);
  }
  for (size_t i = size_t((t0 + 0.002) * double(sr)); i < size_t((t0 + rel) * double(sr)); ++i) CHECK(e.g[i] >= e.g[i - 1] - 1e-4f);
  CHECKF(double(e.at(t0 + rel + 0.003)) > 0.995, "not recovered after hold+release: %.4f", double(e.at(t0 + rel + 0.003)));
  CHECK(double(e.minIn(t0 + rel + 0.003, beat - 0.001)) > 0.995);
  // Next beat: the same again.
  CHECKF(std::fabs(double(e.at(beat + tFloor + hold * 0.5)) - floorG) < 0.004, "second beat floor %.4f", double(e.at(beat + tFloor + hold * 0.5)));
  std::printf("  sr %.0f: attack %.2f ms to floor %.3f (%.1f dB), hold %.1f ms, release %.0f ms, max step %.4f/sample\n", double(sr),
              tFloor * 1000.0, floorG, dB(floorG), hold * 1000.0, rel * 1000.0, double(e.maxStep()));
  // Status meter follows.
  const EngineStatus st = rig.eng.status();
  CHECK(st.duckGain >= float(floorG) - 0.01f && st.duckGain <= 1.0f);
}

static void testParams() {
  const double floorG = 1.0 - 100.0 / 127.0;
  // Hold and release times at other settings (measured from the trace).
  for (int hold : {0, 64, 127}) {
    for (int release : {0, 64, 127}) {
      Setup s;
      s.bpm = 60; s.kickEvery = 8;                  // 2 s between triggers: room for the longest setting
      s.hold = hold; s.release = release;
      const Env e = measure(s, 1.6);
      const double expHold = 0.2 * (hold / 127.0) * (hold / 127.0);
      const double expRel = 0.02 * std::pow(50.0, release / 127.0);
      double tLeave = 0, tDone = 0;
      for (size_t i = 200; i < e.g.size(); ++i) if (double(e.g[i]) > floorG + 0.005) { tLeave = double(i) / 44100.0; break; }
      for (size_t i = 200; i < e.g.size(); ++i) if (double(e.g[i]) > 0.998) { tDone = double(i) / 44100.0; break; }
      CHECKF(std::fabs(tLeave - 0.0022 - expHold) < 0.004 + expRel * 0.02, "hold %d: left the floor at %.1f ms, expected %.1f", hold, tLeave * 1000.0, (expHold + 0.0022) * 1000.0);
      CHECKF(std::fabs((tDone - 0.0022 - expHold) - expRel) < 0.003 + expRel * 0.03, "release %d: took %.1f ms, expected %.1f", release, (tDone - 0.0022 - expHold) * 1000.0, expRel * 1000.0);
    }
  }
  // Curve: log recovers early, exp recovers late; all three monotonic and complete.
  double mid[3];
  int k = 0;
  for (int curve : {0, 64, 127}) {
    Setup s;
    s.curve = curve;
    const Env e = measure(s, 0.45);
    const double hold = 0.2 * (49.0 / 127.0) * (49.0 / 127.0), rel = 0.02 * std::pow(50.0, 78.0 / 127.0);
    mid[k++] = double(e.at(0.0022 + hold + 0.5 * rel));
    CHECK(double(e.at(0.0022 + hold + rel + 0.003)) > 0.995);
    CHECK(e.maxStep() < 0.02f);
  }
  const double span = 1.0 - floorG;
  CHECKF(std::fabs(mid[0] - (floorG + span * 8.0 / 9.0)) < 0.03 && std::fabs(mid[1] - (floorG + span * 0.5)) < 0.03 && std::fabs(mid[2] - (floorG + span / 9.0)) < 0.03,
         "curve mid-release gains: log %.3f lin %.3f exp %.3f", mid[0], mid[1], mid[2]);
  std::printf("  curve at mid-release: log %.3f, linear %.3f, exp %.3f\n", mid[0], mid[1], mid[2]);
  // Depth.
  for (int depth : {0, 32, 127}) {
    Setup s;
    s.depth = depth;
    const Env e = measure(s, 0.3);
    CHECKF(std::fabs(double(e.minIn(0.0, 0.3)) - (1.0 - depth / 127.0)) < 0.004, "depth %d: floor %.4f", depth, double(e.minIn(0.0, 0.3)));
  }
  // Per-track amount scales the dip: gain = 1 + amount * (env - 1).
  for (int amount : {0, 64, 110}) {
    Setup s;
    s.amount = amount;
    const Env e = measure(s, 0.3);
    const double expect = 1.0 - (amount / 127.0) * (100.0 / 127.0);
    CHECKF(std::fabs(double(e.minIn(0.0, 0.3)) - expect) < 0.004, "amount %d: floor %.4f, expected %.4f", amount, double(e.minIn(0.0, 0.3)), expect);
  }
}

static void testSources() {
  Setup s;
  // Off: never ducks.
  s.source = 0;
  CHECK(double(measure(s, 0.6).minIn(0.0, 0.6)) > 0.999);
  // Kick: follows kick steps that actually fire.
  s = Setup(); s.kickEvery = 8;
  {
    const Env e = measure(s, 1.2);
    const double beat = 60.0 / 124.0;
    CHECK(double(e.minIn(0.0, 0.05)) < 0.25);
    CHECKF(double(e.minIn(beat - 0.01, beat + 0.2)) > 0.999, "ducked on a beat without a kick step");
    CHECK(double(e.minIn(2 * beat, 2 * beat + 0.05)) < 0.25);
  }
  // DECISION: Kick source follows the AUDIBLE kick. Muted kick, or a step that
  // loses its probability roll, does not duck ("Beat" is the ghost-kick mode).
  s = Setup(); s.kickMuted = true;
  CHECKF(double(measure(s, 0.6).minIn(0.0, 0.6)) > 0.999, "muted kick still ducked in KICK mode");
  s = Setup(); s.kickProb = 0;
  CHECK(double(measure(s, 0.6).minIn(0.0, 0.6)) > 0.999);
  // Beat: every beat, with no kick steps at all, or with the kick muted.
  s = Setup(); s.source = 2; s.kickEvery = 0;
  {
    const Env e = measure(s, 1.2);
    const double beat = 60.0 / 124.0;
    for (int b = 0; b < 2; ++b) CHECKF(double(e.minIn(b * beat, b * beat + 0.05)) < 0.22, "BEAT mode: beat %d not ducked", b);
    CHECK(double(e.minIn(beat - 0.05, beat - 0.001)) > 0.999);
  }
  s = Setup(); s.source = 2; s.kickMuted = true;
  CHECK(double(measure(s, 0.3).minIn(0.0, 0.3)) < 0.22);
  // Ratchets retrigger; a retrigger during the release restarts the dip without a jump.
  s = Setup(); s.kickEvery = 1; s.release = 100;
  {
    const Env e = measure(s, 0.6);
    const double step = 15.0 / 124.0;
    CHECK(double(e.maxIn(0.004, 0.6)) < 0.75);          // never recovers between 16th-note kicks
    CHECK(double(e.minIn(step, step + 0.05)) < 0.22);
    CHECKF(e.maxStep() < 0.02f, "retrigger step %.4f", double(e.maxStep()));
  }
  s = Setup(); s.kickRatchet = 4; s.hold = 0; s.release = 30;
  {
    const Env e = measure(s, 0.3);
    const double hit = 15.0 / 124.0 / 4.0;
    for (int h = 0; h < 4; ++h) CHECKF(double(e.minIn(h * hit, h * hit + 0.01)) < 0.22, "ratchet hit %d did not duck", h);
  }
  // Transport stopped: BEAT does nothing; a live kick still ducks in KICK mode.
  rig.init();
  rig.neutralMix();
  rig.v[kProbe].mode = StubVoice::Dc;
  rig.proj.tracks[kProbe].params[kMixDuck] = 127;
  rig.proj.globals[kGDuckSource] = 2;
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.runSeconds(1.0);
  CHECK(peakOf(rig.out, 8000, rig.frames(), 0) > 0.499 && rmsOf(rig.out, 8000, rig.frames(), 0) > 0.499);
  rig.post(CmdType::SetGlobal, kGDuckSource, 0, 1);
  rig.post(CmdType::NoteOn, kTrackKick, 60, 100);
  const long f0 = rig.frames();
  rig.runSeconds(0.5);
  double mn = 9;
  for (long f = f0; f < rig.frames(); ++f) mn = std::fmin(mn, double(rig.sample(f, 0)) / 0.5);
  CHECKF(mn < 0.22, "live kick did not duck (min gain %.3f)", mn);
  CHECK(std::fabs(double(rig.sample(rig.frames() - 1, 0)) / 0.5 - 1.0) < 0.003);
  rig.capture = false;
}

// FX returns are ducked by kGDuckFx. The dry track is panned hard left and the
// delay is set to mono, so the RIGHT channel carries only the delay return.
static void testFxDuck() {
  double depthAt[3];
  int k = 0;
  for (int amt : {0, 64, 127}) {
    rig.init();
    rig.neutralMix();
    rig.v[kTrackLead].mode = StubVoice::Tone;
    rig.v[kTrackLead].freq = 1000.0f;
    rig.v[kTrackLead].amp = 0.3f;
    rig.proj.tracks[kTrackLead].params[kMixPan] = 0;
    rig.proj.tracks[kTrackLead].params[kMixDelaySend] = 127;
    rig.proj.globals[kGDelayWidth] = 0;
    rig.proj.globals[kGDelayFeedback] = 0;
    rig.proj.globals[kGDelayTone] = 96;
    rig.proj.globals[kGDuckSource] = 2;
    rig.proj.globals[kGDuckFx] = uint8_t(amt);
    rig.eng.init(44100.0f, &rig.proj);
    rig.runSeconds(1.0);
    rig.capture = true;
    rig.post(CmdType::Play);
    rig.runSeconds(0.45);
    rig.capture = false;
    const double ducked = rmsOf(rig.out, 300, 1300, 1);          // inside the hold
    const double open = rmsOf(rig.out, 15000, 19000, 1);         // fully recovered
    depthAt[k++] = ducked / open;
    CHECK(open > 0.05);
  }
  const double fl = 1.0 - 100.0 / 127.0;
  CHECKF(std::fabs(depthAt[0] - 1.0) < 0.02, "DuckFx 0: return gain %.3f", depthAt[0]);
  CHECKF(std::fabs(depthAt[1] - (1.0 - 64.0 / 127.0 * (1.0 - fl))) < 0.03, "DuckFx 64: return gain %.3f", depthAt[1]);
  CHECKF(std::fabs(depthAt[2] - fl) < 0.03, "DuckFx 127: return gain %.3f", depthAt[2]);
  std::printf("  FX return gain during hold: DuckFx 0 -> %.3f, 64 -> %.3f, 127 -> %.3f\n", depthAt[0], depthAt[1], depthAt[2]);
}

// Defaults at 124 BPM: "an obvious but tasteful pump".
static void testDefaultPump() {
  rig.init();
  for (int t = 0; t < kNumTracks; ++t) { rig.v[t].mode = StubVoice::Dc; rig.v[t].amp = 0.25f; }
  rig.v[kTrackKick].mode = StubVoice::Silent;
  for (int i = 0; i < 16; i += 4) rig.step(0, kTrackKick, i) = mkStep(60);
  rig.proj.globals[kGMasterVolume] = 100;
  rig.eng.init(44100.0f, &rig.proj);
  const double beat = 60.0 / 124.0;
  double dip[kNumTracks], offbeat[kNumTracks];
  for (int t = 1; t < kNumTracks; ++t) {
    // solo each track's DC through its default duck amount
    rig.init();
    rig.v[t].mode = StubVoice::Dc;
    rig.v[t].amp = 0.25f;
    for (int i = 0; i < 16; i += 4) rig.step(0, kTrackKick, i) = mkStep(60);
    for (int u = 0; u < kNumTracks; ++u) { rig.proj.tracks[u].params[kMixDelaySend] = 0; rig.proj.tracks[u].params[kMixReverbSend] = 0; rig.proj.tracks[u].params[kMixPan] = 64; }
    rig.eng.init(44100.0f, &rig.proj);
    rig.runSeconds(0.3);
    rig.capture = true;
    rig.post(CmdType::Play);
    rig.runSeconds(1.0);
    rig.capture = false;
    const double ref = double(rig.sample(long(0.95 * beat * 44100.0), 0));
    double mn = 9;
    for (long f = 0; f < long(beat * 44100.0); ++f) mn = std::fmin(mn, double(rig.sample(f, 0)));
    dip[t] = mn / ref;
    offbeat[t] = double(rig.sample(long(0.5 * beat * 44100.0), 0)) / ref;
  }
  std::printf("  default pump at 124 BPM (dip at the kick / gain on the off-beat):\n");
  const char* names[kNumTracks] = {"kick", "snare", "clap", "c-hat", "o-hat", "perc", "bass", "lead"};
  for (int t = 1; t < kNumTracks; ++t) std::printf("    %-5s %6.1f dB / %5.1f dB\n", names[t], dB(dip[t]), dB(offbeat[t]));
  CHECKF(dB(dip[kTrackBass]) < -8.0 && dB(dip[kTrackBass]) > -14.0, "bass dip %.1f dB (want an obvious -8..-14)", dB(dip[kTrackBass]));
  CHECKF(dB(dip[kTrackLead]) < -3.0 && dB(dip[kTrackLead]) > -7.0, "lead dip %.1f dB (want a moderate -3..-7)", dB(dip[kTrackLead]));
  CHECK(std::fabs(dB(dip[kTrackSnare])) < 0.05 && std::fabs(dB(dip[kTrackHatClosed])) < 0.05);
  // Back to within 1 dB by the off-beat 8th, so the off-beat bass note is not swallowed.
  CHECKF(dB(offbeat[kTrackBass]) > -1.0, "bass still %.1f dB down on the off-beat", dB(offbeat[kTrackBass]));
}

static void renderPump() {
  // 4 bars at 124: a sustained 110 Hz "bass" tone pumped by a four-on-the-floor
  // kick blip, then 2 bars with the kick muted (pump stops in KICK mode), then
  // 2 bars in BEAT mode with the kick still muted (ghost pump).
  rig.init();
  rig.v[kTrackBass].mode = StubVoice::Tone;
  rig.v[kTrackBass].freq = 110.0f;
  rig.v[kTrackBass].amp = 0.4f;
  rig.v[kTrackKick].mode = StubVoice::Blip;
  rig.v[kTrackKick].amp = 0.45f;
  rig.v[kTrackKick].decaySec = 0.08f;
  for (int i = 0; i < 16; i += 4) rig.step(0, kTrackKick, i) = mkStep(33);
  rig.eng.init(44100.0f, &rig.proj);
  rig.runSeconds(0.2);
  rig.capture = true;
  rig.post(CmdType::Play);
  const double bar = 4.0 * 60.0 / 124.0;
  rig.runSeconds(4 * bar);
  rig.post(CmdType::SetMute, kTrackKick, 0, 1);
  rig.runSeconds(2 * bar);
  rig.post(CmdType::SetGlobal, kGDuckSource, 0, 2);
  rig.runSeconds(2 * bar);
  rig.capture = false;
  CHECK(rig.writeWav("ducker_pump.wav"));
  // Sanity on the render itself: bass level under the kick vs between kicks.
  // (amplitude of the 110 Hz component over two of its periods, so the 55 Hz kick blip mostly drops out)
  auto toneAmp = [&](long f0) {
    double re = 0, im = 0;
    const int n = 802;
    for (int i = 0; i < n; ++i) {
      const double x = double(rig.sample(f0 + i, 0)), ph = 6.283185307179586 * 110.0 * double(i) / 44100.0;
      re += x * std::cos(ph); im += x * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / double(n);
  };
  const long beat = long(60.0 / 124.0 * 44100.0);
  const double under = toneAmp(4 * beat + 420);
  const double between = toneAmp(4 * beat + 17000);
  std::printf("  render build/renders/engine/ducker_pump.wav: tone %.1f dB under the kick, %.1f dB between kicks\n", dB(under), dB(between));
  CHECKF(between > under * 2.2, "pump too shallow in the render: %.3f vs %.3f", under, between);
  const double mutedA = rmsOf(rig.out, long(4.5 * bar * 44100.0), long(4.5 * bar * 44100.0) + 4000, 0);
  const double mutedB = rmsOf(rig.out, long(5.0 * bar * 44100.0) + 500, long(5.0 * bar * 44100.0) + 4500, 0);
  CHECKF(std::fabs(dB(mutedA) - dB(mutedB)) < 0.5, "kick muted in KICK mode should not pump (%.1f vs %.1f dB)", dB(mutedA), dB(mutedB));
}

int main() {
  testShape(44100.0f);
  testShape(32000.0f);
  testParams();
  testSources();
  testFxDuck();
  testDefaultPump();
  renderPump();
  CHECK(engine_detail::faultCount() == 0);
  return finish("test_engine_ducker");
}
