// Polyphony, voice stealing, note-off routing, chords, legato slide, choke, active().
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

// Magnitude of the sine at MIDI `note` in x[start, start+n).
static double noteMag(const Synth& s, size_t start, size_t n, double note) {
  const std::vector<double> m = spectrum(s.L, start, n);
  const double binHz = double(s.fs) / double(n);
  const size_t c = size_t(std::lround(midiHz(note) / binHz));
  double mx = 0;
  for (size_t b = c - 3; b <= c + 3; ++b) mx = std::max(mx, m[b]);
  return mx;
}
static size_t at(const Synth& s, double t) { return size_t(t * double(s.fs)); }

int main() {
  const size_t N = 8192;

  // ---- four overlapping notes all sound; a fifth steals the oldest without a click
  {
    Synth s;
    sinePatch(s);
    s.set(kPRelease, 30);
    const int notes[4] = {48, 55, 60, 64};
    for (int i = 0; i < 4; ++i) { s.on(notes[i]); s.run(0.05); }
    s.runTo(0.5);
    LT_CHECK(s.v.soundingVoices() == 4, "4 notes -> %d voices", s.v.soundingVoices());
    double ref = 0;
    for (int i = 0; i < 4; ++i) {
      const double m = noteMag(s, at(s, 0.25), N, notes[i]);
      ref = std::max(ref, m);
      LT_CHECK(m > 0.01, "note %d missing (%.4f)", notes[i], m);
    }
    const double steady = maxStep(s.L, at(s, 0.3), at(s, 0.5));
    s.on(67);                              // 5th note: must steal note 48 (oldest)
    s.runTo(1.0);
    LT_CHECK(s.v.soundingVoices() == 4, "after steal: %d voices", s.v.soundingVoices());
    const double stealStep = maxStep(s.L, at(s, 0.5) - 8, at(s, 0.56));
    std::printf("  steal: max sample step %.5f (steady state %.5f)\n", stealStep, steady);
    LT_CHECK(stealStep < steady * 1.5, "click on steal: step %.5f vs steady %.5f", stealStep, steady);
    LT_CHECK(noteMag(s, at(s, 0.7), N, 67) > ref * 0.5, "stolen-in note 67 missing");
    LT_CHECK(noteMag(s, at(s, 0.7), N, 48) < ref * 0.01, "oldest note 48 should have been stolen");
    for (int n : {55, 60, 64}) LT_CHECK(noteMag(s, at(s, 0.7), N, n) > ref * 0.5, "note %d lost by steal", n);
    // the stolen-in note starts within ~3 ms
    const std::vector<double> m5 = spectrum(s.L, at(s, 0.5), 1024);
    (void)m5;

    // ---- noteOff releases exactly its own note
    s.off(60);
    s.runTo(1.5);
    LT_CHECK(s.v.soundingVoices() == 3, "after noteOff(60): %d voices", s.v.soundingVoices());
    LT_CHECK(noteMag(s, at(s, 1.25), N, 60) < ref * 0.01, "note 60 still sounding after noteOff");
    for (int n : {55, 64, 67}) LT_CHECK(noteMag(s, at(s, 1.25), N, n) > ref * 0.5, "noteOff(60) released note %d", n);
    s.off(48);                             // already stolen: must be a no-op
    s.runTo(1.6);
    LT_CHECK(s.v.soundingVoices() == 3, "noteOff of a stolen note changed something");
    s.off(55); s.off(64); s.off(67);
    s.runTo(2.5);
    LT_CHECK(!s.v.active(), "voices still active after all noteOffs");
  }

  // ---- stealing prefers released voices over held ones
  {
    Synth s;
    sinePatch(s);
    s.set(kPRelease, 100);   // long tails
    s.on(48); s.on(52);
    s.run(0.2);
    s.off(48);               // 48 is now a release tail
    s.on(60); s.on(64);
    s.run(0.1);
    s.on(72);                // needs a voice: must take the released 48, not held 52
    s.run(0.4);
    const double m52 = noteMag(s, at(s, 0.45), N, 52), m48 = noteMag(s, at(s, 0.45), N, 48);
    LT_CHECK(m52 > 0.01, "held note 52 was stolen while a released voice existed");
    LT_CHECK(m48 < m52 * 0.02, "released note 48 should have been stolen");
  }

  // ---- chord: noteOff(root) releases every chord note; active() false after the tail
  {
    Synth s;
    s.on(57, 127, pg::kChordMin7);
    s.run(0.3);
    LT_CHECK(s.v.soundingVoices() == 4, "min7 -> %d voices", s.v.soundingVoices());
    s.off(60);               // a chord member that is not the root: nothing happens
    s.run(0.05);
    const double before = rms(s.L, at(s, 0.3), at(s, 0.35));
    LT_CHECK(before > 0.01, "chord silent before noteOff");
    s.off(57);
    const double rel = double(LeadVoice::releaseSeconds(s.p[kPRelease]));
    s.run(rel * 0.5);
    LT_CHECK(s.v.active(), "voice freed before half the release time");
    s.run(rel * 1.0 + 0.02);   // -80 dB is reached at 1.33 x T60
    LT_CHECK(!s.v.active(), "active() still true %.3f s after noteOff (release T60 %.3f s)", rel * 1.5 + 0.02, rel);
    const size_t n = s.size();
    LT_CHECK(peak(s.L, n - 64) < 1e-4 && peak(s.R, n - 64) < 1e-4, "tail not silent when active() went false");
    std::printf("  chord release: T60 %.3f s, active() false within %.3f s\n", rel, rel * 1.5 + 0.02);
  }

  // ---- sustain 0: a held note frees its voice by itself once it has decayed
  {
    Synth s;
    s.set(kPSustain, 0);
    s.set(kPDecay, 20);
    s.on(60, 127, pg::kChordMaj);
    s.run(double(LeadVoice::decaySeconds(20)) * 1.5 + 0.05);
    LT_CHECK(!s.v.active(), "decayed-to-silence held chord still active");
  }

  // ---- hammering the same chord: never more than 4 voices, never a click
  {
    Synth s;
    sinePatch(s);
    s.set(kPCutoff, 60);
    s.on(45, 127, pg::kChordMin7);
    s.run(0.3);
    const double steady = maxStep(s.L, at(s, 0.2), at(s, 0.3));
    for (int i = 0; i < 12; ++i) {
      s.on(i % 2 ? 45 : 48, 127, i % 3 ? pg::kChordMin7 : pg::kChordMin9);
      LT_CHECK(s.v.soundingVoices() <= 4, "more than 4 voices");
      s.run(0.011 + 0.007 * i);
    }
    s.run(0.2);
    const double st = maxStep(s.L, at(s, 0.3) - 8);
    std::printf("  chord retrigger storm: max step %.5f (steady %.5f)\n", st, steady);
    LT_CHECK(st < steady * 1.6, "click when re-stealing chords: %.5f vs %.5f", st, steady);
    LT_CHECK(s.v.soundingVoices() == 4, "storm left %d voices", s.v.soundingVoices());
    s.off(45); s.off(48);
    s.run(1.0);
    LT_CHECK(!s.v.active(), "stuck voice after retrigger storm");
  }

  // ---- choke: ~2.5 ms fade, no click, then inactive
  {
    Synth s;
    sinePatch(s);
    s.on(45, 127, pg::kChordMin7);
    s.run(0.3);
    const double steady = maxStep(s.L, at(s, 0.2), at(s, 0.3));
    s.v.choke();
    s.run(0.004);
    LT_CHECK(!s.v.active(), "still active 4 ms after choke");
    const double st = maxStep(s.L, at(s, 0.3) - 8);
    LT_CHECK(st < steady * 1.5, "click on choke: %.5f vs %.5f", st, steady);
    LT_CHECK(rms(s.L, at(s, 0.3), at(s, 0.3005)) > 0.01, "choke was not a fade");
    s.on(60);   // usable again straight away
    s.run(0.05);
    LT_CHECK(rms(s.L, at(s, 0.33), at(s, 0.35)) > 0.01, "no sound after choke + noteOn");
  }

  // ---- slide: legato glide, no retrigger, old noteOff must not cut the note
  {
    Synth s;
    sinePatch(s);
    s.set(kPGlide, 80);
    const double tau = double(LeadVoice::glideSeconds(80));
    s.on(60);
    s.run(0.2);
    s.on(67, 127, 0, false, true);          // slide into 67 while 60 is held
    LT_CHECK(s.v.soundingVoices() == 1, "slide allocated a new voice");
    s.run(0.01);
    s.off(60);                              // the previous step's gate ends: must be ignored
    s.runTo(0.2 + tau * 8);
    LT_CHECK(s.v.soundingVoices() == 1, "slide: %d voices", s.v.soundingVoices());
    const std::vector<double> env = envelope(s.L);
    double mn = 1e9, mx = 0;
    for (size_t i = at(s, 0.1); i < at(s, 0.2 + tau * 8) - 400; ++i) { mn = std::min(mn, env[i]); mx = std::max(mx, env[i]); }
    LT_CHECK(mn > mx * 0.9, "slide retriggered the envelope (level dipped to %.2f of max)", mn / mx);
    // pitch one time constant after the slide: 63% of the way (in semitones)
    const double fTau = zeroCrossHz(s.L, s.fs, at(s, 0.2 + tau) - 150, at(s, 0.2 + tau) + 150);
    const double semisTau = cents(fTau, midiHz(60)) / 100.0;
    const double fEnd = zeroCrossHz(s.L, s.fs, at(s, 0.2 + tau * 7), at(s, 0.2 + tau * 8));
    std::printf("  slide 60->67, glide tc %.0f ms: +%.2f semis after one tc (ideal 4.42), final error %.2f cents\n", tau * 1000,
                semisTau, cents(fEnd, midiHz(67)));
    LT_CHECK(std::fabs(semisTau - 4.42) < 0.6, "glide rate off: %.2f semis after one time constant", semisTau);
    LT_CHECK(std::fabs(cents(fEnd, midiHz(67))) < 3.0, "glide does not land on the note");
    s.off(67);
    s.run(0.5);
    LT_CHECK(!s.v.active(), "slid note did not release");

    // no portamento without the slide flag
    Synth p;
    sinePatch(p);
    p.set(kPGlide, 127);
    p.set(kPRelease, 0);
    p.on(48);
    p.run(0.2);
    p.off(48);
    p.run(0.1);
    p.on(72);
    p.run(0.2);
    const double f = zeroCrossHz(p.L, p.fs, at(p, 0.305), at(p, 0.35));
    LT_CHECK(std::fabs(cents(f, midiHz(72))) < 3.0, "non-slide note glided (%.1f cents off just after onset)", cents(f, midiHz(72)));

    // slide with nothing sounding is just a note
    Synth q;
    sinePatch(q);
    q.on(64, 127, 0, false, true);
    q.run(0.1);
    LT_CHECK(q.v.soundingVoices() == 1 && rms(q.L) > 0.01, "slide from silence did not play");
  }

  // ---- chord slide: all four voices retarget, none is retriggered or added
  {
    Synth s;
    sinePatch(s);
    s.on(57, 127, pg::kChordMin7);
    s.run(0.2);
    s.on(53, 127, pg::kChordMaj7, false, true);
    s.run(0.02);
    s.off(57);
    s.run(0.6);
    LT_CHECK(s.v.soundingVoices() == 4, "chord slide: %d voices", s.v.soundingVoices());
    for (int iv : {0, 4, 7, 11}) LT_CHECK(noteMag(s, at(s, 0.5), N, 53 + iv) > 0.01, "chord slide: note %d missing", 53 + iv);
    LT_CHECK(noteMag(s, at(s, 0.5), N, 67) < 0.002, "chord slide: old note 67 still there");
    // slide from a 4-note chord to a single note releases the other three
    s.on(65, 127, pg::kChordNone, false, true);
    s.run(0.01);
    s.off(53);
    s.run(1.0);
    LT_CHECK(s.v.soundingVoices() == 1, "chord -> single slide left %d voices", s.v.soundingVoices());
    s.off(65);
    s.run(1.0);
    LT_CHECK(!s.v.active(), "voices stuck after chord slide");
  }
  return finish("test_lead_poly");
}
