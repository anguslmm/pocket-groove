// Pocket Groove hardware spike -- shared declarations of the two pages.
//
// Threads:  UI     = Arduino loop() on core 1, priority 1: keys, display, I2C to the codec, reports
//           audio  = hws::duplex task, core 0, priority 20: page render + mic input
//           imu    = hws::imu task, core 1, priority 9: BMI270 reads + tilt maths
//           keys   = plat::keys task, core 1, priority 10
//           writer = recording / bench-prep job, core 1, priority 5 (flash writes)
#pragma once
#include <cstdint>

#include "../smoke/gfx.h"
#include "report.h"

namespace hws {
namespace app {

extern smoke::Canvas g_cv;
extern uint32_t g_sampleRate;

void sp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));     // serial
void msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));    // LCD message line
const char* currentMsg();
void reportSink(const char* line, void* user);                           // serial line sink
void fillMemory(uint32_t* heapFree, uint32_t* heapMin, uint32_t* largest, uint32_t* staticDram);

// Key ids (pg::platform.h: id = row * 14 + col).
constexpr uint8_t key(int row, int col) { return static_cast<uint8_t>(row * 14 + col); }

}  // namespace app

namespace tilt {
void begin();
void enter();
void leave();
void render(int16_t* outStereo, int frames);    // audio task
void key(uint8_t k, bool down);                 // UI
void draw();                                    // UI
void tick();                                    // UI, every loop
void noteKeyLatency(uint32_t us);               // UI: INT -> read of a matrix key event
void noteKeyEvent(bool down);
void report();
}  // namespace tilt

namespace sampler {
void begin();
void enter();
void leave();
void input(const int16_t* inStereo, int frames);   // audio task, every block, both pages
void render(int16_t* outStereo, int frames);       // audio task
void key(uint8_t k, bool down);
void draw();
void tick();
void report();
}  // namespace sampler

}  // namespace hws
