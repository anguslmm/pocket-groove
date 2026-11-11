// Renders audition WAVs for the drum voices into build/renders/drums/ so a
// human can listen: every voice at its defaults and at contrasting presets,
// plus short house / techno loops sequenced here. Also fails if any render
// is silent, clips or is non-finite.
#include <filesystem>
#include <string>
#include "common/wav.h"
#include "tests/drums_testutil.h"

using namespace pg::drums;
using dt::Params;

namespace {
const float kFs = 44100.0f;
const char* kDir = "build/renders/drums";
dt::Checker T;

struct Set { int param; int value; };

Params preset(const pg::Voice& v, std::initializer_list<Set> sets) {
  Params p = dt::defaults(v);
  for (const Set& s : sets) p[s.param] = uint8_t(s.value);
  return p;
}

void save(const std::string& name, const std::vector<float>& mono) {
  std::vector<float> st(mono.size() * 2);
  for (size_t i = 0; i < mono.size(); ++i) st[2 * i] = st[2 * i + 1] = mono[i];
  const std::string path = std::string(kDir) + "/" + name + ".wav";
  const bool ok = wav::writeFloat(path.c_str(), st, int(kFs), 2);
  const float pk = dt::peak(mono);
  std::printf("  %-28s peak %6.3f (%6.1f dBFS)  rms %6.1f dB\n", name.c_str(), double(pk), double(dt::dB(pk)),
              double(dt::dB(dt::rms(mono))));
  T.check(ok, "wav written");
  T.check(dt::allFinite(mono), "finite");
  T.check(pk > 0.02f && pk < 0.99f, "audition peak sane");
}

// Four hits: full velocity, default step velocity, accented, soft.
template <class V>
void audition(const std::string& name, V& voice, const Params& p, float gap = 0.7f, uint8_t note = 60) {
  voice.init(kFs);
  std::vector<dt::Event> ev = {dt::hit(0.0f, kFs, 127, note), dt::hit(gap, kFs, 100, note),
                               dt::hit(2 * gap, kFs, 100, note, true), dt::hit(3 * gap, kFs, 56, note)};
  save(name, dt::run(voice, p, ev, 4 * gap + 0.3f, kFs));
}

// ---- a few-line step sequencer -------------------------------------------
// Pattern chars per 16th: '.' rest, 'x' vel 100, 'X' accent, 'o' ghost (vel 50), 'f' vel 127.
struct Track {
  pg::Voice* voice;
  Params params;
  const char* pattern;  // repeated to fill the loop
  const char* notes;    // optional: per-step semitone offsets as letters ('a' = 0, 'b' = +1...), or nullptr
  int chokes;           // index of a track this one chokes when it fires, or -1
};

std::vector<float> sequence(std::vector<Track>& tracks, float bpm, int bars, float swing) {
  const float stepSec = 60.0f / bpm / 4.0f;
  const int steps = bars * 16;
  const int totalQ = int((float(steps) * stepSec + 1.0f) * kFs / float(pg::kQuantum));
  std::vector<float> mix(size_t(totalQ) * pg::kQuantum, 0.0f);
  float l[pg::kQuantum], r[pg::kQuantum];
  for (Track& t : tracks) t.voice->init(kFs);
  for (int q = 0; q < totalQ; ++q) {
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
      Track& t = tracks[ti];
      t.voice->setParams(t.params.v);
      const int len = int(std::char_traits<char>::length(t.pattern));
      for (int s = 0; s < steps; ++s) {
        const float when = (float(s) + ((s & 1) ? swing : 0.0f)) * stepSec;
        if (int(when * kFs / float(pg::kQuantum) + 0.5f) != q) continue;
        const char c = t.pattern[s % len];
        if (c == '.') continue;
        uint8_t note = 60;
        if (t.notes) note = uint8_t(60 + (t.notes[s % int(std::char_traits<char>::length(t.notes))] - 'm'));
        const uint8_t vel = c == 'o' ? 50 : (c == 'f' ? 127 : 100);
        if (t.chokes >= 0) tracks[size_t(t.chokes)].voice->choke();
        t.voice->noteOn(pg::NoteEvent{note, vel, c == 'X', false, 0});
      }
    }
    for (Track& t : tracks) {
      if (!t.voice->active()) continue;
      t.voice->render(l, r, pg::kQuantum);
      for (int i = 0; i < pg::kQuantum; ++i) mix[size_t(q) * pg::kQuantum + size_t(i)] += l[i];
    }
  }
  return mix;
}
}  // namespace

