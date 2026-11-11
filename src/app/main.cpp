// Pocket Groove -- the instrument (env:cardputer-adv). Not built yet: the
// portable engine/UI in lib/pg is still being written. Until then this only
// proves the platform layer boots and says so on screen and over serial.
// Hardware bring-up and measurements: env:smoke (src/smoke, docs/BRINGUP.md).
#include "../platform/display.h"
#include "../platform/system.h"

void setup() {
  const plat::sys::BootInfo boot = plat::sys::begin(115200);
  plat::display::begin();
  plat::display::directText(8, 50, "Pocket Groove: app not built yet", 0xFFFF);
  plat::display::directText(8, 70, "flash env:smoke for the hardware test", 0x07E0);
  plat::sys::serialWrite("Pocket Groove: app not built yet (board: ");
  plat::sys::serialWrite(boot.boardName);
  plat::sys::serialWrite(")\n");
}

void loop() { plat::sys::delayMs(100); }
