// The bench's lines about what determines speed (daisy/app/bench.h, Hal::benchInfo):
// the build (variant, flags, compiler), the caches (SCB->CCR), and which memory the
// audio path's code and data sit in. Main loop only; built -Os.
#pragma once
#include <cstdint>

#include "../app/app.h"

namespace pgdev {

// The memory region an address is in ("AXI SRAM", "DTCM", "ITCM", ...).
const char* regionOf(uint32_t address);
void benchInfo(pgd::LineFn fn, void* user, const void* instrument);

}  // namespace pgdev