int main() {
  std::error_code ec;
  std::filesystem::create_directories(kDir, ec);
  std::printf("rendering auditions to %s/\n", kDir);

  // ---- Kick ----------------------------------------------------------------
  KickVoice kick;
  using K = KickVoice;
  audition("kick_default_909", kick, dt::defaults(kick), 0.8f);
  const Params kick808 = preset(kick, {{K::kTune, 50}, {K::kDecay, 92}, {K::kPunch, 40}, {K::kSweep, 40},
                                       {K::kClick, 30}, {K::kDrive, 22}, {K::kTone, 70}, {K::kHold, 0},
                                       {K::kHarm, 20}, {K::kClickTone, 20}});
  audition("kick_808_sub_boom", kick, kick808, 1.4f);
  const Params kickTechno = preset(kick, {{K::kTune, 52}, {K::kDecay, 52}, {K::kPunch, 92}, {K::kSweep, 66},
                                          {K::kClick, 84}, {K::kDrive, 96}, {K::kTone, 84}, {K::kHold, 70},
                                          {K::kHarm, 60}, {K::kClickTone, 80}});
  audition("kick_techno_hard", kick, kickTechno, 0.8f);
  const Params kickTight = preset(kick, {{K::kTune, 70}, {K::kDecay, 30}, {K::kPunch, 84}, {K::kSweep, 46},
                                         {K::kClick, 90}, {K::kDrive, 50}, {K::kTone, 110}, {K::kHold, 20}});
  audition("kick_tight_clicky", kick, kickTight, 0.6f);
  const Params kickSoft = preset(kick, {{K::kTune, 56}, {K::kDecay, 60}, {K::kPunch, 56}, {K::kSweep, 80},
                                        {K::kClick, 10}, {K::kDrive, 10}, {K::kTone, 40}, {K::kHold, 40}});
  audition("kick_deep_soft", kick, kickSoft, 0.8f);
  {  // 808 kick played as a bassline: note transposition
    kick.init(kFs);
    std::vector<dt::Event> ev;
    const int notes[8] = {60, 60, 63, 60, 67, 65, 58, 55};
    for (int i = 0; i < 8; ++i) ev.push_back(dt::hit(0.4f * float(i), kFs, 110, uint8_t(notes[i])));
    save("kick_808_bassline_notes", dt::run(kick, kick808, ev, 4.5f, kFs));
  }

  // ---- Snare ---------------------------------------------------------------
  SnareVoice snare;
  using S = SnareVoice;
  audition("snare_default_909", snare, dt::defaults(snare));
  audition("snare_tight_rimmy_techno", snare,
           preset(snare, {{S::kTune, 96}, {S::kDecay, 22}, {S::kSnap, 52}, {S::kTone, 96}, {S::kBody, 28},
                          {S::kBend, 30}, {S::kRing, 100}, {S::kDetune, 76}, {S::kDrive, 70}}));
  audition("snare_fat_long", snare,
           preset(snare, {{S::kTune, 36}, {S::kDecay, 86}, {S::kSnap, 96}, {S::kTone, 50}, {S::kBody, 76},
                          {S::kBend, 80}, {S::kRing, 40}, {S::kDrive, 54}}));
  audition("snare_808_tonal", snare,
           preset(snare, {{S::kTune, 80}, {S::kDecay, 50}, {S::kSnap, 56}, {S::kTone, 84}, {S::kBody, 70},
                          {S::kBend, 10}, {S::kRing, 90}, {S::kDetune, 73}, {S::kDrive, 14}}));
  audition("snare_noise_crack", snare,
           preset(snare, {{S::kTune, 70}, {S::kDecay, 40}, {S::kSnap, 127}, {S::kTone, 110}, {S::kBody, 20},
                          {S::kDrive, 90}}));

  // ---- Clap ----------------------------------------------------------------
  ClapVoice clap;
  using C = ClapVoice;
  audition("clap_default_909", clap, dt::defaults(clap));
  audition("clap_tight_dry", clap,
           preset(clap, {{C::kTone, 76}, {C::kDecay, 20}, {C::kSpread, 40}, {C::kTail, 30}, {C::kBursts, 2},
                         {C::kDrive, 70}}));
  audition("clap_big_room", clap,
           preset(clap, {{C::kTone, 44}, {C::kDecay, 100}, {C::kSpread, 86}, {C::kTail, 110}, {C::kBursts, 4},
                         {C::kReso, 36}, {C::kAir, 70}}));
  audition("clap_808_narrow", clap,
           preset(clap, {{C::kTone, 52}, {C::kDecay, 70}, {C::kSpread, 58}, {C::kTail, 84}, {C::kBursts, 3},
                         {C::kReso, 96}, {C::kAir, 10}, {C::kDrive, 20}}));
  audition("clap_snap_single", clap,
           preset(clap, {{C::kTone, 90}, {C::kDecay, 34}, {C::kBursts, 0}, {C::kTail, 100}, {C::kAir, 90}}));

  // ---- Hats ----------------------------------------------------------------
  HatVoice ch(false), oh(true);
  using H = HatVoice;
  audition("hat_closed_default", ch, dt::defaults(ch), 0.4f);
  audition("hat_closed_808_metal", ch,
           preset(ch, {{H::kTune, 40}, {H::kDecay, 50}, {H::kTone, 60}, {H::kMetal, 127}, {H::kColour, 90},
                       {H::kSnap, 30}}), 0.4f);
  audition("hat_closed_noisy_soft", ch,
           preset(ch, {{H::kDecay, 76}, {H::kTone, 96}, {H::kMetal, 20}, {H::kColour, 20}, {H::kSnap, 20}}), 0.4f);
  audition("hat_closed_tick", ch,
           preset(ch, {{H::kTune, 100}, {H::kDecay, 20}, {H::kTone, 110}, {H::kSnap, 110}}), 0.4f);
  audition("hat_open_default", oh, dt::defaults(oh), 0.9f);
  audition("hat_open_long_808", oh,
           preset(oh, {{H::kTune, 40}, {H::kDecay, 96}, {H::kTone, 56}, {H::kMetal, 127}, {H::kColour, 84}}), 1.6f);
  audition("hat_open_short_noisy", oh,
           preset(oh, {{H::kDecay, 20}, {H::kTone, 90}, {H::kMetal, 40}, {H::kSpread, 100}}), 0.6f);
  audition("hat_open_trashy_low", oh,
           preset(oh, {{H::kTune, 10}, {H::kDecay, 64}, {H::kTone, 10}, {H::kMetal, 110}, {H::kColour, 110},
                       {H::kSpread, 20}}), 1.0f);
  {  // open hat choked by the closed hat, as the engine does it
    ch.init(kFs); oh.init(kFs);
    std::vector<Track> tr = {{&oh, dt::defaults(oh), "..x...x...x...x.", nullptr, -1},
                             {&ch, dt::defaults(ch), "x...x...x...x...", nullptr, 0}};
    save("hat_open_choked_by_closed", sequence(tr, 124.0f, 2, 0.0f));
  }

  // ---- Perc ----------------------------------------------------------------
  PercVoice perc;
  using P = PercVoice;
  const char* modelFile[P::kNumModels] = {"perc_tom_default", "perc_rim_default", "perc_cowbell_default",
                                          "perc_clave_default", "perc_shaker_default", "perc_zap_default"};
  for (int m = 0; m < P::kNumModels; ++m) audition(modelFile[m], perc, preset(perc, {{P::kModel, m}}), 0.6f);
  audition("perc_tom_low_long", perc,
           preset(perc, {{P::kModel, P::kTom}, {P::kTune, 30}, {P::kDecay, 96}, {P::kBend, 90}, {P::kNoise, 30}}), 0.9f);
  audition("perc_conga_high", perc,
           preset(perc, {{P::kModel, P::kTom}, {P::kTune, 104}, {P::kDecay, 40}, {P::kBend, 30}, {P::kTone, 30},
                         {P::kNoise, 80}}), 0.5f);
  audition("perc_woodblock", perc,
           preset(perc, {{P::kModel, P::kClave}, {P::kTune, 20}, {P::kDecay, 50}, {P::kTone, 100}, {P::kHarm, 40}}), 0.5f);
  audition("perc_shaker_soft_grainy", perc,
           preset(perc, {{P::kModel, P::kShaker}, {P::kBend, 100}, {P::kDecay, 80}, {P::kHarm, 110}, {P::kTune, 50}}), 0.5f);
  audition("perc_zap_laser", perc,
           preset(perc, {{P::kModel, P::kZap}, {P::kTune, 40}, {P::kBend, 127}, {P::kDecay, 90}, {P::kDrive, 70}}), 0.7f);
  audition("perc_blip_tuned", perc,
           preset(perc, {{P::kModel, P::kZap}, {P::kTune, 90}, {P::kBend, 0}, {P::kDecay, 30}, {P::kTone, 20},
                         {P::kNoise, 0}}), 0.4f);
  audition("perc_cowbell_low_dirty", perc,
           preset(perc, {{P::kModel, P::kCowbell}, {P::kTune, 30}, {P::kDecay, 90}, {P::kTone, 40}, {P::kNoise, 110},
                         {P::kDrive, 90}}), 0.7f);
  {  // tuned perc melody via note transposition (blip)
    perc.init(kFs);
    std::vector<dt::Event> ev;
    const int notes[8] = {60, 63, 67, 72, 70, 67, 63, 58};
    for (int i = 0; i < 8; ++i) ev.push_back(dt::hit(0.25f * float(i), kFs, 110, uint8_t(notes[i])));
    save("perc_blip_melody_notes", dt::run(perc, preset(perc, {{P::kModel, P::kZap}, {P::kTune, 80}, {P::kBend, 20},
                                                              {P::kDecay, 50}}), ev, 2.5f, kFs));
  }

  // ---- Loops: all mixer levels equal (unity), straight sum -------------------
  {
    KickVoice k; SnareVoice s; ClapVoice c; HatVoice h(false), o(true); PercVoice p;
    std::vector<Track> tr = {
        {&k, dt::defaults(k), "x...x...x...x...x...x...x...x..x", nullptr, -1},
        {&s, dt::defaults(s), "............o..o............o.xo", nullptr, -1},
        {&c, dt::defaults(c), "....x.......x.......x.......x...", nullptr, -1},
        {&o, dt::defaults(o), "..x...x...x...x.", nullptr, -1},
        {&h, dt::defaults(h), "xoxoXoxoxoxoXoxx", nullptr, 3},
        {&p, dt::defaults(p), "...x..x....x..o.", nullptr, -1},
    };
    save("loop_house_124_default_kit", sequence(tr, 124.0f, 4, 0.12f));
  }
  {
    KickVoice k; SnareVoice s; ClapVoice c; HatVoice h(false), o(true); PercVoice p;
    Params hp = preset(h, {{H::kDecay, 44}, {H::kTone, 92}});
    Params pp = preset(p, {{P::kModel, P::kZap}, {P::kTune, 50}, {P::kDecay, 44}, {P::kBend, 80}});
    Params sp = preset(s, {{S::kTune, 96}, {S::kDecay, 22}, {S::kSnap, 52}, {S::kTone, 96}, {S::kBody, 28},
                           {S::kBend, 30}, {S::kRing, 100}, {S::kDetune, 76}, {S::kDrive, 70}});
    Params cp = preset(c, {{C::kTone, 70}, {C::kDecay, 40}, {C::kDrive, 70}});
    std::vector<Track> tr = {
        {&k, kickTechno,       "x...x...x...x...x...x...x...x.x.", nullptr, -1},
        {&s, sp,               "..x.....x..x......x.....x..x..x.", nullptr, -1},
        {&c, cp,               "....x.......x...", nullptr, -1},
        {&o, dt::defaults(o),  "..x...x...x...x.", nullptr, -1},
        {&h, hp,               "xxoxxxoxxxoxxXox", nullptr, 3},
        {&p, pp,               ".....x.....o...x......x..o.....x", "mmmmmmmmmmmtmmmrmmmmmmmmmhmmmmmy", -1},
    };
    save("loop_techno_132", sequence(tr, 132.0f, 4, 0.0f));
  }
  {  // 808-flavoured: long tuned kick as bass, cowbell, shaker
    KickVoice k; SnareVoice s; ClapVoice c; HatVoice h(false), o(true); PercVoice p;
    Params pp = preset(p, {{P::kModel, P::kShaker}});
    Params kp = kick808; kp[K::kDecay] = 78;
    Params sp = preset(s, {{S::kTune, 80}, {S::kDecay, 50}, {S::kSnap, 56}, {S::kTone, 84}, {S::kBody, 70},
                           {S::kBend, 10}, {S::kRing, 90}, {S::kDetune, 73}, {S::kDrive, 14}});
    std::vector<Track> tr = {
        {&k, kp,               "x.....x...x.....x.....x...x..x..", "mmmmmmmmmmmmmmmmmmmmmmjmmmpmmrmm", -1},
        {&s, sp,               "....x.......x...", nullptr, -1},
        {&c, dt::defaults(c),  "....x.......x..x", nullptr, -1},
        {&o, dt::defaults(o),  "......x.......x.", nullptr, -1},
        {&h, dt::defaults(h),  "x.x.x.x.x.xxx.x.", nullptr, 3},
        {&p, pp,               "oxoXoxoXoxoXoxoX", nullptr, -1},
    };
    save("loop_808_bass_kick_120", sequence(tr, 120.0f, 4, 0.1f));
  }
  return T.done("test_drums_render");
}
