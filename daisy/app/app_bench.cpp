// The App's bench command (">bench", bench.h) and the report's records of late audio
// blocks (load_meter.h EVENTS). No %f anywhere: newlib-nano's printf has none.
//   >bench            every scene (about a minute), then the table, then a restart
//   >bench quick      short spans (a few seconds: the host tests, a first look)
//   >bench scene N    one scene (0 idle, 1.. the demos, the last one busy)
//   >bench norestart  stay up afterwards (daisy_sim and the tests; on the board the
//                     instrument then plays the bench's last scene until a restart)
#include <cstdio>
#include <cstring>

#include "app.h"
#include "bench_ref.h"

namespace pgd {

namespace {

// x / 100 as "12.34" (x >= 0).
struct Num {
  char s[24];
};
Num hundredths(uint64_t x) {
  Num n;
  std::snprintf(n.s, sizeof(n.s), "%lu.%02lu", static_cast<unsigned long>(x / 100), static_cast<unsigned long>(x % 100));
  return n;
}

float refIr(const char* scene) {
  for (const BenchRef& r : kBenchRef)
    if (r.scene[0] && std::strcmp(r.scene, scene) == 0) return r.irPerFrame;
  return 0.0f;
}

}  // namespace

void App::describeEvent(const LoadMeter::Event& e, char* out, size_t cap) const {
  const uint32_t per = meter_.periodUs() ? meter_.periodUs() : 1;
  char head[16] = "";
  if (e.seq) std::snprintf(head, sizeof(head), "#%lu ", static_cast<unsigned long>(e.seq));
  const char* kind = (e.kind & LoadMeter::kUnderrun) && (e.kind & LoadMeter::kLateStart) ? ", UNDERRUN + LATE START"
                     : (e.kind & LoadMeter::kUnderrun)                                  ? ", UNDERRUN"
                     : (e.kind & LoadMeter::kLateStart)                                 ? ", LATE START"
                                                                                        : "";
  int n = std::snprintf(out, cap, "%sat %lu.%03lu s, block %lu: %luus (%lu%% of the period)%s, %luus after the previous "
                        "block began;", head, static_cast<unsigned long>(e.ms / 1000),
                        static_cast<unsigned long>(e.ms % 1000), static_cast<unsigned long>(e.block),
                        static_cast<unsigned long>(e.us), static_cast<unsigned long>(e.us * 100u / per), kind,
                        static_cast<unsigned long>(e.gapUs));
  struct F {
    uint16_t bit;
    const char* text;
  };
  static const F kF[] = {
      {LoadMeter::kFlagWarmup, "warm-up"},
      {LoadMeter::kFlagFirstBlock, "the first block after the audio started"},
      {LoadMeter::kFlagPlayStart, "the transport started"},
      {LoadMeter::kFlagRefresh, "instruments re-evaluated"},
      {LoadMeter::kFlagPlaying, "playing"},
      {LoadMeter::kFlagQspi, "a QSPI erase/write under way"},
      {LoadMeter::kFlagUsbDown, "USB not configured"},
      {LoadMeter::kFlagMacroMove, "a pot moved a macro"},
      {LoadMeter::kFlagKey, "a key event"},
      {LoadMeter::kFlagSerialRx, "USB bytes in"},
  };
  for (const F& f : kF) {
    if (n < 0 || static_cast<size_t>(n) >= cap) return;
    if (e.ctx.flags & f.bit) n += std::snprintf(out + n, cap - static_cast<size_t>(n), " %s,", f.text);
  }
  if (n < 0 || static_cast<size_t>(n) >= cap) return;
  const int t = e.ctx.task;
  std::snprintf(out + n, cap - static_cast<size_t>(n), " main loop %s%s", t >= 0 && t < sched_.count() ? "in task " : "",
                t >= 0 && t < sched_.count() ? sched_.stats(t).name : "between tasks (or not started)");
}

void App::printAudioEvents() {
  LoadMeter::Event ev[LoadMeter::kKeepFirst + LoadMeter::kKeepLast];
  const int n = meter_.events(ev, static_cast<int>(sizeof(ev) / sizeof(ev[0])));
  char l[360];
  if (n == 0) {
    sp("[audio] late blocks: none since boot\n");
  } else {
    sp("[audio] late blocks: %lu since boot (records: the first %d and the latest %d)\n",
       static_cast<unsigned long>(meter_.eventCount()), LoadMeter::kKeepFirst, LoadMeter::kKeepLast);
    for (int i = 0; i < n; ++i) {
      describeEvent(ev[i], l, sizeof(l));
      sp("[audio] late %s\n", l);
    }
  }
  const LoadMeter::Event lo = meter_.longest();
  if (lo.us) {
    describeEvent(lo, l, sizeof(l));
    sp("[audio] longest after the warm-up: %s\n", l);
  }
}

bool App::benchCommand(char** w, int n) {
  bool quick = false, restart = true;
  int only = -1;
  for (int i = 1; i < n; ++i) {
    if (std::strcmp(w[i], "quick") == 0) quick = true;
    else if (std::strcmp(w[i], "norestart") == 0) restart = false;
    else if (std::strcmp(w[i], "scene") == 0 && i + 1 < n && w[i + 1][0] >= '0' && w[i + 1][0] <= '9')
      only = w[++i][0] - '0';
    else {
      sp("[bench] ? \"%s\": >bench [quick] [scene N] [norestart]\n", w[i]);
      return true;
    }
  }
  if (bench_.active()) {
    sp("[bench] already running\n");
    return true;
  }
  if (!hal_->audioRunning()) {
    sp("[bench] the audio is not running: no interrupt to measure in\n");
    return true;
  }
  benchQuick_ = quick;
  benchRestart_ = restart;
  benchLiveAud_ = meter_.nowPermille();
  // The bench replaces the project and the voices: the session to /int, /int to flash
  // first (as 'R' does), so the restart afterwards finds it as it is now.
  ui_.saveSessionNow();
  persist_.requestNow();
  const uint32_t t0 = hal_->millis();
  while (persist_.dirty() && hal_->millis() - t0 < 5000) persist_.step(hal_->millis());
  const int scenes = benchScenes();
  const int per = quick ? kBenchQuickWarm + kBenchQuickMeasure + kBenchQuickCold + kBenchQuickWarm + kBenchQuickSplit
                        : kBenchWarm + kBenchMeasure + kBenchCold + kBenchWarm + kBenchSplit;
  const uint32_t blockUs = hal_->sampleRate() ? hal_->blockFrames() * 1000000u / hal_->sampleRate() : 2000u;
  const uint32_t secs = static_cast<uint32_t>(per) * (only >= 0 ? 1u : static_cast<uint32_t>(scenes)) * blockUs / 1000000u;
  sp("[bench] start: %d scene(s), about %lu s; the sound stops, the UI pauses%s\n", only >= 0 ? 1 : scenes,
     static_cast<unsigned long>(secs + 1), restart ? ", the board restarts at the end (the session is saved)" : "");
  benchStartMs_ = hal_->millis();
  benchDoneMs_ = 0;
  bench_.start(
      &inst_, cfg_.sampleRate, static_cast<int>(hal_->blockFrames()),
      [](void* u) { return static_cast<App*>(u)->hal_->cycles(); },
      [](void* u) { static_cast<App*>(u)->hal_->flushCaches(); }, this, quick, only);
  return true;
}

void App::serviceBench() {
  if (benchDoneMs_) {   // the table is out: restart once the serial port has drained
    hal_->serialFlush();
    if (hal_->millis() - benchDoneMs_ < 1500) return;
    benchDoneMs_ = 0;
    if (benchRestart_) {
      sp("[bench] restarting\n");
      for (int i = 0; i < 50; ++i) hal_->serialFlush();
      hal_->restart(false);
    }
    return;
  }
  const int before = bench_.scene();
  if (bench_.step()) {
    sp("[bench] %s done\n", benchSceneName(before));
    printBench();
    benchDoneMs_ = hal_->millis() ? hal_->millis() : 1;
    if (!benchRestart_) {
      sp("[bench] not restarting: the instrument now holds the bench's last scene (R then y restarts)\n");
    }
    return;
  }
  if (bench_.scene() != before) sp("[bench] %s done\n", benchSceneName(before));
}

void App::printBench() {
  const uint32_t frames = static_cast<uint32_t>(bench_.frames());
  const uint32_t period = meter_.periodCycles();
  const uint32_t hz = hal_->cpuHz() ? hal_->cpuHz() : 1;
  sp("[bench] Pocket Groove %s, %s, %lu MHz, %lu Hz stereo, %lu-frame blocks (%lu cycles each)\n", cfg_.version,
     hal_->stageName(), static_cast<unsigned long>(hz / 1000000u), static_cast<unsigned long>(hal_->sampleRate()),
     static_cast<unsigned long>(frames), static_cast<unsigned long>(period));
  sp("[bench] method: inside the audio interrupt, one block per callback, the output muted; main loop: serial only; "
     "per scene %d blocks of warm-up, %d measured, %d cold (both caches emptied before each), split: the scene again "
     "with timed voices, %d measured%s\n",
     bench_.warmBlocks(), bench_.measureBlocks(), bench_.coldBlocks(), bench_.splitBlocks(),
     benchQuick_ ? " (QUICK: short spans)" : "");
  sp("[bench] live before the bench: AUD now %lu.%lu%% (the session that was playing)\n",
     static_cast<unsigned long>(benchLiveAud_ / 10), static_cast<unsigned long>(benchLiveAud_ % 10));
  hal_->benchInfo([](void* u, const char* l) { static_cast<App*>(u)->sp("[bench] %s\n", l); }, this);
  sp("[bench] host reference: %s\n", kBenchRefNote);
  sp("[bench] %-9s %7s %7s %7s %6s %7s %7s %5s %7s %6s | %5s %5s %5s %5s %6s\n", "scene", "cyc/fr", "min", "max",
     "load%", "max_us", "cold", "out", "host_Ir", "cyc/Ir", "drums", "bass", "lead", "smpl", "engine");
  for (int s = bench_.firstScene(); s < bench_.scenes(); ++s) {
    const Bench::Result& r = bench_.result(s);
    if (!r.done || !r.blocks) continue;
    const uint64_t avg100 = r.sum * 100u / (static_cast<uint64_t>(r.blocks) * frames);   // cycles / frame x 100
    const uint64_t loadPm = r.sum * 1000u / (static_cast<uint64_t>(r.blocks) * period);
    const uint32_t minF = r.minC / frames, maxF = r.maxC / frames;
    const uint32_t maxUs = static_cast<uint32_t>(static_cast<uint64_t>(r.maxC) * 1000000u / hz);
    const uint64_t cold = r.coldBlocks ? r.coldSum / (static_cast<uint64_t>(r.coldBlocks) * frames) : 0;
    const uint64_t out = r.outSum / (static_cast<uint64_t>(r.blocks) * frames);
    const char* name = benchSceneName(s);
    const float ir = refIr(name);
    char irS[16] = "-", ratio[sizeof(Num::s)] = "-";
    if (ir > 0.0f) {
      const uint64_t ir100 = static_cast<uint64_t>(ir * 100.0f);
      std::snprintf(irS, sizeof(irS), "%lu", static_cast<unsigned long>((ir100 + 50) / 100));
      std::snprintf(ratio, sizeof(ratio), "%s", hundredths(ir100 ? avg100 * 100u / ir100 : 0).s);
    }
    char split[64] = "    -     -     -     -      -";
    if (r.splitBlocks && r.splitSum) {
      uint64_t g[Bench::kGroups];
      uint64_t voices = 0;
      for (int i = 0; i < Bench::kGroups; ++i) {
        g[i] = r.group[i] * 100u / r.splitSum;
        voices += r.group[i];
      }
      const uint64_t eng = voices < r.splitSum ? (r.splitSum - voices) * 100u / r.splitSum : 0;
      std::snprintf(split, sizeof(split), "%4lu%% %4lu%% %4lu%% %4lu%% %5lu%%", static_cast<unsigned long>(g[0]),
                    static_cast<unsigned long>(g[1]), static_cast<unsigned long>(g[2]), static_cast<unsigned long>(g[3]),
                    static_cast<unsigned long>(eng));
    }
    sp("[bench] %-9s %7lu %7lu %7lu %4lu.%lu %7lu %7lu %5lu %7s %6s | %s\n", name,
       static_cast<unsigned long>((avg100 + 50) / 100), static_cast<unsigned long>(minF),
       static_cast<unsigned long>(maxF), static_cast<unsigned long>(loadPm / 10), static_cast<unsigned long>(loadPm % 10),
       static_cast<unsigned long>(maxUs), static_cast<unsigned long>(cold), static_cast<unsigned long>(out), irS, ratio,
       split);
  }
  sp("[bench] columns: cyc/fr cycles per frame (average of the measured blocks); min / max the cheapest and the "
     "dearest block, per frame; load%% that average against a 2 ms block; max_us the dearest block\n");
  sp("[bench] columns: cold with both caches emptied before each block; out the output stage (level, clamp, peak); "
     "host_Ir the host's x86 instructions per frame; cyc/Ir cycles per x86 instruction\n");
  sp("[bench] columns: split, the share of the timed pass: drums, bass, lead, sampler voices, engine (the rest: "
     "sequencer, mixer, FX, master)\n");
  sp("[bench] end (%lu s)\n", static_cast<unsigned long>((hal_->millis() - benchStartMs_) / 1000));
}

}  // namespace pgd
