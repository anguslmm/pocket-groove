// demo_compare: measures the factory demos against a saved copy of themselves
// (stage G, docs/STATE.md: the demos moved from CLASSIC to instruments; this
// tool is how that was done without ears) and fits instruments to a CLASSIC
// track.
//
//   demo_compare --dump DIR
//       writes every demo as built now (raw Project bytes) to DIR/demoN.prj
//   demo_compare --compare DIR [--demo N] [--pattern P|song] [--song] [--wav WAVDIR [--wav-track T]]
//       renders each pattern of each demo twice, from DIR/demoN.prj ("before")
//       and as built now ("after"), through the stereo engine with the stem tap
//       (host/common/bounce.h), and prints per track and for the mix: RMS and
//       peak (dBFS), spectral centroid (Hz), a coarse band balance (the share
//       of energy below 150 Hz, 150 Hz..2 kHz, above 2 kHz, in dB), the
//       track's delay / reverb send, and for the mix: clipped samples, engine
//       faults (NaN / absurd signal) and the ducker (lowest and mean gain over
//       the render, and the bass stem's level just after the kick against just
//       before it). With --wav, writes the mixes as 16-bit WAVs:
//       WAVDIR/demoN_pP_before.wav / _after.wav (song: _song_); --wav-track T also
//       writes track T's dry stem, WAVDIR/demoN_pP_TRACK_before.wav / _after.wav.
//   demo_compare --fit FILE --track T --pattern P [--top K] [--grid G] [--only NAME]
//       FILE is a CLASSIC project (from --dump). For every built-in instrument
//       of track T, a grid of macro positions (G levels per macro) is ranked by
//       how close the derived sound parameters are to the track's own; the best
//       K are rendered (track T's stem in pattern P, the track's step locks
//       removed on both sides so the base sound is compared) and scored on the
//       sound: band shape (10 octave bands, level-normalised) and the envelope
//       over time (10 ms frames, level-normalised); the best few are refined by
//       a coordinate search. Prints the ranking with the level change the
//       mixer would need. --try NAME:A,B,C,D (repeatable) scores only those
//       candidates (positions as bytes).
//   demo_compare --inverse NAME --track T --macro M --param P --pos A,B,C,D --values V,..
//       for each value V of sound parameter P, the position of macro M of
//       instrument NAME (the others at A..D, bytes) that derives the closest
//       value: how a CLASSIC step lock becomes a note's own macro value.
//
// Host tool: heap and the standard library are fine.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/bounce.h"
#include "common/wav.h"
#include "pg/demo/demo.h"
#include "pg/macro/macro.h"

using namespace pg;

namespace {

Instrument g_inst;
macro::ParamTable g_pt;
uint8_t g_def[kNumTracks][kParamsPerTrack];
const char* kTrackNames[kNumTracks] = {"kick", "snare", "clap", "chh", "ohh", "perc", "bass", "lead"};
constexpr float kSr = 44100.0f;

void mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool readProject(const std::string& path, Project& p) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const size_t n = std::fread(&p, 1, sizeof(Project), f);
  std::fclose(f);
  return n == sizeof(Project);
}

bool writeProject(const std::string& path, const Project& p) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const size_t n = std::fwrite(&p, 1, sizeof(Project), f);
  return std::fclose(f) == 0 && n == sizeof(Project);
}

// ---- analysis ------------------------------------------------------------------

void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * M_PI / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

constexpr int kBands = 10;   // octaves centred 31.5 Hz .. 16 kHz
struct Features {
  double rmsDb = -120, peakDb = -120, centroid = 0;
  double band[kBands] = {};   // dB relative to the total
  double low = -120, mid = -120, high = -120;   // dB share below 150 Hz, 150..2k, above 2k
  std::vector<double> env;    // 10 ms frames, dB
  bool silent = true;
};

