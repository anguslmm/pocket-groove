// bounce: render a saved project to stems (one stereo WAV per track, the
// delay and reverb returns, and the full mix), for mixing in a DAW.
//
//   bounce FILE.pgv | --demo N   [--out DIR] [--name NAME]
//          [--pattern P] [--loops N] [--tail S] [--wet-stems] [--no-duck]
//          [--sr 44100|32000] [--bits 16|24|32f] [--seed N]
//
// Renders the project's song chain (from slot 1 to the first END) if it has
// one, otherwise pattern 1; --pattern P (1..16) renders pattern P --loops
// times instead. After the last step the sequencer stops firing (nothing is
// choked) and up to --tail seconds (default 8) of release and FX tail follow,
// cut once everything has decayed below -100 dBFS (plus 100 ms).
//
// Files, all stereo, all the same length, all starting at the first downbeat:
//   <NAME>_<BPM>bpm_01_kick.wav ... _08_lead.wav   the tracks
//   <NAME>_<BPM>bpm_09_delay.wav, _10_reverb.wav    the FX returns
//   <NAME>_<BPM>bpm_mix.wav                         the instrument's stereo output
//   <NAME>_<BPM>bpm_info.txt                        what was rendered, levels
// A stem is the track after its level, pan, mute and sidechain duck, before
// the master bus. The 8 stems + 2 returns sum to the pre-master mix (exactly,
// in 32-bit float). See docs/MANUAL.md "Bouncing stems".
//
// Loads .pgv files exactly as the device saved them (pg::io: header and
// version checks, CRCs, PGLZ), including the autosaved session _last.pgv.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/bounce.h"
#include "pg/demo/demo.h"
#include "pg/io/project_io.h"

namespace {

pg::Instrument g_inst;   // ~70 KB: static, one per program (as on the device)
pg::Project g_project;

void usage() {
  std::fprintf(stderr,
               "usage: bounce FILE.pgv | --demo N  [--out DIR] [--name NAME] [--pattern P] [--loops N]\n"
               "              [--tail S] [--wet-stems] [--no-duck] [--sr 44100|32000] [--bits 16|24|32f] [--seed N]\n");
}

bool mkdirs(const std::string& dir) {
  std::string cur;
  for (size_t i = 0; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
      cur = dir.substr(0, i);
      if (!cur.empty() && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return true;
}

bool loadPgv(const std::string& path, pg::Project& dest, pg::io::HeaderInfo* info) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { std::fprintf(stderr, "bounce: cannot open %s: %s\n", path.c_str(), std::strerror(errno)); return false; }
  std::vector<uint8_t> buf;
  uint8_t tmp[4096];
  size_t n;
  while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
  std::fclose(f);
  const pg::io::Status st = pg::io::decodeFromBuffer(buf.data(), buf.size(), dest, info);
  if (st != pg::io::Status::Ok) {
    std::fprintf(stderr, "bounce: %s: %s%s\n", path.c_str(), pg::io::statusName(st),
                 st == pg::io::Status::NewerFormat || st == pg::io::Status::ProjectVersion
                     ? " (saved by newer firmware: update this repository)" : "");
    return false;
  }
  return true;
}

// File-name safe: A-Z a-z 0-9 - _ ; empty -> "PROJECT".
std::string safeName(const std::string& s) {
  std::string o;
  for (char c : s)
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') o += c;
  return o.empty() ? "PROJECT" : o;
}

std::string baseName(const std::string& path) {
  std::string b = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
  const size_t dot = b.rfind('.');
  return dot == std::string::npos ? b : b.substr(0, dot);
}

// Removes files of an earlier bounce of the same project (<prefix>_*bpm_*.wav / _info.txt),
// so a re-bounce with other options leaves no stale stems behind.
void removeOld(const std::string& dir, const std::string& name) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  const std::string pre = name + "_";
  while (dirent* e = ::readdir(d)) {
    const std::string f = e->d_name;
    if (f.compare(0, pre.size(), pre) != 0) continue;
    const size_t bpm = f.find("bpm_", pre.size());
    if (bpm == std::string::npos) continue;
    bool digits = bpm > pre.size();
    for (size_t i = pre.size(); i < bpm; ++i) digits = digits && f[i] >= '0' && f[i] <= '9';
    const bool ext = (f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0) ||
                     (f.size() > 9 && f.compare(f.size() - 9, 9, "_info.txt") == 0);
    if (digits && ext) ::unlink((dir + "/" + f).c_str());
  }
  ::closedir(d);
}

double db(double x) { return 20.0 * std::log10(x > 1e-12 ? x : 1e-12); }

}  // namespace

