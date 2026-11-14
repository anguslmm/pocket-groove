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
    // stage 0: four pots and the battery divider (the first breadboard / the Labor)
    PGD_PIN("POT1 (macro 1)", 15, true, kStage0, "ADC1 INP10; wiper, ends to 3V3A (pin 21) and AGND (pin 20)"),
    PGD_PIN("POT2 (macro 2)", 16, true, kStage0, "ADC INP15"),
    PGD_PIN("POT3 (macro 3)", 17, true, kStage0, "ADC INP5"),
    PGD_PIN("POT4 (macro 4)", 18, true, kStage0, "ADC INP7"),
    PGD_PIN("BATTERY", 19, true, kStage1, "ADC INP3, LiPo through a 100k/100k divider (max 2.1 V at 4.2 V)"),
    // stage 1: microSD on SDMMC1, 4-bit (libDaisy's SdmmcHandler pins; the bootloader's too)
    PGD_PIN("SD D3", 1, false, kStage1, "SDMMC1_D3, 47k pull-up"),
    PGD_PIN("SD D2", 2, false, kStage1, "SDMMC1_D2, 47k pull-up"),
    PGD_PIN("SD D1", 3, false, kStage1, "SDMMC1_D1, 47k pull-up"),
    PGD_PIN("SD D0", 4, false, kStage1, "SDMMC1_D0, 47k pull-up"),
    PGD_PIN("SD CMD", 5, false, kStage1, "SDMMC1_CMD, 47k pull-up"),
    PGD_PIN("SD CK", 6, false, kStage1, "SDMMC1_CK"),
    // stage 1: ST7789 240x135 on SPI1 (write only)
    PGD_PIN("LCD CS", 7, false, kStage1, "SPI1_NSS (driven as a GPIO)"),
    PGD_PIN("LCD SCK", 8, false, kStage1, "SPI1_SCK"),
    PGD_PIN("LCD MOSI", 10, false, kStage1, "SPI1_MOSI"),
    PGD_PIN("LCD DC", 9, false, kStage1, "SPI1_MISO pin used as a GPIO (the panel sends nothing)"),
    PGD_PIN("LCD RST", 26, false, kStage1, "GPIO"),
    PGD_PIN("LCD BL", 27, false, kStage1, "GPIO, high = on (DC: no PWM edges near the audio)"),
    // stage 1: TCA8418 keys on I2C1, BMI270 on I2C4 (a second bus)
    PGD_PIN("KEYS SCL", 11, false, kStage1, "I2C1_SCL, 2.2k pull-up to 3V3"),
    PGD_PIN("KEYS SDA", 12, false, kStage1, "I2C1_SDA, 2.2k pull-up to 3V3"),
    PGD_PIN("KEYS INT", 24, false, kStage1, "GPIO input, pull-up (TCA8418 INT is open drain)"),
    PGD_PIN("IMU SCL", 13, false, kStage1, "I2C4_SCL, 2.2k pull-up"),
    PGD_PIN("IMU SDA", 14, false, kStage1, "I2C4_SDA, 2.2k pull-up"),
    PGD_PIN("G0 BUTTON", 25, false, kStage1, "GPIO input, pull-up, to ground when pressed"),
};
#undef PGD_PIN
constexpr int kMapCount = int(sizeof(kMap) / sizeof(kMap[0]));

// The pots in order (seed D numbers) and the battery's.
constexpr uint8_t kPotSeed[4] = {15, 16, 17, 18};
constexpr uint8_t kBatterySeed = 19;

// Not GPIO: the codec's analog pins and the supplies (header pin numbers).
struct Analog {
  uint8_t header;
  const char* name;
  const char* use;
};
constexpr Analog kAnalog[] = {
    {16, "AUDIO IN L", "stage 1: the MAX9814 output through 10 uF (the recorder's input); stage 0: nothing"},
    {17, "AUDIO IN R", "nothing"},
    {18, "AUDIO OUT L", "line out L: to the headphone amp (TPA6132) or the Labor's output amplifier input"},
    {19, "AUDIO OUT R", "line out R"},
    {20, "AGND", "analog ground: the audio grounds and the pots' low ends"},
    {21, "3V3A", "analog 3.3 V: the pots' high ends (reference for the ADC)"},
    {38, "3V3D", "digital 3.3 V out (100 mA budget): display, TCA8418, BMI270, SD card, MAX9814"},
    {39, "VIN", "supply input (its range is in Electrosmith's datasheet, not in libDaisy: NOT USED, power over USB)"},
    {40, "DGND", "digital ground: join with every other board's ground"},
};

}  // namespace pins
}  // namespace pgd
