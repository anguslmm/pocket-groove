// See bench.h.
#include "bench.h"

#include <cstring>

#include "common/grooves.h"   // host/common: the busy pattern, the same as bench_instrument's
#include "output_stage.h"
#include "pg/demo/demo.h"

namespace pgd {

int benchScenes() {
  const int n = 2 + pg::demo::count();
  return n > Bench::kMaxScenes ? Bench::kMaxScenes : n;
}

const char* benchSceneName(int scene) {
  if (scene == 0) return "idle";
  if (scene == benchScenes() - 1) return "busy";
  const char* n = pg::demo::name(scene - 1);
  return n && n[0] ? n : "demo?";
}

void benchSetup(pg::Instrument& inst, int scene, float sampleRate) {
  inst.init(sampleRate, /*monoOutput=*/false);
  if (scene <= 0) return;   // idle: the engine stopped, the default project
  pg::Project& p = inst.project();
  if (scene == benchScenes() - 1) {
    grooves::programBusy(p);
  } else {
    pg::demo::build(inst.engine(), p, scene - 1);
    p.songMode = 1;
    inst.engine().sanitizeProject(p);
  }
  pg::Command c{};
  c.type = pg::CmdType::Play;
  inst.engine().post(c);
}

void Bench::start(pg::Instrument* inst, float sampleRate, int frames, Clock clock, Flush flush, void* user,
                  bool quick, int only) {
  inst_ = inst;
  rate_ = sampleRate;
  frames_ = frames > 0 && frames <= 256 ? frames : 96;
  clock_ = clock;
  flush_ = flush;
  user_ = user;
  warm_ = quick ? kBenchQuickWarm : kBenchWarm;
  measure_ = quick ? kBenchQuickMeasure : kBenchMeasure;
  cold_ = quick ? kBenchQuickCold : kBenchCold;
  split_ = quick ? kBenchQuickSplit : kBenchSplit;
  nScenes_ = benchScenes();
  first_ = only >= 0 && only < nScenes_ ? only : 0;
  last_ = only >= 0 && only < nScenes_ ? only : nScenes_ - 1;
  for (Result& r : res_) r = Result{};
  scene_ = first_;
  pass_ = Pass::Plain;
  phase_ = Phase::Setup;
  setupPass();
}

void Bench::setupPass() {
  Result& r = res_[scene_];
  timing_ = false;
  block_ = 0;
  if (pass_ == Pass::Plain) {
    benchSetup(*inst_, scene_, rate_);
    r.minC = 0xFFFFFFFFu;
    passWarm_ = warm_;
    passBlocks_ = warm_ + measure_;
  } else if (pass_ == Pass::Cold) {
    passWarm_ = 0;   // straight on from pass 1
    passBlocks_ = cold_;
  } else {
    benchSetup(*inst_, scene_, rate_);
    pg::Engine& e = inst_->engine();
    for (int t = 0; t < pg::kNumTracks; ++t) {
      const Group g = t == pg::kTrackBass ? kBass : t == pg::kTrackLead ? kLead : kDrums;
      wrap_[t].wrap(inst_->voice(t), &r.group[g], clock_, user_, &timing_);
      e.setVoice(t, &wrap_[t]);
    }
    wrap_[pg::kNumTracks].wrap(&inst_->sampler(), &r.group[kSampler], clock_, user_, &timing_);
    e.setAltVoice(pg::kTrackPerc, pg::kVoiceSampler, &wrap_[pg::kNumTracks]);
    wrap_[pg::kNumTracks + 1].wrap(&inst_->melodic(), &r.group[kSampler], clock_, user_, &timing_);
    e.setAltVoice(pg::kTrackLead, pg::kVoiceSampler, &wrap_[pg::kNumTracks + 1]);
    passWarm_ = warm_;
    passBlocks_ = warm_ + split_;
  }
  phase_ = Phase::Run;   // the interrupt takes it from here
}

bool Bench::step() {
  if (phase_ != Phase::Done) return false;
  if (pass_ == Pass::Plain) {
    pass_ = Pass::Cold;
  } else if (pass_ == Pass::Cold) {
    pass_ = Pass::Split;
  } else {
    res_[scene_].done = true;
    if (scene_ >= last_) {
      phase_ = Phase::Off;
      pass_ = Pass::None;
      return true;
    }
    ++scene_;
    pass_ = Pass::Plain;
  }
  phase_ = Phase::Setup;
  setupPass();
  return false;
}

bool Bench::audioBlock(float* out, int frames) {
  if (phase_ == Phase::Off) return false;
  if (phase_ == Phase::Run && frames == frames_) {
    Result& r = res_[scene_];
    const int b = block_;
    const bool measured = b >= passWarm_;
    if (pass_ == Pass::Split) timing_ = measured;
    if (pass_ == Pass::Cold && flush_) flush_(user_);
    const uint32_t t0 = clock_(user_);
    inst_->processFloat(scratch_, frames);
    const uint32_t t1 = clock_(user_);
    applyOutputLevel(scratch_, frames, 1.0f);
    const uint32_t t2 = clock_(user_);
    if (measured) {
      const uint32_t c = t1 - t0;
      if (pass_ == Pass::Plain) {
        ++r.blocks;
        r.sum += c;
        r.outSum += t2 - t1;
        if (c < r.minC) r.minC = c;
        if (c > r.maxC) r.maxC = c;
      } else if (pass_ == Pass::Cold) {
        ++r.coldBlocks;
        r.coldSum += c;
        if (c > r.coldMax) r.coldMax = c;
      } else {
        ++r.splitBlocks;
        r.splitSum += c;
      }
    }
    block_ = b + 1;
    if (b + 1 >= passBlocks_) {
      timing_ = false;
      phase_ = Phase::Done;
    }
  }
  std::memset(out, 0, sizeof(float) * 2 * static_cast<size_t>(frames));
  return true;
}

}  // namespace pgd