Features analyse(const std::vector<float>& l, const std::vector<float>& r) {
  Features f;
  const size_t n = l.size();
  double sum = 0, pk = 0;
  for (size_t i = 0; i < n; ++i) {
    const double a = double(l[i]), b = double(r[i]);
    sum += a * a + b * b;
    pk = std::max(pk, std::max(std::fabs(a), std::fabs(b)));
  }
  if (n == 0 || sum <= 0) return f;
  f.silent = false;
  f.rmsDb = 10.0 * std::log10(sum / double(2 * n) + 1e-30);
  f.peakDb = 20.0 * std::log10(pk + 1e-30);
  const size_t fr = size_t(kSr / 100.0f);
  for (size_t i = 0; i + fr <= n; i += fr) {
    double e = 0;
    for (size_t k = i; k < i + fr; ++k) e += double(l[k]) * double(l[k]) + double(r[k]) * double(r[k]);
    f.env.push_back(10.0 * std::log10(e / double(2 * fr) + 1e-12));
  }
  constexpr size_t N = 4096;
  std::vector<double> spec(N / 2, 0.0);
  std::vector<std::complex<double>> buf(N);
  for (size_t i = 0; i + N <= n; i += N / 2) {
    for (size_t k = 0; k < N; ++k) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * double(k) / double(N));
      buf[k] = std::complex<double>(0.5 * (double(l[i + k]) + double(r[i + k])) * w, 0.0);
    }
    fft(buf);
    for (size_t k = 0; k < N / 2; ++k) spec[k] += std::norm(buf[k]);
  }
  double tot = 0, cw = 0, lo = 0, mi = 0, hi = 0;
  double bands[kBands] = {};
  for (size_t k = 1; k < N / 2; ++k) {
    const double hz = double(k) * double(kSr) / double(N);
    tot += spec[k];
    cw += spec[k] * hz;
    if (hz < 150) lo += spec[k];
    else if (hz < 2000) mi += spec[k];
    else hi += spec[k];
    int b = int(std::floor(std::log2(hz / 22.0)));   // 22..44 Hz = band 0
    if (b < 0) b = 0;
    if (b >= kBands) b = kBands - 1;
    bands[b] += spec[k];
  }
  if (tot <= 0) return f;
  f.centroid = cw / tot;
  auto db = [&](double x) { return std::max(-60.0, 10.0 * std::log10(x / tot + 1e-30)); };
  f.low = db(lo);
  f.mid = db(mi);
  f.high = db(hi);
  for (int b = 0; b < kBands; ++b) f.band[b] = db(bands[b]);
  return f;
}

// How different two sounds are, independent of their level: band shape (dB)
// and envelope shape (dB, each relative to its own RMS), RMS over bands / frames.
double soundDistance(const Features& a, const Features& b, double* bandOut = nullptr, double* envOut = nullptr) {
  if (a.silent || b.silent) return 1e9;
  double bs = 0;
  for (int k = 0; k < kBands; ++k) {
    const double d = a.band[k] - b.band[k];
    bs += d * d;
  }
  bs = std::sqrt(bs / kBands);
  const size_t n = std::min(a.env.size(), b.env.size());
  double es = 0;
  int cnt = 0;
  const double floorA = a.rmsDb - 40, floorB = b.rmsDb - 40;
  for (size_t i = 0; i < n; ++i) {
    const double x = std::max(a.env[i], floorA) - a.rmsDb, y = std::max(b.env[i], floorB) - b.rmsDb;
    if (a.env[i] < floorA && b.env[i] < floorB) continue;
    es += (x - y) * (x - y);
    ++cnt;
  }
  es = cnt ? std::sqrt(es / cnt) : 0;
  if (bandOut) *bandOut = bs;
  if (envOut) *envOut = es;
  return bs + 0.5 * es;
}

// ---- rendering -------------------------------------------------------------------

struct Render {
  std::vector<float> trk[kNumTracks][2];
  std::vector<float> dly[2], rev[2], mix[2];
  long clipped = 0;
  uint32_t faults = 0;
  double duckMin = 1, duckMean = 1;
  long duckN = 0;
};

