// The firmware's microsecond clock (daisy/core/us_clock.h explains why libDaisy's
// System::GetUs() cannot be used: it wraps every 17.9 s). Every microsecond reading
// in daisy/src goes through micros(); millis() is libDaisy's System::GetNow() (the
// HAL's 1 kHz SysTick count, a plain 32-bit counter: it wraps after 49.7 days, and
// unsigned differences are right across that wrap).
#pragma once
#include <cstdint>

namespace pgdev {
namespace clock {

void begin();          // after DaisySeed::Init (TIM2 running)
uint32_t micros();     // main loop and interrupts
uint32_t ticksPerUs(); // TIM2's ticks per microsecond (240 at 480 MHz)

}  // namespace clock
}  // namespace pgdev
