#include "app_settings.h"

#include <cstring>

#include "pg/io/project_io.h"

namespace app {

namespace {
// "PGFW" ver rate backend vol 0 0 0 crc32(bytes 0..7) -> 12 bytes
constexpr uint8_t kVersion = 1;
constexpr size_t kSize = 12;
}  // namespace

const char* backendName(AudioBackend b) {
  switch (b) {
    case AudioBackend::DirectI2S: return "DIRECT I2S";
    case AudioBackend::M5Speaker: return "M5.SPEAKER";
    case AudioBackend::Off:       return "OFF";
  }
  return "?";
}

bool loadSettings(pg::FileSystem& fs, Settings* out) {
  *out = Settings();
  if (!fs.mounted("/int")) return false;
  uint8_t b[kSize + 4] = {};
  size_t len = 0;
  if (!fs.read(kFwSettingsPath, b, sizeof(b), &len) || len != kSize) return false;
  if (memcmp(b, "PGFW", 4) != 0 || b[4] != kVersion) return false;
  const uint32_t crc = pg::io::crc32(0, b, 8);
  const uint32_t got = uint32_t(b[8]) | (uint32_t(b[9]) << 8) | (uint32_t(b[10]) << 16) | (uint32_t(b[11]) << 24);
  if (crc != got) return false;
  Settings s;
  s.rate = b[5] == 1 ? Rate::k32000 : Rate::k44100;
  s.backend = b[6] <= 2 ? static_cast<AudioBackend>(b[6]) : AudioBackend::DirectI2S;
  s.volReg = b[7] > kVolMax ? kVolMax : b[7];
  *out = s;
  return true;
}

bool saveSettings(pg::FileSystem& fs, const Settings& s) {
  if (!fs.mounted("/int")) return false;
  uint8_t b[kSize] = {'P', 'G', 'F', 'W', kVersion, static_cast<uint8_t>(s.rate), static_cast<uint8_t>(s.backend),
                      s.volReg, 0, 0, 0, 0};
  const uint32_t crc = pg::io::crc32(0, b, 8);
  for (int i = 0; i < 4; ++i) b[8 + i] = static_cast<uint8_t>(crc >> (8 * i));
  return fs.write(kFwSettingsPath, b, sizeof(b));
}

}  // namespace app
