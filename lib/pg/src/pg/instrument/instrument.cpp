#include "instrument.h"

#include <new>

#include "../engine/engine_hooks.h"

namespace pg {

Instrument::Instrument() : hatClosed_(false), hatOpen_(true) {}

Voice* Instrument::voice(int track) {
  switch (track) {
    case kTrackKick: return &kick_;
    case kTrackSnare: return &snare_;
    case kTrackClap: return &clap_;
    case kTrackHatClosed: return &hatClosed_;
    case kTrackHatOpen: return &hatOpen_;
    case kTrackPerc: return &perc_;
    case kTrackBass: return &bass_;
    case kTrackLead: return &lead_;
    default: return nullptr;
  }
}

void Instrument::init(float sampleRate, bool monoOutput) {
  sampleRate_ = sampleRate;
  mono_ = monoOutput;
  // Re-construct the voices in place (no heap): some state is deliberately
  // not reset by Voice::init (the hats' free-running oscillator bank, noise
  // generator seeds), so this is what makes every init() start from the same
  // state and a render reproducible for a given RNG seed.
  kick_.~KickVoice();         new (&kick_) drums::KickVoice();
  snare_.~SnareVoice();       new (&snare_) drums::SnareVoice();
  clap_.~ClapVoice();         new (&clap_) drums::ClapVoice();
  hatClosed_.~HatVoice();     new (&hatClosed_) drums::HatVoice(false);
  hatOpen_.~HatVoice();       new (&hatOpen_) drums::HatVoice(true);
  perc_.~PercVoice();         new (&perc_) drums::PercVoice();
  bass_.~BassVoice();         new (&bass_) bass::BassVoice();
  lead_.~LeadVoice();         new (&lead_) lead::LeadVoice();
  for (int t = 0; t < kNumTracks; ++t) {
    Voice* v = voice(t);
    v->init(sampleRate);
    engine_.setVoice(t, v);
  }
  // initProject reads the voices' ParamInfo defaults, so it comes after setVoice.
  engine_.initProject(project_);
  engine_.init(sampleRate, &project_);
  engine_.setMonoOutput(monoOutput);
}

size_t Instrument::staticRamBytes() { return sizeof(Instrument) + engine_detail::staticRamBytes(); }

}  // namespace pg
