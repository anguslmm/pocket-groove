// See memory.h.
#include "memory.h"

#include <cstdio>
#include <cstring>

#include "../core/layout.h"
#include "daisy_seed.h"

extern "C" {
// daisy/pg_daisy.lds
extern uint8_t _pg_image_start[], _pg_image_end[];
extern uint8_t _sqspi_rom[], _eqspi_rom[], _siqspi_rom[];
extern uint8_t _sdata[], _edata[], _sbss[], _ebss[], _sdtcmram_bss[], _edtcmram_bss[], _estack[];
extern uint8_t _saxi_bss[], _eaxi_bss[], _sd2_bss[], _ed2_bss[], _pg_d2_end[];
extern uint8_t _ssram1_bss[], _esram1_bss[], _ssdram_bss[], _esdram_bss[];
extern uint8_t _sd3_noinit[], _ed3_noinit[];
extern void* g_pfnVectors[];
}

namespace pgdev {
namespace memory {

namespace {

constexpr uint32_t kPaint = 0xA5C3E1F0u;
constexpr uint32_t kFaultMagic = 0x46415554u;   // "FAUT"
constexpr uint32_t kRomMagic = 0x50474D52u;     // "PGMR"

// The ROM part's marker: one copy in the image, one in .qspi_rom. They match only
// if the image and the ROM part come from the same build.
struct RomMarker {
  uint32_t magic;
  char build[28];
};
#define PG_BUILD_ID __DATE__ " " __TIME__
__attribute__((section(".qspi_rom.marker"), used)) const RomMarker kRomMarkerRom = {kRomMagic, PG_BUILD_ID};
const RomMarker kRomMarkerImage = {kRomMagic, PG_BUILD_ID};

struct FaultRecord {
  uint32_t magic;
  uint32_t pc, lr, cfsr, hfsr, bfar, mmfar, uptimeMs, count, check;
};
__attribute__((section(".d3_noinit"))) FaultRecord g_fault;

bool g_romOk = false;
uint32_t g_romUs = 0;
uint32_t (*g_ms)() = nullptr;

uint32_t faultCheck(const FaultRecord& f) {
  return f.pc ^ f.lr ^ f.cfsr ^ f.hfsr ^ f.bfar ^ f.mmfar ^ f.uptimeMs ^ f.count ^ 0x5A5A5A5Au;
}

uint32_t* stackLow() { return reinterpret_cast<uint32_t*>(_edtcmram_bss); }

// Before any constructor (and before main): __libc_init_array runs .preinit_array
// first. The startup code has copied .data and zeroed .bss (both DTCM) by now.
void preinit() {
  // The FPU: full access to CP10 / CP11. libDaisy's SystemInit does this, but the
  // startup code skips SystemInit in a BOOT_APP build, so it is the bootloader's
  // doing (or a fault at the first float instruction): set it again, harmless.
  SCB->CPACR |= (3u << 20) | (3u << 22);
  __DSB();
  __ISB();
  // Denormals: flush-to-zero (FZ, bit 24) and default NaN (DN, bit 25) for the main
  // loop (FPSCR) and for every interrupt handler (FPDSCR: the core loads an
  // exception's FPSCR from it). libDaisy does not touch either. Decaying FX tails
  // otherwise produce subnormal floats; the M7 handles them in hardware, but FZ makes
  // the arithmetic the same in every context. The audio callback checks it took.
  FPU->FPDSCR |= (1u << 24) | (1u << 25);
  __set_FPSCR(__get_FPSCR() | (1u << 24) | (1u << 25));
  // The vector table is the image's (the bootloader should have set this already).
  SCB->VTOR = reinterpret_cast<uint32_t>(g_pfnVectors);
  // D2 SRAM1-3: their clocks (the bootloader's SystemInit enables them, libDaisy's
  // DMA buffers rely on it; done again here, harmless).
  RCC->AHB2ENR |= RCC_AHB2ENR_D2SRAM1EN | RCC_AHB2ENR_D2SRAM2EN | RCC_AHB2ENR_D2SRAM3EN;
  (void)RCC->AHB2ENR;
  __DSB();
  std::memset(_saxi_bss, 0, static_cast<size_t>(_eaxi_bss - _saxi_bss));
  std::memset(_sd2_bss, 0, static_cast<size_t>(_ed2_bss - _sd2_bss));
  // Paint the free stack (DTCM above the statics, below the frame we run in).
  uint32_t* p = stackLow();
  uint32_t* top = reinterpret_cast<uint32_t*>(__get_MSP()) - 32;
  while (p < top) *p++ = kPaint;
}
__attribute__((section(".preinit_array"), used)) void (*const g_preinit)() = preinit;

// The fault handlers: record, then reset.
extern "C" void pgFaultRecord(uint32_t* frame) {
  FaultRecord f;
  f.magic = kFaultMagic;
  f.pc = frame[6];
  f.lr = frame[5];
  f.cfsr = SCB->CFSR;
  f.hfsr = SCB->HFSR;
  f.bfar = SCB->BFAR;
  f.mmfar = SCB->MMFAR;
  f.uptimeMs = g_ms ? g_ms() : 0;
  f.count = (g_fault.magic == kFaultMagic && g_fault.check == faultCheck(g_fault)) ? g_fault.count + 1 : 1;
  f.check = faultCheck(f);
  g_fault = f;
  __DSB();
  NVIC_SystemReset();
}

extern "C" __attribute__((naked)) void pgFaultEntry() {
  __asm volatile(
      "tst lr, #4      \n"
      "ite eq          \n"
      "mrseq r0, msp   \n"
      "mrsne r0, psp   \n"
      "b pgFaultRecord \n");
}

}  // namespace

void installFaultHandlers() {
  // The vector table is in RAM (the image): point the four fault vectors at ours
  // (libDaisy's HardFault_Handler stops at a breakpoint, which without a debugger
  // locks the core up silently).
  void** v = reinterpret_cast<void**>(SCB->VTOR);
  for (int i = 3; i <= 6; ++i) v[i] = reinterpret_cast<void*>(pgFaultEntry);
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
  __DSB();
  __ISB();
}

void setUptimeHook(uint32_t (*ms)()) { g_ms = ms; }

Fault lastFault() {
  Fault f{};
  if (g_fault.magic == kFaultMagic && g_fault.check == faultCheck(g_fault)) {
    f = Fault{true, g_fault.pc, g_fault.lr, g_fault.cfsr, g_fault.hfsr, g_fault.bfar, g_fault.mmfar, g_fault.uptimeMs,
              g_fault.count};
    // Reported once: keep the count (consecutive faults), forget the details.
    g_fault.magic = 0;
  }
  return f;
}

bool copyRom() {
  const size_t n = static_cast<size_t>(_eqspi_rom - _sqspi_rom);
  const uint32_t t0 = daisy::System::GetUs();
  std::memcpy(_sqspi_rom, _siqspi_rom, n);
  SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(_sqspi_rom), static_cast<int32_t>(n));
  g_romUs = daisy::System::GetUs() - t0;
  // The SDRAM copy now. Read through volatile: the compiler knows the initializer and
  // would otherwise fold the comparison to true.
  const volatile uint8_t* rom = reinterpret_cast<const volatile uint8_t*>(&kRomMarkerRom);
  const uint8_t* img = reinterpret_cast<const uint8_t*>(&kRomMarkerImage);
  g_romOk = true;
  for (size_t i = 0; i < sizeof(RomMarker); ++i)
    if (rom[i] != img[i]) g_romOk = false;
  if (!g_romOk) std::memset(_sqspi_rom, 0, n);   // silence, not noise
  return g_romOk;
}
bool romOk() { return g_romOk; }
uint32_t romBytes() { return static_cast<uint32_t>(_eqspi_rom - _sqspi_rom); }
uint32_t romCopyUs() { return g_romUs; }

uint32_t stackSize() { return static_cast<uint32_t>(_estack - _edtcmram_bss); }
uint32_t stackUsedMax() {
  const uint32_t* p = stackLow();
  const uint32_t* end = reinterpret_cast<const uint32_t*>(_estack);
  while (p < end && *p == kPaint) ++p;
  return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(end) - reinterpret_cast<const uint8_t*>(p));
}

