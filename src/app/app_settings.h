// Pocket Groove firmware -- firmware-level settings (not the UI's settings):
// sample rate, audio backend and the codec (analog) output volume. A 12-byte
// CRC-checked file on internal flash; anything unreadable means defaults.
// UI thread only. Portable: no hardware includes.
#pragma once
#include <cstdint>

#include "pg/platform.h"

namespace app {

#define PG_VERSION "0.1.0-fw1"

constexpr const char* kFwSettingsPath = "/int/pg/_fw.cfg";

enum class Rate : uint8_t { k44100 = 0, k32000 = 1 };
enum class AudioBackend : uint8_t { DirectI2S = 0, M5Speaker = 1, Off = 2 };

// ES8311 reg 0x32: 0xBF = 0 dB, 0.5 dB per step.
// Default 0x70 = -39.5 dB. Reasoning (docs/BRINGUP.md section 4): the smoke
// test starts at 0x80 (-31.5 dB) with -18 dBFS-peak sines (about -21 dBFS RMS)
// and calls that "quiet on headphones". The instrument masters to about
// -14 LUFS, ~7 dB hotter than those sines, so 0x80 would start ~7 dB louder
// than the smoke test's known-quiet level; 0x70 starts ~1 dB below it.
constexpr uint8_t kVolDefault = 0x70;
constexpr uint8_t kVolMax     = 0xBF;   // 0 dB: the loudest clean setting
constexpr uint8_t kVolStep    = 4;      // 2 dB per key press, as in the smoke test
// A saved volume above this is restored only up to it at power-on: whoever
// turned it up on the speaker may plug earbuds in before the next boot.
constexpr uint8_t kVolBootMax = 0xA7;   // -12 dB

struct Settings {
  Rate         rate    = Rate::k44100;
  AudioBackend backend = AudioBackend::DirectI2S;
  uint8_t      volReg  = kVolDefault;
};

inline uint32_t sampleRateHz(Rate r) { return r == Rate::k32000 ? 32000u : 44100u; }
inline float volDb(uint8_t reg) { return (static_cast<int>(reg) - 0xBF) * 0.5f; }
const char* backendName(AudioBackend b);

// false (and *out = defaults) if missing or corrupt.
bool loadSettings(pg::FileSystem& fs, Settings* out);
bool saveSettings(pg::FileSystem& fs, const Settings& s);

}  // namespace app