// One pass of pattern `pattern` (-1: the song chain) with a 0.5 s tail.
// `wanted`: bit t = keep track t's stem (the mix and FX returns are always kept).
bool render(const Project& proj, int pattern, uint32_t wanted, Render& out) {
  out = Render{};
  bounce::Options o;
  o.pattern = pattern;
  o.loops = 1;
  o.tailSec = 0.5f;
  o.sampleRate = kSr;
  bounce::Plan plan;
  std::string err;
  if (!bounce::makePlan(proj, o, plan, &err)) {
    std::fprintf(stderr, "plan: %s\n", err.c_str());
    return false;
  }
  bounce::PassResult res;
  double duckSum = 0;
  auto onBlock = [&](const bounce::Block& b) {
    for (int t = 0; t < kNumTracks; ++t)
      if (wanted & (1u << t))
        for (int c = 0; c < 2; ++c) out.trk[t][c].insert(out.trk[t][c].end(), b.track[t][c], b.track[t][c] + b.n);
    for (int c = 0; c < 2; ++c) {
      out.dly[c].insert(out.dly[c].end(), b.delay[c], b.delay[c] + b.n);
      out.rev[c].insert(out.rev[c].end(), b.reverb[c], b.reverb[c] + b.n);
    }
    for (int i = 0; i < b.n; ++i) {
      for (int c = 0; c < 2; ++c) {
        const int16_t s = b.mix[2 * i + c];
        out.mix[c].push_back(float(s) * (1.0f / 32768.0f));
        if (s >= 32767 || s <= -32767) ++out.clipped;
      }
    }
    const EngineStatus st = g_inst.engine().status();
    out.duckMin = std::min(out.duckMin, double(st.duckGain));
    duckSum += double(st.duckGain);
    ++out.duckN;
  };
  if (!bounce::renderPass(g_inst, proj, o, plan, bounce::Pass{}, 0, onBlock, res, &err)) {
    std::fprintf(stderr, "render: %s\n", err.c_str());
    return false;
  }
  out.faults = res.faults;
  out.duckMean = out.duckN ? duckSum / double(out.duckN) : 1;
  return true;
}

// The bass stem's level in the first 16th after each kick hit against the
// 16th before the next beat: how much the ducker pulls it (dB, positive = pumps).
double pumpDb(const Project& p, int pattern, const std::vector<float>& l, const std::vector<float>& r) {
  if (pattern < 0) return 0;
  const double step = double(kSr) * 15.0 / double(p.globals[kGTempo]);
  const TrackPattern& k = p.patterns[pattern].tracks[kTrackKick];
  double after = 0, before = 0;
  const int len = p.patterns[pattern].length;
  for (int s = 0; s + 4 <= len; s += 4) {
    if (!k.steps[s % k.length].on()) continue;
    const size_t a0 = size_t(double(s) * step), a1 = size_t(double(s) * step + step);
    const size_t b0 = size_t(double(s + 3) * step), b1 = size_t(double(s + 4) * step);
    for (size_t i = a0; i < a1 && i < l.size(); ++i) after += double(l[i]) * double(l[i]) + double(r[i]) * double(r[i]);
    for (size_t i = b0; i < b1 && i < l.size(); ++i) before += double(l[i]) * double(l[i]) + double(r[i]) * double(r[i]);
  }
  if (after <= 0 || before <= 0) return 0;
  return 10.0 * std::log10(before / after);
}

// ---- compare -------------------------------------------------------------------

double sendOf(const Project& p, int t, int row) { return double(p.tracks[t].params[row]); }

void printRow(const char* what, const Features& a, const Features& b) {
  std::printf("  %-6s rms %6.1f -> %6.1f (%+5.1f)  peak %6.1f -> %6.1f  cen %5.0f -> %5.0f Hz  "
              "lo/mid/hi %5.1f/%5.1f/%5.1f -> %5.1f/%5.1f/%5.1f\n",
              what, a.rmsDb, b.rmsDb, b.rmsDb - a.rmsDb, a.peakDb, b.peakDb, a.centroid, b.centroid, a.low, a.mid,
              a.high, b.low, b.mid, b.high);
}

void writeWav(const std::string& path, const std::vector<float> (&lr)[2]) {
  std::vector<int16_t> pcm(lr[0].size() * 2);
  for (size_t i = 0; i < lr[0].size(); ++i)
    for (size_t c = 0; c < 2; ++c)
      pcm[2 * i + c] = int16_t(std::clamp(std::lround(double(lr[c][i]) * 32768.0), -32768L, 32767L));
  if (!wav::write(path.c_str(), pcm, int(kSr), 2)) std::fprintf(stderr, "cannot write %s\n", path.c_str());
}

