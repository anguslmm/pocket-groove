// pg::Instrument: the whole groovebox in one object. Owns the eight concrete
// voices (in track order), the Engine handle and one Project, all as plain
// members: no heap. The firmware and the host tools each declare ONE of these
// with static storage duration (it is ~70 KB, mostly the Project; never put it
// on a task stack). The engine's own state (queue, sequencer, delay line,
// reverb pool) is a separate static block inside engine.cpp, and there is only
// one engine per program, so there must only be one Instrument.
//
//   static pg::Instrument inst;
//   inst.init(44100.0f, true);        // true = device (mono codec) mode
//   inst.engine().post(...);          // UI thread
//   inst.process(buf, frames);        // audio thread, interleaved stereo int16
#pragma once
#include <cstddef>
#include <cstdint>

#include "../engine.h"
#include "../bass/bass.h"
#include "../drums/drums.h"
#include "../lead/lead.h"
#include "../sampler/sampler.h"

namespace pg {

class Instrument {
 public:
  Instrument();

  // Not real-time. Re-creates and initialises every voice at `sampleRate`
  // (so repeated init() calls start from identical state), plugs them into
  // the engine, fills the Project with Engine::initProject and starts the
  // engine on it (stopped). monoOutput selects device mode
  // (Engine::setMonoOutput): one bus, mono FX, L == R.
  void init(float sampleRate, bool monoOutput);

  Engine& engine() { return engine_; }
  const Engine& engine() const { return engine_; }
  // The UI reads this directly; while the engine runs, change it only through
  // Engine::post (see project.h / engine.h). To load a whole project:
  // engine().suspend(); copy in; engine().sanitizeProject(project()); engine().resume().
  Project& project() { return project_; }
  const Project& project() const { return project_; }

  Voice* voice(int track);   // the track's native voice
  // The PERC track's sampler (VOICE KINDS: a `voice perc sampler` instrument plays
  // through it) and the pool it reads (nullptr: the built-in sounds only). The
  // platform sets its pool (flash on the device, RAM on the desktop) after init(),
  // before audio starts or with the engine suspended; init() keeps it.
  sampler::SamplerVoice& sampler() { return sampler_; }
  const sampler::SamplerVoice& sampler() const { return sampler_; }
  void setSamplePool(const sampler::SamplePool* pool);
  float sampleRate() const { return sampleRate_; }
  bool monoOutput() const { return mono_; }

  // Audio thread: `frames` (a multiple of kQuantum) of interleaved stereo int16.
  void process(int16_t* out, int frames) { engine_.process(out, frames); }

  // Static RAM this object and the engine's internal state block occupy
  // (sizeof(Instrument) + the engine's statically allocated state). Voice
  // lookup tables (lead wavetables, bass tables) are separate, see the report
  // printed by host/tools/bench_instrument.
  static size_t staticRamBytes();

 private:
  drums::KickVoice  kick_;
  drums::SnareVoice snare_;
  drums::ClapVoice  clap_;
  drums::HatVoice   hatClosed_;
  drums::HatVoice   hatOpen_;
  drums::PercVoice  perc_;
  bass::BassVoice   bass_;
  lead::LeadVoice   lead_;
  sampler::SamplerVoice sampler_;
  const sampler::SamplePool* pool_ = nullptr;
  Engine            engine_;
  Project           project_;
  float sampleRate_ = 0.0f;
  bool mono_ = false;
};

}  // namespace pg
