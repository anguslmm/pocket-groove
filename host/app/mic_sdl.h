// The desktop microphone's SDL source (mic.h): the capture device chooseMicInput
// picks (mic.h: --mic-device, else the system default unless it is the Pocket
// Groove's own stream, else a microphone by name), chosen again at every open,
// opened asking for mono int16 at 44,100 Hz with no format changes allowed, so SDL
// converts whatever the Mac's input is (rate, channels, float) to exactly that.
// macOS asks for microphone permission the first time; until it is granted the
// input is silence (the page stays WAITING, and Mic says MIC SILENT).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mic.h"

namespace desk {

class SdlMicSource final : public MicSource {
 public:
  void setDevice(const std::string& name) { wanted_ = name; }   // --mic-device: exact or part of a name; "" = choose
  bool open(Callback cb, void* user, std::string* err) override;
  void close() override;
  std::string name() const override { return name_.empty() ? std::string("the default input") : name_; }

  // SDL's capture devices' names (SDL_INIT_AUDIO done).
  static std::vector<std::string> inputNames();
  // The system's default input by name, where this SDL can tell (2.24 and later,
  // and only on backends that implement it); false otherwise.
  static bool defaultInputName(std::string* name);
  // The choice for the current inputs and default (what open() will use).
  MicChoice choose(std::vector<std::string>* inputs = nullptr) const;

 private:
  static void callback(void* self, uint8_t* stream, int len);
  std::string wanted_, name_;
  uint32_t dev_ = 0;
  Callback cb_ = nullptr;
  void* user_ = nullptr;
};

}  // namespace desk