int compare(const std::string& dir, int onlyDemo, int onlyPattern, bool songToo, const std::string& wavDir,
            int wavTrack) {
  static Project before, after;
  int bad = 0;
  for (int d = 0; d < demo::count(); ++d) {
    if (onlyDemo >= 0 && d != onlyDemo) continue;
    if (!readProject(dir + "/demo" + std::to_string(d) + ".prj", before)) {
      std::fprintf(stderr, "cannot read %s/demo%d.prj\n", dir.c_str(), d);
      return 2;
    }
    g_inst.init(kSr, false);
    demo::build(g_inst.engine(), after, d);
    g_inst.engine().sanitizeProject(after);
    g_inst.init(kSr, false);
    g_inst.engine().sanitizeProject(before);
    std::printf("== demo %d %s\n", d, demo::name(d));
    for (int t = 0; t < kNumTracks; ++t) {
      const macro::MacroSet* s = macro::findInstrument(instrumentId(after.tracks[t]));
      std::printf("  %-6s %-8s", kTrackNames[t], s ? s->name : "CLASSIC");
      if (s)
        for (int m = 0; m < s->numMacros; ++m)
          std::printf(" %s=%d%%", s->macros[m].name, (after.tracks[t].macro[m] * 100 + 127) / 255);
      std::printf("   level %d -> %d  dly %d -> %d  rev %d -> %d", before.tracks[t].params[kMixLevel],
                  after.tracks[t].params[kMixLevel], int(sendOf(before, t, kMixDelaySend)),
                  int(sendOf(after, t, kMixDelaySend)), int(sendOf(before, t, kMixReverbSend)),
                  int(sendOf(after, t, kMixReverbSend)));
      if (s) {   // what the instrument adds to the player's sends (macro::combineMixer)
        float fp[4];
        for (int m = 0; m < 4; ++m) fp[m] = macro::posFromByte(after.tracks[t].macro[m]);
        uint8_t out[kParamsPerTrack];
        macro::evaluate(*s, g_pt, fp, g_def[t], out);
        std::printf("  (instrument adds dly %+d rev %+d)", int(out[kMixDelaySend]) - int(g_def[t][kMixDelaySend]),
                    int(out[kMixReverbSend]) - int(g_def[t][kMixReverbSend]));
      }
      std::printf("\n");
    }
    std::vector<int> pats;
    auto used = [&](int pt) {
      for (const TrackPattern& tp : before.patterns[pt].tracks)
        for (const Step& st : tp.steps)
          if (st.on()) return true;
      return false;
    };
    for (int pt = 0; pt < kNumPatterns; ++pt)
      if ((onlyPattern < 0 && used(pt)) || pt == onlyPattern) pats.push_back(pt);
    if (songToo) pats.push_back(-1);
    for (int pt : pats) {
      Render a, b;
      if (!render(before, pt, 0xFF, a) || !render(after, pt, 0xFF, b)) return 2;
      if (pt < 0) std::printf(" -- song\n");
      else std::printf(" -- P%d\n", pt + 1);
      for (int t = 0; t < kNumTracks; ++t) {
        const Features fa = analyse(a.trk[t][0], a.trk[t][1]), fb = analyse(b.trk[t][0], b.trk[t][1]);
        if (fa.silent && fb.silent) continue;
        printRow(kTrackNames[t], fa, fb);
        if (std::fabs(fb.rmsDb - fa.rmsDb) > 1.5) std::printf("         ^ level differs by more than 1.5 dB\n");
      }
      printRow("delay", analyse(a.dly[0], a.dly[1]), analyse(b.dly[0], b.dly[1]));
      printRow("reverb", analyse(a.rev[0], a.rev[1]), analyse(b.rev[0], b.rev[1]));
      printRow("MIX", analyse(a.mix[0], a.mix[1]), analyse(b.mix[0], b.mix[1]));
      std::printf("  clipped %ld -> %ld  faults %u -> %u  duck min %.2f -> %.2f mean %.2f -> %.2f  "
                  "bass pump %.1f -> %.1f dB\n",
                  a.clipped, b.clipped, a.faults, b.faults, a.duckMin, b.duckMin, a.duckMean, b.duckMean,
                  pumpDb(before, pt, a.trk[kTrackBass][0], a.trk[kTrackBass][1]),
                  pumpDb(after, pt, b.trk[kTrackBass][0], b.trk[kTrackBass][1]));
      if (b.clipped || b.faults) ++bad;
      if (!wavDir.empty()) {
        mkdirs(wavDir);
        const std::string stem =
            wavDir + "/demo" + std::to_string(d) + (pt < 0 ? std::string("_song") : "_p" + std::to_string(pt + 1));
        writeWav(stem + "_before.wav", a.mix);
        writeWav(stem + "_after.wav", b.mix);
        if (wavTrack >= 0) {   // the track's own stem too (dry: without the FX returns)
          writeWav(stem + "_" + kTrackNames[wavTrack] + "_before.wav", a.trk[wavTrack]);
          writeWav(stem + "_" + kTrackNames[wavTrack] + "_after.wav", b.trk[wavTrack]);
        }
      }
    }
  }
  return bad ? 1 : 0;
}

