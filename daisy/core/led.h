// What the Seed's on-board LED says (the owner's first sign of life, stage 0):
//   Booting      on, steady (from the first line of main() until the main loop runs)
//   Running      a heartbeat: two short flashes every second (audio running)
//   Playing      a slow 50 % blink (0.5 s on, 0.5 s off) while the transport runs
//   NoAudio      fast blink (5 Hz): the audio did not start
//   RomMissing   three short flashes, pause: the ROM part of the image is missing
//                (only the first 480 KB were flashed; flash the whole .bin again)
//   Fault        on, steady, after a hard fault (the fault handler holds it)
// The Daisy bootloader before it: a slow sine-like pulse for ~2.5 s (its grace
// period), an SOS pattern if it rejected the image.
// Portable (host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstdint>

namespace pgd {

enum class LedMode : uint8_t { Booting, Running, Playing, NoAudio, RomMissing };

inline bool ledOn(LedMode m, uint32_t nowMs) {
  switch (m) {
    case LedMode::Booting:
      return true;
    case LedMode::Running: {
      const uint32_t t = nowMs % 1000u;
      return t < 60u || (t >= 160u && t < 220u);
    }
    case LedMode::Playing:
      return (nowMs % 1000u) < 500u;
    case LedMode::NoAudio:
      return (nowMs % 200u) < 100u;
    case LedMode::RomMissing: {
      const uint32_t t = nowMs % 1500u;
      return t < 600u && (t % 200u) < 80u;
    }
  }
  return false;
}

inline const char* ledModeName(LedMode m) {
  switch (m) {
    case LedMode::Booting: return "steady (booting)";
    case LedMode::Running: return "double flash every second (running, stopped)";
    case LedMode::Playing: return "slow blink (playing)";
    case LedMode::NoAudio: return "fast blink (NO AUDIO)";
    case LedMode::RomMissing: return "three flashes and a pause (ROM PART MISSING)";
  }
  return "?";
}

}  // namespace pgd
