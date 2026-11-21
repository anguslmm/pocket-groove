// See clock.h.
#include "clock.h"

#include "../core/us_clock.h"
#include "daisy_seed.h"

namespace pgdev {
namespace clock {

namespace {
pgd::UsClock g_clock;
uint32_t g_perUs = 1;
bool g_started = false;
}  // namespace

void begin() {
  // TIM2's clock: libDaisy's TimerHandle::GetFreq() is 2 x PCLK1 / (PSC + 1); System
  // starts it with PSC 0 (src/sys/system.cpp, src/per/tim.cpp).
  const uint32_t hz = HAL_RCC_GetPCLK1Freq() * 2u / (TIM2->PSC + 1u);
  g_perUs = hz / 1000000u ? hz / 1000000u : 1u;
  g_clock.start(daisy::System::GetTick(), g_perUs, daisy::System::GetTick() / g_perUs);
  g_started = true;
}

uint32_t ticksPerUs() { return g_perUs; }

uint32_t micros() {
  if (!g_started) return daisy::System::GetUs();   // before begin(): boot only, short spans
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  const uint32_t us = g_clock.update(daisy::System::GetTick());
  __set_PRIMASK(primask);
  return us;
}

}  // namespace clock
}  // namespace pgdev
