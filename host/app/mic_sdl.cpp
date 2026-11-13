// The desktop microphone's SDL source. See mic_sdl.h.
#include "mic_sdl.h"

#include <SDL.h>

namespace desk {

void SdlMicSource::callback(void* self, uint8_t* stream, int len) {
  SdlMicSource* s = static_cast<SdlMicSource*>(self);
  if (s->cb_) s->cb_(reinterpret_cast<const int16_t*>(stream), len / int(sizeof(int16_t)), s->user_);
}

bool SdlMicSource::open(Callback cb, void* user, std::string* err) {
  close();
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
  dev_ = SDL_OpenAudioDevice(wanted_.empty() ? nullptr : wanted_.c_str(), 1, &want, &got, 0);
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
