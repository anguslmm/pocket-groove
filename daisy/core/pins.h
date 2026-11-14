// The pin map of the breadboard prototype (docs/DAISY.md "Pin map"). Seed pins are
// libDaisy's D0..D30 (header pins 1-15 and 22-37; the Seed3 has these 31, the
// Seed2 DFM the same plus D31 / D32, which this map does not use, so one map fits
// both). Port and pin are the STM32's, from libDaisy's daisy_seed.h; header pins
// and alternate functions from libDaisy's doc/Daisy_Seed_Rev4_Pinout.csv (the Seed3
// is pin-compatible, per daisy_seed.h). The firmware's drivers take their pins from
// here (daisy/src), and host/daisy/test_daisy_core.cpp checks the table: no pin
// used twice, every analog function on an ADC pin, the peripherals' pins where the
// chip routes them.
#pragma once
#include <cstdint>

namespace pgd {
namespace pins {

enum Stage : uint8_t { kStage0 = 0, kStage1 = 1 };

struct PinUse {
  const char* function;
  uint8_t seed;        // D number (libDaisy seed::D<n>)
  uint8_t header;      // the Seed's header pin (1..40)
  char port;           // 'A'..'G'
  uint8_t pin;
  bool adc;            // needs an ADC input
  Stage stage;         // when it is wired: 0 = on the Labor / first breadboard, 1 = the full prototype
  const char* note;
};

// D-number -> (port, pin), as daisy_seed.h's seed::D0..D30.
struct SeedPin {
  char port;
  uint8_t pin;
  bool adc;
  uint8_t header;
};
constexpr SeedPin kSeed[31] = {
    {'B', 12, false, 1}, {'C', 11, false, 2}, {'C', 10, false, 3}, {'C', 9, false, 4},  {'C', 8, false, 5},
    {'D', 2, false, 6},  {'C', 12, false, 7}, {'G', 10, false, 8}, {'G', 11, false, 9}, {'B', 4, false, 10},
    {'B', 5, false, 11}, {'B', 8, false, 12}, {'B', 9, false, 13}, {'B', 6, false, 14}, {'B', 7, false, 15},
    {'C', 0, true, 22},  {'A', 3, true, 23},  {'B', 1, true, 24},  {'A', 7, true, 25},  {'A', 6, true, 26},
    {'C', 1, true, 27},  {'C', 4, true, 28},  {'A', 5, true, 29},  {'A', 4, true, 30},  {'A', 1, true, 31},
    {'A', 0, true, 32},  {'D', 11, false, 33}, {'G', 9, false, 34}, {'A', 2, true, 35}, {'B', 14, false, 36},
    {'B', 15, false, 37},
};

#define PGD_PIN(fn, d, adc, st, note) \
  PinUse { fn, d, kSeed[d].header, kSeed[d].port, kSeed[d].pin, adc, st, note }

constexpr PinUse kMap[] = {
    // stage 0: four pots (the first breadboard / the Labor)
    PGD_PIN("POT1 (macro 1)", 15, true, kStage0, "ADC INP10; wiper, ends to 3V3A (pin 21) and AGND (pin 20)"),
    PGD_PIN("POT2 (macro 2)", 16, true, kStage0, "ADC INP15"),
    PGD_PIN("POT3 (macro 3)", 17, true, kStage0, "ADC INP5"),
    PGD_PIN("POT4 (macro 4)", 18, true, kStage0, "ADC INP7"),
    // stage 1
    PGD_PIN("BATTERY", 19, true, kStage1, "ADC INP3, LiPo through a 100k/100k divider (max 2.1 V at 4.2 V)"),
    // microSD (Adafruit 4682) on SDMMC1, 4-bit: libDaisy's SdmmcHandler pins (the bootloader's too)
    PGD_PIN("SD D3", 1, false, kStage1, "SDMMC1_D3"),
    PGD_PIN("SD D2", 2, false, kStage1, "SDMMC1_D2"),
    PGD_PIN("SD D1", 3, false, kStage1, "SDMMC1_D1"),
    PGD_PIN("SD D0", 4, false, kStage1, "SDMMC1_D0"),
    PGD_PIN("SD CMD", 5, false, kStage1, "SDMMC1_CMD"),
    PGD_PIN("SD CK", 6, false, kStage1, "SDMMC1_CK"),
    // the display: Adafruit 2090 (2.8" 320x240 ILI9341 over SPI; write only, software reset).
    // Also fits the other panel configurations (panel.h: ST7789 240x135, HX8357D 480x320).
    PGD_PIN("LCD CS", 7, false, kStage1, "2090 CS: the SPI1_NSS pin driven as a GPIO (held low: the only device)"),
    PGD_PIN("LCD SCK", 8, false, kStage1, "2090 CLK: SPI1_SCK"),
    PGD_PIN("LCD DC", 9, false, kStage1, "2090 D/C: the SPI1_MISO pin used as a GPIO (the panel sends nothing)"),
    PGD_PIN("LCD MOSI", 10, false, kStage1, "2090 MOSI: SPI1_MOSI"),
    PGD_PIN("LCD BL", 27, false, kStage1, "2090 Lite: GPIO, high = on (DC: no PWM edges near the audio)"),
    // I2C1: the BMI270 (SparkFun Qwiic board on a lead, 0x68) and the 2090's FT6206 touch (0x38)
    PGD_PIN("I2C SCL", 11, false, kStage1, "I2C1_SCL: BMI270 + FT6206 (pull-ups on the boards, none on the Seed)"),
    PGD_PIN("I2C SDA", 12, false, kStage1, "I2C1_SDA"),
    PGD_PIN("TOUCH INT", 24, false, kStage1, "2090 IRQ (FT6206, low while touched): GPIO input, pull-up; reported only"),
    PGD_PIN("G0 BUTTON", 25, false, kStage1, "GPIO input, pull-up, to ground when pressed"),
    // four force sensors (Adafruit 1075), each a divider: 3V3A - FSR - pin - 10k - AGND
    PGD_PIN("PAD1 (kick)", 20, true, kStage1, "A5, ADC1 INP11; FSR to 3V3A (pin 21), 10k to AGND (pin 20)"),
    PGD_PIN("PAD2 (snare)", 21, true, kStage1, "A6, ADC1 INP4"),
    PGD_PIN("PAD3 (clap)", 22, true, kStage1, "A7, ADC1 INP19 (also the DAC's OUT2: the DAC is not used)"),
    PGD_PIN("PAD4 (closed hat)", 23, true, kStage1, "A8, ADC1 INP18 (also the DAC's OUT1: the DAC is not used)"),
};
// Free for what is undecided (more pads through a multiplexer, function buttons):
// D0, D13, D14 (I2C4), D26, D28 (A11: analog); D29 / D30 are the second USB port.
#undef PGD_PIN
constexpr int kMapCount = int(sizeof(kMap) / sizeof(kMap[0]));

// The pots in order (seed D numbers), the battery's, the force sensors' (pads 1-4),
// the touch controller's interrupt.
constexpr uint8_t kPotSeed[4] = {15, 16, 17, 18};
constexpr uint8_t kBatterySeed = 19;
constexpr uint8_t kPadSeed[4] = {20, 21, 22, 23};
constexpr uint8_t kTouchIrqSeed = 24;
// The STM32 pins libDaisy's AdcHandle can read (src/per/adc.cpp's pin table: ADC1
// INP3..INP19 without INP6), as port / pin. The test checks every ADC function on it.
struct PortPin {
  char port;
  uint8_t pin;
};
constexpr PortPin kAdc1Pins[] = {{'A', 6}, {'C', 4}, {'B', 1}, {'A', 7}, {'C', 5}, {'B', 0}, {'C', 0}, {'C', 1},
                                 {'C', 2}, {'C', 3}, {'A', 2}, {'A', 3}, {'A', 0}, {'A', 1}, {'A', 4}, {'A', 5}};

// Not GPIO: the codec's analog pins and the supplies (header pin numbers).
struct Analog {
  uint8_t header;
  const char* name;
  const char* use;
};
constexpr Analog kAnalog[] = {
    {16, "AUDIO IN L", "line in L: the first recording path (line level only, never Eurorack levels)"},
    {17, "AUDIO IN R", "line in R (a mono source: into L, R tied to AGND: recorded as L alone)"},
    {18, "AUDIO OUT L", "line out L: to the headphone amp (TPA6132) or the Labor's output amplifier input"},
    {19, "AUDIO OUT R", "line out R"},
    {20, "AGND", "analog ground: the audio grounds and the pots' low ends"},
    {21, "3V3A", "analog 3.3 V: the pots' and the force sensors' high ends (the ADC's reference)"},
    {38, "3V3D", "digital 3.3 V out: display (2090 Vin), BMI270, SD card (how much current it can spare: Seed datasheet, unverified here)"},
    {39, "VIN", "supply input (its range is in Electrosmith's datasheet, not in libDaisy: NOT USED, power over USB)"},
    {40, "DGND", "digital ground: join with every other board's ground"},
};

}  // namespace pins
}  // namespace pgd
