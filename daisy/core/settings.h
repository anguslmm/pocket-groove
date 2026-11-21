// The Daisy's own settings (stage 1): the pads' calibration and job (drums / notes),
// the display's orientation, the touch mapping, the display's SPI speed. One small file on /int
// (kPath), next to the UI's settings and the session, so it goes into the same flash
// snapshots and survives a firmware update. Changed over serial (">pad ...",
// ">pads ...", ">display ...", ">touch map ..."; docs/DAISY.md), saved at once.
// Format: "PGDS", version, length, the fields little-endian, CRC-32 of all before it.
// Version 2 adds the pads' job (one byte before the CRC), version 3 whether the pots
// are on (one byte after it); older files still load (the job: drums; the pots: off).
// Portable (host/daisy/test_daisy_pads.cpp: round trip, a corrupt file refused).
#pragma once
#include <cstddef>
#include <cstdint>

#include "pads.h"

namespace pgd {

struct DaisySettings {
  static constexpr const char* kPath = "/int/pg/daisy.cfg";
  static constexpr uint8_t kVersion = 3;
  static constexpr size_t kBytesV1 = 8 + PadScanner::kPads * 13 + 11 + 3 + 4;
  static constexpr size_t kBytesV2 = kBytesV1 + 1;
  static constexpr size_t kBytes = kBytesV2 + 1;

  PadCal pads[PadScanner::kPads];
  PadGlobal padGlobal;
  bool displayTurned = false;     // ILI9341: MADCTL 0xE8 (turned over) instead of 0x28
  int8_t touchOrientation = -1;   // -1: follows the display; 0..7 forced (touch.h TouchMap)
  uint8_t spiDivider = 2;         // the display's SPI clock = 25 MHz / this (2, 4, 8, 16)
  uint8_t padJob = 0;             // pg::ui::PadJob: 0 drums, 1 notes
  // The four pots on A0..A3 move the selected track's macros. OFF unless switched on
  // ('p', ">pots on"): an ADC pin with nothing wired floats, and its noise would
  // drag the macros about (the first Seed3 did exactly that: 29 moves in 14 s).
  bool potsOn = false;

  size_t encode(uint8_t* out, size_t cap) const;   // bytes written, 0 if cap is short
  bool decode(const uint8_t* in, size_t n);        // false: not ours or corrupt (nothing changed)
};

}  // namespace pgd
