// The desktop microphone's SDL source. See mic_sdl.h.
#include "mic_sdl.h"

#include <SDL.h>

namespace desk {

std::vector<std::string> SdlMicSource::inputNames() {
  std::vector<std::string> v;
  const int n = SDL_GetNumAudioDevices(1);   // -1: SDL cannot list them
  for (int i = 0; i < n; ++i)
    if (const char* nm = SDL_GetAudioDeviceName(i, 1)) v.push_back(nm);
  return v;
}

bool SdlMicSource::defaultInputName(std::string* name) {
#if SDL_VERSION_ATLEAST(2, 24, 0)
  // Built against 2.24 or later; the library at run time is at least as new (or
  // sdl2-compat). Backends that cannot tell (dummy, disk, older ones) fail here.
  char* nm = nullptr;
  SDL_AudioSpec spec;
  SDL_zero(spec);
  if (SDL_GetDefaultAudioInfo(&nm, &spec, 1) == 0 && nm) {
    *name = nm;
    SDL_free(nm);
    return !name->empty();
  }
  if (nm) SDL_free(nm);
#else
  (void)name;
#endif
  return false;
}

MicChoice SdlMicSource::choose(std::vector<std::string>* inputs) const {
  std::vector<std::string> names = inputNames();
  std::string def;
  const bool haveDef = wanted_.empty() && defaultInputName(&def);
  MicChoice c = chooseMicInput(names, haveDef ? &def : nullptr, wanted_);
  if (inputs) inputs->swap(names);
  return c;
}

void SdlMicSource::callback(void* self, uint8_t* stream, int len) {
  SdlMicSource* s = static_cast<SdlMicSource*>(self);
  if (s->cb_) s->cb_(reinterpret_cast<const int16_t*>(stream), len / int(sizeof(int16_t)), s->user_);
}

bool SdlMicSource::open(Callback cb, void* user, std::string* err) {
  close();
  // Chosen again at every hold of L: inputs come and go (the device plugged in).
  const MicChoice c = choose();
  name_ = c.name;
  if (!c.ok) {
    *err = c.why;
    return false;
  }
  cb_ = cb;
  user_ = user;
  SDL_AudioSpec want, got;
  SDL_zero(want);
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  want.callback = &SdlMicSource::callback;
  want.userdata = this;
  dev_ = SDL_OpenAudioDevice(c.open.empty() ? nullptr : c.open.c_str(), 1, &want, &got, 0);
  if (!dev_) {
    *err = SDL_GetError();
    return false;
  }
  SDL_PauseAudioDevice(dev_, 0);
  return true;
}

void SdlMicSource::close() {
  if (dev_) SDL_CloseAudioDevice(dev_);
  dev_ = 0;
}

}  // namespace desk