// ---- fit -------------------------------------------------------------------------

void stripLocks(Project& p, int track) {
  for (Pattern& pat : p.patterns)
    for (Step& s : pat.tracks[track].steps) s.clearExt();
}

struct Cand {
  const macro::MacroSet* set;
  uint8_t pos[4];
  double pdist = 0, sdist = 0, band = 0, env = 0, dLevel = 0, cen = 0;
  double bands[kBands] = {};
};

double paramDistance(const macro::MacroSet& s, int track, const uint8_t* pos, const uint8_t* want) {
  float fp[4];
  for (int m = 0; m < 4; ++m) fp[m] = macro::posFromByte(pos[m]);
  uint8_t out[kParamsPerTrack];
  macro::evaluate(s, g_pt, fp, g_def[track], out);
  double d = 0;
  for (int i = 0; i < kSoundParams; ++i) {
    const ParamInfo* info = g_pt.info[track][i];
    if (!info) continue;
    const double range = double(info->max - info->min);
    if (range <= 0) continue;
    if (info->fmt == ParamFormat::Enum) d += out[i] == want[i] ? 0.0 : 1.0;
    else d += std::pow(double(int(out[i]) - int(want[i])) / range, 2.0);
  }
  return d;
}

bool scoreCand(const Project& base, int track, int pattern, const Features& ref, Cand& c) {
  static Project p;
  std::memcpy(&p, &base, sizeof(Project));
  setInstrumentId(p.tracks[track], c.set->id);
  for (int m = 0; m < 4; ++m) p.tracks[track].macro[m] = m < c.set->numMacros ? c.pos[m] : 0;
  Render r;
  if (!render(p, pattern, 1u << track, r)) return false;
  const Features f = analyse(r.trk[track][0], r.trk[track][1]);
  c.sdist = soundDistance(ref, f, &c.band, &c.env);
  c.dLevel = f.rmsDb - ref.rmsDb;
  c.cen = f.centroid;
  for (int k = 0; k < kBands; ++k) c.bands[k] = f.band[k];
  return true;
}

void printCand(const Cand& c) {
  std::printf("  %-8s", c.set->name);
  for (int m = 0; m < c.set->numMacros; ++m) std::printf(" %s=%3d", c.set->macros[m].name, c.pos[m]);
  std::printf("   sound %.2f (bands %.2f env %.2f)  level %+5.1f dB  cen %5.0f  params %.3f\n", c.sdist, c.band, c.env,
              c.dLevel, c.cen, c.pdist);
}

