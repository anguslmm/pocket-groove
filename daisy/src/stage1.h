// The breadboard prototype's peripherals (stage 1, docs/DAISY.md): the ST7789
// display, the TCA8418 key matrix and G0, the BMI270, the microSD card, the
// microphone. begin(false) (stage 0) touches none of them and every query answers
// "absent". The drivers' logic is portable (daisy/core: st7789, tca8418, bmi270),
// host-tested against simulated buses; this file binds them to libDaisy's SPI, I2C,
// GPIO and SDMMC.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/platform.h"
#include "pg/sampler/recorder.h"

namespace pgdev {
namespace stage1 {

void begin(bool enabled);
void service(uint32_t nowMs);   // main loop, every pass

bool pollKey(pg::KeyEvent* e);
bool displayPresent();
void present(const pg::Framebuffer& fb, int y0, int y1);
bool imuPresent();
void tiltPress(int responseX, int responseY);
void tiltRelease();
bool tiltRead(float* x, float* y);
pg::sampler::MicInput* mic();
pg::FileSystem* sdCard();

// The audio interrupt: the codec's input block (interleaved stereo float).
void audioInput(const float* in, size_t size);

using LineFn = void (*)(void* user, const char* line);
void reportLines(LineFn fn, void* user);

}  // namespace stage1
}  // namespace pgdev
