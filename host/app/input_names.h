// Capture-device names, shared by Listen (listen.h) and the microphone (mic.h):
// the device's own USB audio input, and the name matching both use for
// --listen-device / --mic-device. No SDL in here (the unit tests use it).
#pragma once
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace desk {

constexpr const char* kListenDeviceName = "Pocket Groove";   // the firmware's USB product / audio input name

// Does hay contain needle, ignoring case (ASCII)?
inline bool containsNoCase(const char* hay, const char* needle) {
  const size_t n = std::strlen(needle);
  for (; *hay; ++hay) {
    size_t i = 0;
    while (i < n && hay[i] && std::tolower(static_cast<unsigned char>(hay[i])) ==
                                  std::tolower(static_cast<unsigned char>(needle[i])))
      ++i;
    if (i == n) return true;
  }
  return n == 0;
}

// The name matching of --listen-device and --mic-device: the input called
// exactly `want`, failing that the first whose name contains it (any case).
// -1: none.
inline int findInput(const std::vector<std::string>& names, const std::string& want) {
  for (size_t i = 0; i < names.size(); ++i)
    if (names[i] == want) return int(i);
  for (size_t i = 0; i < names.size(); ++i)
    if (containsNoCase(names[i].c_str(), want.c_str())) return int(i);
  return -1;
}

// Is this the connected device's own audio stream (the input Listen plays)?
inline bool isDeviceStream(const std::string& name) { return containsNoCase(name.c_str(), kListenDeviceName); }

}  // namespace desk