int fit(const std::string& file, int track, int pattern, int top, int grid, const char* only,
        const std::vector<std::string>& tries) {
  static Project base;
  if (!readProject(file, base)) {
    std::fprintf(stderr, "cannot read %s\n", file.c_str());
    return 2;
  }
  g_inst.init(kSr, false);
  g_inst.engine().sanitizeProject(base);
  stripLocks(base, track);
  uint8_t want[kParamsPerTrack];
  std::memcpy(want, base.tracks[track].params, sizeof(want));
  Render r;
  if (!render(base, pattern, 1u << track, r)) return 2;
  const Features ref = analyse(r.trk[track][0], r.trk[track][1]);
  std::printf("fit %s P%d: CLASSIC rms %.1f dB centroid %.0f Hz  bands", kTrackNames[track], pattern + 1, ref.rmsDb,
              ref.centroid);
  for (int k = 0; k < kBands; ++k) std::printf(" %.0f", ref.band[k]);
  std::printf("\n");
  if (!tries.empty()) {   // only the given candidates, NAME:a,b,c,d (bytes)
    for (const std::string& tr : tries) {
      Cand c{};
      const size_t colon = tr.find(':');
      const std::string nm = tr.substr(0, colon);
      for (int i = 0; i < macro::instrumentCount(track); ++i)
        if (nm == macro::instrumentAt(track, i)->name) c.set = macro::instrumentAt(track, i);
      if (!c.set) { std::fprintf(stderr, "no instrument %s\n", nm.c_str()); return 2; }
      int v[4] = {0, 0, 0, 0};
      if (colon != std::string::npos) std::sscanf(tr.c_str() + colon + 1, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]);
      for (int m = 0; m < 4; ++m) c.pos[m] = uint8_t(v[m]);
      c.pdist = paramDistance(*c.set, track, c.pos, want);
      if (!scoreCand(base, track, pattern, ref, c)) return 2;
      printCand(c);
      std::printf("           bands");
      for (int k = 0; k < kBands; ++k) std::printf(" %.0f", c.bands[k]);
      std::printf("\n");
    }
    return 0;
  }
  std::vector<Cand> all;
  for (int i = 0; i < macro::instrumentCount(track); ++i) {
    const macro::MacroSet* s = macro::instrumentAt(track, i);
    if (!s || (only && std::strcmp(only, s->name) != 0)) continue;
    std::vector<Cand> mine;
    const int n = s->numMacros;
    int idx[4] = {0, 0, 0, 0};
    for (;;) {
      Cand c{};
      c.set = s;
      for (int m = 0; m < 4; ++m) c.pos[m] = m < n ? uint8_t(idx[m] * 255 / (grid - 1)) : 0;
      c.pdist = paramDistance(*s, track, c.pos, want);
      mine.push_back(c);
      int m = 0;
      while (m < n && ++idx[m] == grid) idx[m++] = 0;
      if (m >= n) break;
    }
    std::sort(mine.begin(), mine.end(), [](const Cand& a, const Cand& b) { return a.pdist < b.pdist; });
    // the parameter ranking is only a coarse guide: keep the best few and a spread of the rest
    for (size_t k = 0; k < mine.size() && k < size_t(top); ++k) all.push_back(mine[k]);
    for (size_t k = size_t(top); k < mine.size(); k += mine.size() / size_t(top) + 1) all.push_back(mine[k]);
  }
  for (Cand& c : all)
    if (!scoreCand(base, track, pattern, ref, c)) return 2;
  std::sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.sdist < b.sdist; });
  // refine the best of each instrument (coordinate search)
  std::vector<Cand> best;
  for (const Cand& c : all) {
    bool have = false;
    for (const Cand& b : best) have = have || b.set == c.set;
    if (!have) best.push_back(c);
  }
  for (Cand& c : best) {
    for (int stepSize : {32, 16, 8}) {
      bool improved = true;
      while (improved) {
        improved = false;
        for (int m = 0; m < c.set->numMacros; ++m)
          for (int dir : {-1, 1}) {
            Cand t = c;
            const int v = int(c.pos[m]) + dir * stepSize;
            if (v < 0 || v > 255) continue;
            t.pos[m] = uint8_t(v);
            t.pdist = paramDistance(*t.set, track, t.pos, want);
            if (!scoreCand(base, track, pattern, ref, t)) return 2;
            if (t.sdist + 1e-9 < c.sdist) {
              c = t;
              improved = true;
            }
          }
      }
    }
  }
  std::sort(best.begin(), best.end(), [](const Cand& a, const Cand& b) { return a.sdist < b.sdist; });
  std::printf("refined, best per instrument:\n");
  for (const Cand& c : best) printCand(c);
  std::printf("grid ranking (top 12):\n");
  for (size_t k = 0; k < all.size() && k < 12; ++k) printCand(all[k]);
  return 0;
}

// ---- inverse ---------------------------------------------------------------------

// For each wanted value of parameter `param`, the position of macro `m` (the
// other macros at `pos`) whose derived value is closest: how a CLASSIC lock
// becomes a note's own macro value.
int inverse(int track, const char* name, int m, int param, const uint8_t* pos, const std::vector<int>& values) {
  const macro::MacroSet* s = nullptr;
  for (int i = 0; i < macro::instrumentCount(track); ++i)
    if (std::strcmp(macro::instrumentAt(track, i)->name, name) == 0) s = macro::instrumentAt(track, i);
  if (!s || m < 0 || m >= s->numMacros) {
    std::fprintf(stderr, "no instrument %s / macro %d on track %d\n", name, m, track);
    return 2;
  }
  for (int v : values) {
    int best = 0, bestErr = 1 << 30;
    for (int b = 0; b <= 255; ++b) {
      float fp[4];
      for (int k = 0; k < 4; ++k) fp[k] = macro::posFromByte(k == m ? uint8_t(b) : pos[k]);
      uint8_t out[kParamsPerTrack];
      macro::evaluate(*s, g_pt, fp, g_def[track], out);
      const int e = std::abs(int(out[param]) - v);
      if (e < bestErr) { bestErr = e; best = b; }
    }
    std::printf("%s %s: %s %d -> position %d (code %d, off by %d)\n", s->name, s->macros[m].name,
                g_pt.info[track][param]->name, v, best, macroPosToCode(best), bestErr);
  }
  return 0;
}

