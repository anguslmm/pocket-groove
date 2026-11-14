// Memory on the Daisy Seed (daisy/pg_daisy.lds, docs/DAISY.md "Memory map"): what
// happens before main() (the .preinit_array hook: vector table, D2 SRAM clocks,
// zeroing the extra .bss regions, painting the stack), the ROM copy from QSPI to
// SDRAM after DaisySeed::Init, the fault record kept over a reset, and the
// report's memory lines (from the linker's symbols).
#pragma once
#include <cstdint>

namespace pgdev {
namespace memory {

// After DaisySeed::Init (SDRAM and QSPI up): .qspi_rom from QSPI to SDRAM, then the
// marker check. false: the ROM part is missing or from another build (only the
// first 480 KB were flashed): the region is zeroed (silent built-in sounds).
bool copyRom();
bool romOk();
uint32_t romBytes();
uint32_t romCopyUs();

// Hard fault / bus fault / usage fault: recorded in a RAM area no one clears, then a
// reset. lastFault() says what the previous run died of (once per fault).
void installFaultHandlers();
struct Fault {
  bool valid;
  uint32_t pc, lr, cfsr, hfsr, bfar, mmfar, uptimeMs, count;
};
Fault lastFault();
void setUptimeHook(uint32_t (*ms)());

// Stack high-water (bytes used at most since boot) and its size.
uint32_t stackUsedMax();
uint32_t stackSize();

// One line per region for the report.
using LineFn = void (*)(void* user, const char* line);
void reportLines(LineFn fn, void* user);

}  // namespace memory
}  // namespace pgdev
