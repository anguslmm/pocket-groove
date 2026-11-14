// The breadboard prototype's peripherals (stage 1, docs/DAISY.md): the Adafruit 2090
// display (ILI9341) and its FT6206 touch, G0, the BMI270, the microSD card. The four
// force-sensor pads and the line input are read by main.cpp (the ADC, the audio
// callback). begin(false) (stage 0) touches none of them and every query answers
// "absent". The drivers' logic is portable (daisy/core: panel, touch, bmi270,
// i2c_bus), host-tested against simulated buses; this file binds them to libDaisy's
// SPI, I2C, GPIO and SDMMC.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../app/app.h"
#include "pg/platform.h"

namespace pgdev {
namespace stage1 {

void begin(bool enabled);
void service(uint32_t nowMs);   // main loop, every pass

bool pollKey(pg::KeyEvent* e);
bool pollPointer(pgd::Hal::Pointer* p);
bool touchInfo(pgd::Hal::TouchInfo* t);
bool displayPresent();
void present(const pg::Framebuffer& fb, int y0, int y1);
void applySettings(const pgd::DaisySettings& s);
bool imuPresent();
void tiltPress(int responseX, int responseY);
void tiltRelease();
bool tiltRead(float* x, float* y);
pg::FileSystem* sdCard();

using LineFn = void (*)(void* user, const char* line);
void reportLines(LineFn fn, void* user);

}  // namespace stage1
}  // namespace pgdev