void usage() {
  std::fprintf(stderr,
               "usage: demo_compare --dump DIR\n"
               "       demo_compare --compare DIR [--demo N] [--pattern P|song] [--song] [--wav WAVDIR [--wav-track T]]\n"
               "       demo_compare --fit FILE --track T --pattern P [--top K] [--grid G] [--only NAME]\n"
               "                    [--try NAME:A,B,C,D ...]\n"
               "       demo_compare --inverse NAME --track T --macro M --param P --pos A,B,C,D --values V,V,..\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dump, cmp, fitFile, wavDir;
  int demoN = -1, pattern = -1, track = -1, top = 30, grid = 9, wavTrack = -1;
  bool song = false, songOnly = false;
  const char* only = nullptr;
  const char* invName = nullptr;
  int invMacro = 0, invParam = 0;
  uint8_t invPos[4] = {0, 0, 0, 0};
  std::vector<int> invValues;
  std::vector<std::string> tries;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(a, "--dump")) dump = next();
    else if (!std::strcmp(a, "--compare")) cmp = next();
    else if (!std::strcmp(a, "--fit")) fitFile = next();
    else if (!std::strcmp(a, "--wav")) wavDir = next();
    else if (!std::strcmp(a, "--wav-track")) wavTrack = std::atoi(next());
    else if (!std::strcmp(a, "--demo")) demoN = std::atoi(next());
    else if (!std::strcmp(a, "--pattern")) {
      const char* v = next();
      if (!std::strcmp(v, "song")) song = songOnly = true;
      else pattern = std::atoi(v) - 1;
    } else if (!std::strcmp(a, "--song")) song = true;
    else if (!std::strcmp(a, "--track")) track = std::atoi(next());
    else if (!std::strcmp(a, "--top")) top = std::atoi(next());
    else if (!std::strcmp(a, "--grid")) grid = std::atoi(next());
    else if (!std::strcmp(a, "--only")) only = next();
    else if (!std::strcmp(a, "--try")) tries.push_back(next());
    else if (!std::strcmp(a, "--inverse")) invName = next();
    else if (!std::strcmp(a, "--macro")) invMacro = std::atoi(next());
    else if (!std::strcmp(a, "--param")) invParam = std::atoi(next());
    else if (!std::strcmp(a, "--pos")) {
      int v[4] = {0, 0, 0, 0};
      std::sscanf(next(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]);
      for (int k = 0; k < 4; ++k) invPos[k] = uint8_t(v[k]);
    } else if (!std::strcmp(a, "--values")) {
      for (const char* c = next(); *c;) {
        invValues.push_back(std::atoi(c));
        while (*c && *c != ',') ++c;
        if (*c) ++c;
      }
    }
    else { usage(); return 2; }
  }
  g_inst.init(kSr, false);
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = 0; p < kParamsPerTrack; ++p) {
      g_pt.info[t][p] = g_inst.engine().trackParamInfo(t, p);
      g_def[t][p] = g_inst.project().tracks[t].params[p];
    }
  if (!dump.empty()) {
    mkdirs(dump);
    static Project p;
    for (int d = 0; d < demo::count(); ++d) {
      g_inst.init(kSr, false);
      demo::build(g_inst.engine(), p, d);
      g_inst.engine().sanitizeProject(p);
      const std::string path = dump + "/demo" + std::to_string(d) + ".prj";
      if (!writeProject(path, p)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 2; }
      std::printf("%s: %s\n", path.c_str(), p.name);
    }
    return 0;
  }
  if (!cmp.empty()) {
    // --pattern song: the song chain only; --song: every pattern (or --pattern P) and the song
    const bool onlySong = songOnly && pattern < 0;
    return compare(cmp, demoN, onlySong ? kNumPatterns : pattern, song, wavDir, wavTrack);
  }
  if (invName) return inverse(track, invName, invMacro, invParam, invPos, invValues);
  if (!fitFile.empty()) {
    if (track < 0 || track >= kNumTracks || pattern < 0 || grid < 2) { usage(); return 2; }
    return fit(fitFile, track, pattern, top, grid, only, tries);
  }
  usage();
  return 2;
}
