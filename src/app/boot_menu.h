// Pocket Groove firmware -- the boot menu for a first flash on unknown hardware.
//
// Entered when a matrix key is held at power-on (G0 cannot be used for that:
// it is the ESP32-S3 download-mode strap), when "m" arrives on the serial port
// during boot, or automatically when the previous run ended in a crash,
// watchdog reset or brownout -- so a firmware that crashes while loading its
// saved state can never trap the device in a boot loop: the menu comes up
// before any of that runs, and "reset session" removes the saved state.
//
// Keys: ; / . (up/down) or 1..6, ENTER/SPACE to change/run, ` to start.
// Without a working keyboard: G0 tap = next item, G0 hold 1 s = select;
// serial: 1..6, j/k, Enter.
// Runs on the UI thread before audio and the UI start. Portable.
#pragma once
#include <cstdint>

#include "app_settings.h"
#include "pg/platform.h"

namespace app {

struct BootMenuEnv {
  pg::Framebuffer* fb;
  pg::FileSystem*  fs;
  Settings*        settings;     // edited in place and saved on every change
  const char*      resetReason;  // e.g. "PANIC"
  bool             abnormalReset;
  bool             intMounted;
};

// Blocks until the user picks START. Returns the key mask still held at exit
// (their key-ups must not reach the UI).
uint64_t runBootMenu(const BootMenuEnv& env);

// Shared with main: a full-screen message (title + up to 3 lines).
void drawMessage(pg::Framebuffer& fb, const char* title, const char* l1, const char* l2 = nullptr,
                 const char* l3 = nullptr, uint8_t colour = 0);

}  // namespace app
