// See bench_info.h.
#include "bench_info.h"

#include <cstdio>

#include "daisy_seed.h"
#include "memory.h"
#include "pg/instrument/instrument.h"

#ifndef PG_VARIANT
#define PG_VARIANT "base"
#endif
#ifndef PG_BUILD_FLAGS
#define PG_BUILD_FLAGS "?"
#endif

// The functions and tables the "code" and "data" lines locate (weak: a symbol the
// compiler inlined away reads 0). Mangled names, as the ELF has them.
extern "C" {
extern char _ZN2pg6Engine12processFloatEPfi[] __attribute__((weak));
extern char _ZN2pg13engine_detail6Reverb3runILb0EEEvPKfPfS5_i[] __attribute__((weak));
extern char _ZN2pg4lead9LeadVoice6renderEPfS2_i[] __attribute__((weak));
extern char _ZN2pg4bass9BassVoice6renderEPfS2_i[] __attribute__((weak));
extern char _ZN2pg5drums8HatVoice6renderEPfS2_i[] __attribute__((weak));
extern char _ZN2pg4bass5gSineE[] __attribute__((weak));
extern char _pg_engine_state[], _pg_engine_state_end[];   // daisy/pg_daisy.lds
}

namespace pgdev {

const char* regionOf(uint32_t a) {
  if (a < 0x00010000u) return "ITCM";
  if (a >= 0x20000000u && a < 0x20020000u) return "DTCM";
  if (a >= 0x24000000u && a < 0x24080000u) return "AXI SRAM";
  if (a >= 0x30000000u && a < 0x30008000u) return "D2 SRAM, DMA, not cached";
  if (a >= 0x30008000u && a < 0x30048000u) return "D2 SRAM";
  if (a >= 0x38000000u && a < 0x38010000u) return "D3 SRAM";
  if (a >= 0x90000000u && a < 0x90800000u) return "QSPI";
  if (a >= 0xC0000000u && a < 0xC4000000u) return "SDRAM";
  return "?";
}


void benchInfo(pgd::LineFn fn, void* user, const void* instrument) {
  char l[400];
  std::snprintf(l, sizeof(l), "build: variant %s (%s); gcc %s; libDaisy %s -Os", PG_VARIANT, PG_BUILD_FLAGS,
                __VERSION__, PG_LIBDAISY_COMMIT);
  fn(user, l);
  const uint32_t ccr = SCB->CCR;
  std::snprintf(l, sizeof(l),
                "caches: SCB->CCR 0x%08lx: I-cache %s, D-cache %s, branch prediction %s; ITCMCR 0x%08lx, DTCMCR 0x%08lx; "
                "CPU %lu MHz, HCLK (AXI) %lu MHz",
                static_cast<unsigned long>(ccr), (ccr & SCB_CCR_IC_Msk) ? "on" : "OFF",
                (ccr & SCB_CCR_DC_Msk) ? "on" : "OFF", (ccr & SCB_CCR_BP_Msk) ? "on" : "off",
                static_cast<unsigned long>(SCB->ITCMCR), static_cast<unsigned long>(SCB->DTCMCR),
                static_cast<unsigned long>(HAL_RCC_GetSysClockFreq() / 1000000u),
                static_cast<unsigned long>(HAL_RCC_GetHCLKFreq() / 1000000u));
  fn(user, l);
  struct W {
    const char* what;
    const void* at;
  };
  const W code[] = {{"Engine::processFloat", _ZN2pg6Engine12processFloatEPfi},
                    {"Reverb::run", _ZN2pg13engine_detail6Reverb3runILb0EEEvPKfPfS5_i},
                    {"LeadVoice::render", _ZN2pg4lead9LeadVoice6renderEPfS2_i},
                    {"BassVoice::render", _ZN2pg4bass9BassVoice6renderEPfS2_i},
                    {"HatVoice::render", _ZN2pg5drums8HatVoice6renderEPfS2_i}};
  int n = std::snprintf(l, sizeof(l), "code:");
  for (const W& w : code) {
    const uint32_t a = reinterpret_cast<uint32_t>(w.at);
    n += std::snprintf(l + n, sizeof(l) - static_cast<size_t>(n), " %s 0x%08lx (%s)%s", w.what,
                       static_cast<unsigned long>(a), a ? regionOf(a) : "inlined", &w == &code[4] ? "" : ",");
  }
  std::snprintf(l + n, sizeof(l) - static_cast<size_t>(n), "; ITCM holds %lu B", static_cast<unsigned long>(pgdev::memory::itcmBytes()));
  fn(user, l);
  const uint32_t eng = reinterpret_cast<uint32_t>(_pg_engine_state);
  const uint32_t ins = reinterpret_cast<uint32_t>(instrument);
  const uint32_t tab = reinterpret_cast<uint32_t>(_ZN2pg4bass5gSineE);
  const uint32_t sp = __get_MSP();
  std::snprintf(l, sizeof(l),
                "data: engine state 0x%08lx %lu B (%s: delay, reverb, sequencer, mixer), instrument 0x%08lx %lu B (%s: "
                "voices and the Project), voice tables 0x%08lx (%s: the bass's sine, with the lead's wavetables), "
                "stack near 0x%08lx (%s), DMA buffers (D2 SRAM, not cached)",
                static_cast<unsigned long>(eng), static_cast<unsigned long>(_pg_engine_state_end - _pg_engine_state),
                regionOf(eng), static_cast<unsigned long>(ins), static_cast<unsigned long>(sizeof(pg::Instrument)),
                regionOf(ins), static_cast<unsigned long>(tab), tab ? regionOf(tab) : "?",
                static_cast<unsigned long>(sp), regionOf(sp));
  fn(user, l);
}

}  // namespace pgdev