int main(int argc, char** argv) {
  bounce::Request rq;
  std::string file, outDir, name;
  int demo = -1, patternArg = 0;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(a, "--out")) outDir = next();
    else if (!std::strcmp(a, "--name")) name = next();
    else if (!std::strcmp(a, "--demo")) demo = std::atoi(next());
    else if (!std::strcmp(a, "--pattern")) patternArg = std::atoi(next());
    else if (!std::strcmp(a, "--loops")) rq.opt.loops = std::atoi(next());
    else if (!std::strcmp(a, "--tail")) rq.opt.tailSec = float(std::atof(next()));
    else if (!std::strcmp(a, "--wet-stems")) rq.wet = true;
    else if (!std::strcmp(a, "--no-duck")) rq.noDuck = true;
    else if (!std::strcmp(a, "--sr")) rq.opt.sampleRate = float(std::atof(next()));
    else if (!std::strcmp(a, "--seed")) rq.opt.seed = uint32_t(std::strtoul(next(), nullptr, 0));
    else if (!std::strcmp(a, "--bits")) {
      const std::string b = next();
      if (b == "16") rq.bits = bounce::Bits::k16;
      else if (b == "24") rq.bits = bounce::Bits::k24;
      else if (b == "32f" || b == "32") rq.bits = bounce::Bits::k32f;
      else { std::fprintf(stderr, "bounce: --bits must be 16, 24 or 32f\n"); return 2; }
    } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { usage(); return 0; }
    else if (a[0] == '-') { usage(); return 2; }
    else if (file.empty()) file = a;
    else { usage(); return 2; }
  }
  if (rq.opt.sampleRate != 44100.0f && rq.opt.sampleRate != 32000.0f) {
    std::fprintf(stderr, "bounce: --sr must be 44100 or 32000\n");
    return 2;
  }
  if (patternArg < 0 || patternArg > pg::kNumPatterns) {
    std::fprintf(stderr, "bounce: --pattern must be 1..%d\n", pg::kNumPatterns);
    return 2;
  }
  rq.opt.pattern = patternArg - 1;
  if (rq.opt.loops < 1 || rq.opt.loops > 999) { std::fprintf(stderr, "bounce: --loops must be 1..999\n"); return 2; }
  if (!(rq.opt.tailSec >= 0.0f && rq.opt.tailSec <= 60.0f)) { std::fprintf(stderr, "bounce: --tail must be 0..60\n"); return 2; }
  if (file.empty() == (demo < 0)) { usage(); return 2; }

  // ---- load ----
  g_inst.init(rq.opt.sampleRate, false);   // voices first: the sanitizer needs their param ranges
  std::string source;
  if (demo >= 0) {
    if (demo >= pg::demo::count()) { std::fprintf(stderr, "bounce: --demo must be 0..%d\n", pg::demo::count() - 1); return 2; }
    pg::demo::build(g_inst.engine(), g_project, demo);
    source = std::string("factory demo ") + pg::demo::name(demo);
  } else {
    pg::io::HeaderInfo hi{};
    if (!loadPgv(file, g_project, &hi)) return 1;
    source = file + " (format v" + std::to_string(hi.formatVersion) + ", project v" + std::to_string(hi.projectVersion) + ")";
  }
  g_inst.engine().sanitizeProject(g_project);
  if (name.empty()) {
    char pn[17] = {};
    std::memcpy(pn, g_project.name, 16);
    name = pn;
    if (name.empty() && !file.empty()) name = baseName(file);
  }
  name = safeName(name);
  if (outDir.empty()) outDir = "bounces/" + name;

  bounce::Plan plan;
  std::string err;
  if (!bounce::makePlan(g_project, rq.opt, plan, &err)) { std::fprintf(stderr, "bounce: %s\n", err.c_str()); return 1; }
  std::vector<bounce::Pass> passes;
  std::vector<bounce::OutputSpec> outs;
  bounce::layout(rq, passes, outs);

  // ---- run 1: measure (length, peaks), and check every pass ends at the same frame ----
  const size_t nOut = outs.size();
  std::vector<double> peak(nOut, 0.0);
  std::vector<long> lastLoud(nOut, -1);
  const float kSilent = 1.0e-5f;   // -100 dBFS
  std::vector<bounce::PassResult> res1(passes.size());
  std::vector<float> l(256), r(256);
  long endFrame = -1, rendered = 0;
  uint32_t faults = 0;
  for (size_t p = 0; p < passes.size(); ++p) {
    auto onBlock = [&](const bounce::Block& b) {
      for (size_t k = 0; k < nOut; ++k) {
        if (outs[k].pass != int(p)) continue;
        bounce::extract(outs[k], b, l.data(), r.data());
        for (int i = 0; i < b.n; ++i) {
          const double m = std::max(std::fabs(l[size_t(i)]), std::fabs(r[size_t(i)]));
          if (m > peak[k]) peak[k] = m;
          if (m > double(kSilent)) lastLoud[k] = b.frame + i;
        }
      }
    };
    if (!bounce::renderPass(g_inst, g_project, rq.opt, plan, passes[p], int(p), onBlock, res1[p], &err)) {
      std::fprintf(stderr, "bounce: %s\n", err.c_str());
      return 1;
    }
    if (p == 0) { endFrame = res1[p].endFrame; rendered = res1[p].frames; }
    else if (res1[p].endFrame != endFrame || res1[p].frames != rendered) {
      std::fprintf(stderr, "bounce: internal error: pass %zu ended at frame %ld, pass 0 at %ld\n", p, res1[p].endFrame, endFrame);
      return 1;
    }
    faults += res1[p].faults;
  }
  long len = endFrame;
  for (long ll : lastLoud) len = std::max(len, ll + 1);
  len = std::min(rendered, len + long(0.1f * rq.opt.sampleRate));

  // Integer formats: one common gain for every stem (never the mix) so that
  // none clips; whole 3 dB steps, loudest stem at or below -1 dBFS.
  double stemPeak = 0.0;
  for (size_t k = 0; k < nOut; ++k)
    if (!outs[k].mix) stemPeak = std::max(stemPeak, peak[k]);
  int gainDb = 0;
  if (rq.bits != bounce::Bits::k32f) {
    const double ceilDb = -1.0;
    while (db(stemPeak) + gainDb > ceilDb) gainDb -= 3;
  }
  const float gain = float(std::pow(10.0, gainDb / 20.0));

  // ---- run 2: write ----
  if (!mkdirs(outDir)) { std::fprintf(stderr, "bounce: cannot create %s: %s\n", outDir.c_str(), std::strerror(errno)); return 1; }
  removeOld(outDir, name);
  const std::string prefix = outDir + "/" + name + "_" + std::to_string(plan.bpm) + "bpm_";
  std::vector<std::string> paths(nOut);
  std::vector<std::unique_ptr<bounce::WavWriter>> w(nOut);
  for (size_t k = 0; k < nOut; ++k) {
    char num[24];
    std::snprintf(num, sizeof(num), "%02zu_", k + 1);
    paths[k] = prefix + (outs[k].mix ? std::string() : std::string(num)) + outs[k].label + ".wav";
    w[k].reset(new bounce::WavWriter);
    if (!w[k]->open(paths[k], int(rq.opt.sampleRate), rq.bits, len)) {
      std::fprintf(stderr, "bounce: cannot write %s: %s\n", paths[k].c_str(), std::strerror(errno));
      return 1;
    }
  }
  for (size_t p = 0; p < passes.size(); ++p) {
    bounce::PassResult res2;
    auto onBlock = [&](const bounce::Block& b) {
      if (b.frame >= len) return;
      const int n = int(std::min<long>(b.n, len - b.frame));
      for (size_t k = 0; k < nOut; ++k) {
        if (outs[k].pass != int(p)) continue;
        bounce::extract(outs[k], b, l.data(), r.data());
        w[k]->write(l.data(), r.data(), n, outs[k].mix ? 1.0f : gain);
      }
    };
    if (!bounce::renderPass(g_inst, g_project, rq.opt, plan, passes[p], int(p), onBlock, res2, &err)) {
      std::fprintf(stderr, "bounce: %s\n", err.c_str());
      return 1;
    }
    if (res2.hash != res1[p].hash) {
      std::fprintf(stderr, "bounce: internal error: pass %zu is not deterministic\n", p);
      return 1;
    }
  }
  for (size_t k = 0; k < nOut; ++k)
    if (!w[k]->close()) { std::fprintf(stderr, "bounce: write error on %s\n", paths[k].c_str()); return 1; }

  // ---- info file ----
  const double sr = rq.opt.sampleRate;
  const double barSec = 4.0 * 60.0 / plan.bpm;
  const char* bitsName = rq.bits == bounce::Bits::k16 ? "16-bit PCM (TPDF dither)"
                         : rq.bits == bounce::Bits::k24 ? "24-bit PCM" : "32-bit float";
  std::string info;
  char line[512];
  std::snprintf(line, sizeof(line), "Pocket Groove stem bounce\n\nproject   %s\nsource    %s\n", name.c_str(), source.c_str());
  info += line;
  if (plan.song)
    std::snprintf(line, sizeof(line), "rendered  song chain, %d pattern plays, %ld steps (%.2f bars of 4/4)\n", plan.plays,
                  plan.steps, plan.steps / 16.0);
  else
    std::snprintf(line, sizeof(line), "rendered  pattern %d x %d, %ld steps (%.2f bars of 4/4)\n", plan.pattern + 1,
                  plan.plays, plan.steps, plan.steps / 16.0);
  info += line;
  std::snprintf(line, sizeof(line),
                "tempo     %d BPM (one bar = %.4f s), swing %d\nformat    %s stereo, %d Hz, %ld frames (%.3f s)\n"
                "song end  frame %ld (%.3f s); the rest is release and FX tail\n",
                plan.bpm, barSec, g_project.globals[pg::kGSwing], bitsName, int(sr), len, len / sr, endFrame,
                endFrame / sr);
  info += line;
  std::snprintf(line, sizeof(line), "stems     %s, %s\nstem gain %+d dB (every stem and return; the mix is untouched)\n",
                rq.wet ? "wet (each track with its own delay and reverb)" : "dry, plus delay and reverb returns",
                rq.noDuck ? "sidechain ducking OFF" : "with sidechain ducking", gainDb);
  info += line;
  info += "\nAll files start at the first downbeat (bar 1) and have the same length.\n"
          "Stems are post track level/pan/mute/duck, pre master bus. At unity gain they sum\n"
          "to the mix before the master section (DJ filter, drive, master volume, limiter).\n\n";
  for (size_t k = 0; k < nOut; ++k) {
    const double pk = outs[k].mix ? peak[k] : peak[k] * double(gain);
    const std::string base = paths[k].substr(outDir.size() + 1);
    std::snprintf(line, sizeof(line), "  %-36s peak %7.2f dBFS%s\n", base.c_str(), db(pk),
                  peak[k] < double(kSilent) ? "  (silent)" : "");
    info += line;
  }
  if (faults) {
    std::snprintf(line, sizeof(line), "\nWARNING: the engine reported %u faults (a voice or FX went non-finite)\n", faults);
    info += line;
  }
  const std::string infoPath = prefix + "info.txt";
  FILE* fi = std::fopen(infoPath.c_str(), "w");
  if (!fi || std::fputs(info.c_str(), fi) < 0 || std::fclose(fi) != 0) {
    std::fprintf(stderr, "bounce: cannot write %s\n", infoPath.c_str());
    return 1;
  }
  std::printf("bounce: %s -> %s/\n", name.c_str(), outDir.c_str());
  std::printf("  %s, %d BPM, %.1f s (%.1f s song + tail), %s, %d Hz, stem gain %+d dB, %zu passes\n",
              plan.song ? "song" : "pattern", plan.bpm, len / sr, endFrame / sr, bitsName, int(sr), gainDb,
              passes.size());
  for (size_t k = 0; k < nOut; ++k)
    std::printf("  %-40s peak %7.2f dBFS\n", paths[k].substr(outDir.size() + 1).c_str(),
                db(outs[k].mix ? peak[k] : peak[k] * double(gain)));
  if (faults) std::printf("  WARNING: %u engine faults\n", faults);
  return 0;
}