void reportLines(LineFn fn, void* user) {
  char l[200];
  auto kb = [](uint32_t b) { return static_cast<unsigned long>((b + 512u) / 1024u); };
  const uint32_t image = static_cast<uint32_t>(_pg_image_end - _pg_image_start);
  std::snprintf(l, sizeof(l), "[mem] image (AXI SRAM 0x24000000, copied by the bootloader): %lu B of %lu (%lu KB free)",
                static_cast<unsigned long>(image), static_cast<unsigned long>(pgd::layout::kAppMax),
                kb(pgd::layout::kAppMax - image));
  fn(user, l);
  const uint32_t axi = static_cast<uint32_t>(_eaxi_bss - _saxi_bss);
  std::snprintf(l, sizeof(l), "[mem] AXI SRAM after the image: %lu KB of statics, %lu KB free of 512",
                kb(axi), kb(0x24080000u - reinterpret_cast<uint32_t>(_eaxi_bss)));
  fn(user, l);
  const uint32_t dtcm = static_cast<uint32_t>(_edtcmram_bss - _sdata);
  std::snprintf(l, sizeof(l),
                "[mem] DTCM 128 KB: .data %lu B, .bss %lu B (the engine's state), other %lu B; stack %lu B, used at most %lu B",
                static_cast<unsigned long>(_edata - _sdata), static_cast<unsigned long>(_ebss - _sbss),
                static_cast<unsigned long>(_edtcmram_bss - _sdtcmram_bss), static_cast<unsigned long>(stackSize()),
                static_cast<unsigned long>(stackUsedMax()));
  (void)dtcm;
  fn(user, l);
  std::snprintf(l, sizeof(l), "[mem] D2 SRAM: DMA buffers %lu of 32 KB (not cached); statics %lu KB, %lu KB free of 256",
                kb(static_cast<uint32_t>(_esram1_bss - _ssram1_bss)), kb(static_cast<uint32_t>(_ed2_bss - _sd2_bss)),
                kb(0x30048000u - reinterpret_cast<uint32_t>(_pg_d2_end)));
  fn(user, l);
  std::snprintf(l, sizeof(l), "[mem] SDRAM 64 MB: ROM copy %lu KB (%s, copied in %lu us), buffers %lu KB, %lu KB free",
                kb(romBytes()), g_romOk ? "ok" : "MISSING", static_cast<unsigned long>(g_romUs),
                kb(static_cast<uint32_t>(_esdram_bss - _ssdram_bss)),
                kb(0xC4000000u - reinterpret_cast<uint32_t>(_esdram_bss)));
  fn(user, l);
  std::snprintf(l, sizeof(l),
                "[mem] QSPI 8 MB: image 0x%08lx (%lu KB), ROM 0x%08lx (%lu KB), /int 0x%08lx (2 x %lu KB), pool 0x%08lx (%lu KB)",
                static_cast<unsigned long>(pgd::layout::kAppAddr), kb(image),
                static_cast<unsigned long>(pgd::layout::kRomAddr), kb(romBytes()),
                static_cast<unsigned long>(pgd::layout::kFsAddr), kb(pgd::layout::kFsBankBytes),
                static_cast<unsigned long>(pgd::layout::kPoolAddr), kb(pgd::layout::kPoolBytes));
  fn(user, l);
}

}  // namespace memory
}  // namespace pgdev
