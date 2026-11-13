// The desktop microphone's SDL source (mic.h): the system's default capture device,
// opened asking for mono int16 at 44,100 Hz with no format changes allowed, so SDL
// converts whatever the Mac's input is (rate, channels, float) to exactly that.
// macOS asks for microphone permission the first time; until it is granted the
// input is silence (the page stays WAITING).
#pragma once
#include <cstdint>
#include <string>

#include "mic.h"

namespace desk {

class SdlMicSource final : public MicSource {
 public:
  void setDevice(const std::string& name) { wanted_ = name; }   // "" = the system's default input
  bool open(Callback cb, void* user, std::string* err) override;
  void close() override;
  std::string name() const override { return wanted_.empty() ? std::string("the default input") : wanted_; }

 private:
  static void callback(void* self, uint8_t* stream, int len);
  std::string wanted_;
  uint32_t dev_ = 0;
  Callback cb_ = nullptr;
  void* user_ = nullptr;
};

}  // namespace desk
