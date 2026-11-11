# M5Stack Cardputer-Adv — hardware notes for firmware

Researched 2026-11-11. Nothing here was tested on a device. Every fact carries a source and a confidence tag.

## 0. How to read this file

| Tag | Meaning |
|---|---|
| **[SRC]** | Read directly in source code at a pinned tag or commit (link given). Primary. |
| **[BUILD]** | Compiled in this session (pioarduino 55.03.312-1, Arduino core 3.3.12). Proves API names and build config only, not hardware behaviour. |
| **[3P]** | Stated by a third-party project. "hw" means its author says it ran on a Cardputer-Adv. |
| **[SNIPPET]** | Content of a docs.m5stack.com page seen only through web-search result summaries. The page itself could not be opened from this sandbox. Treat as unverified until someone reads the page. |
| **UNVERIFIED** | Could not be confirmed from a primary source. Collected in section 10. |

**Access limits of this research session.** Only github.com and raw.githubusercontent.com were reachable. These were blocked: docs.m5stack.com (product page, pin map), the M5Stack document server that hosts the schematic PDF (`Sch_M5CardputerAdv_v1.0_2025_06_20_17_19_58.pdf`, name from a search result), registry.platformio.org, espressif.com. **The Cardputer-Adv schematic was not read.** Everything about wiring comes from source code that runs on the board.

Pinned sources used throughout:

| Short name | Repository @ ref |
|---|---|
| M5Unified | [m5stack/M5Unified @ 0.2.29](https://github.com/m5stack/M5Unified/tree/0.2.29) (`44952cb`), also 0.3.0 (`96576a1`) |
| M5GFX | [m5stack/M5GFX @ 0.2.36](https://github.com/m5stack/M5GFX/tree/0.2.36) (`7cb26b4`), also 0.3.0 (`28d4794`) |
| M5Cardputer | [m5stack/M5Cardputer @ 1.1.1](https://github.com/m5stack/M5Cardputer/tree/1.1.1) (`e023280`), also tag 1.2.0 (`2d4fa66`) |
| ADV factory demo | [m5stack/M5Cardputer-UserDemo, branch CardputerADV @ 9d71903](https://github.com/m5stack/M5Cardputer-UserDemo/tree/9d7190317d49ff87fa98db345b049ed77c5a1780) (official, ESP-IDF 5.4.2) |
| UIFlow | [m5stack/uiflow-micropython @ f20d5a0](https://github.com/m5stack/uiflow-micropython/tree/f20d5a00e8c9106aae1d944f3d6f1128839f59cb) (official) |
| Meshtastic | [meshtastic/firmware @ bb98c8a](https://github.com/meshtastic/firmware/tree/bb98c8ab6bbd6cfee332c9656fdce47ee79efbc5) |
| espp | [esp-cpp/espp @ 351825c](https://github.com/esp-cpp/espp/tree/351825c16377173ff9fd6716a0f089eaf461d7d2/components/m5stack-cardputer) |
| Rust BSP | [tszarzynski/cardputer-adv @ 2de5578](https://github.com/tszarzynski/cardputer-adv/tree/2de55783bc6c33f4d4e221cbb027c5957c939a1a) ("all but the microSD card tested on the device") |
| Launcher | [bmorcelli/Launcher @ 2f43aa7](https://github.com/bmorcelli/Launcher/tree/2f43aa7bd3ef03e770c0a97135cbcfcc79c15aba/boards/m5stack-cardputer) |

Note on file names: M5Unified moved its sources from `*.cpp` to `*.inl` in 0.2.23. Line numbers below are for tag 0.2.29.

---

## 1. MCU and memory

| Item | Value | Source |
|---|---|---|
| Module | Stamp-S3A | [SNIPPET] docs.m5stack.com/en/core/Cardputer-Adv |
| SoC | ESP32-S3FN8 | [SRC] UIFlow board config: `MICROPY_HW_MCU_NAME "ESP32-S3-FN8"` ([mpconfigboard.h L8](https://github.com/m5stack/uiflow-micropython/blob/f20d5a00e8c9106aae1d944f3d6f1128839f59cb/m5stack/boards/M5STACK_CardputerADV/mpconfigboard.h#L8)) |
| Flash | 8 MB, QIO (image header says DIO, see section 8) | [SRC] factory demo `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` ([sdkconfig.defaults L5](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/sdkconfig.defaults#L5)); UIFlow uses `sdkconfig.flash_8mb` + `sdkconfig.flash_qio` ([mpconfigboard.cmake L29-L37](https://github.com/m5stack/uiflow-micropython/blob/f20d5a00e8c9106aae1d944f3d6f1128839f59cb/m5stack/boards/M5STACK_CardputerADV/mpconfigboard.cmake#L29-L37)) |
| PSRAM | **None** | [SRC, by absence] Neither official build enables SPIRAM: the factory demo's `sdkconfig.defaults` has no `CONFIG_SPIRAM`, and UIFlow's CardputerADV config lists no `sdkconfig.spiram_*` (its CoreS3 config does). [3P hw] C.P.S. reports a boot error "PSRAM chip not found" when `-DBOARD_HAS_PSRAM` was set ([platformio.ini](https://github.com/oldtokage-collab/C.P.S.--Cardputer-SYHNTH--/blob/1931da267bdec85ccbd6ccec8be1a4ff6d195878/platformio.ini)). |
| CPU clock | 240 MHz | [SRC] `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` ([sdkconfig.defaults L20](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/sdkconfig.defaults#L20)); PlatformIO board `f_cpu 240000000L` |
| Cores | 2 (Xtensa LX7) | Espressif ESP32-S3 datasheet, not fetched this session. Consistent with the Arduino core build (`ARDUINO_RUNNING_CORE=1`, no `CONFIG_FREERTOS_UNICORE`). |
| RAM | Internal only. Linker reports 341,760 bytes of DIRAM for the app. | [BUILD] size report of the probe build |

Consequences of no PSRAM, from projects that ran on the device [3P hw, C.P.S. `platformio.ini` and `src/main.cpp` comments]:

- Free heap at boot was about 59 KB before `SD.begin()` and about 31 KB after. Mounting the card cost about 27 KB. These numbers are for that firmware, not a general budget.
- A leaked full-screen sprite made the I2S driver fail to allocate its DMA buffers, which gave silence. Allocate audio before large UI buffers.
- A 240x135 16-bit sprite is 64,800 bytes.

Octal PSRAM cannot be added: GPIO33-37 are the display pins ([SRC] M5GFX marks the board `touches_opi_pins = true`, [esp32s3_board_data.hpp L276-L291 @0.3.0](https://github.com/m5stack/M5GFX/blob/0.3.0/src/board_detect/m5/generated/esp32s3_board_data.hpp#L276-L291)).

---

## 2. Audio path

### 2.1 Pin and bus table

| Signal | GPIO | Source |
|---|---|---|
| Codec | ES8311, I2C address **0x18** | [SRC] `es8311_i2c_addr0 = 0x18` ([M5Unified.inl L524](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L524)) |
| Internal I2C SDA | **8** | [SRC] M5Unified pin table, columns are "In SCL,SDA, EX SCL,SDA": `{ board_M5CardputerADV, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_1, GPIO_NUM_2 }` ([L96-L110](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L96-L110)); Meshtastic `I2C_SDA 8 / I2C_SCL 9` ([variant.h L29-L30](https://github.com/meshtastic/firmware/blob/bb98c8ab6bbd6cfee332c9656fdce47ee79efbc5/variants/esp32s3/m5stack_cardputer_adv/variant.h#L29-L30)) |
| Internal I2C SCL | **9** | same |
| I2S BCLK | **41** | [SRC] M5Unified speaker config ([L3105-L3116](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L3105-L3116)) |
| I2S LRCK / WS | **43** | same |
| I2S DOUT (ESP32 to codec DSDIN) | **42** | same |
| I2S DIN (codec ASDOUT to ESP32) | **46** | [SRC] M5Unified mic config ([L2758-L2765](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L2758-L2765)) |
| I2S MCLK | **not connected** | See 2.2 |
| Amp enable | **no GPIO** | See 2.4 |
| Headphone detect | **no GPIO** | See 2.4 |

The internal I2C bus (GPIO 8/9) also carries the TCA8418 keyboard controller (0x34) and the BMI270 IMU. M5Unified runs it as `I2C_NUM_1` through its own `M5.In_I2C` object ([L2158-L2165](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L2158-L2165)).

All four I2S pin numbers are confirmed independently by Meshtastic ([variant.h L69-L75](https://github.com/meshtastic/firmware/blob/bb98c8ab6bbd6cfee332c9656fdce47ee79efbc5/variants/esp32s3/m5stack_cardputer_adv/variant.h#L69-L75)), espp, and the Rust BSP ([pins.rs](https://github.com/tszarzynski/cardputer-adv/blob/2de55783bc6c33f4d4e221cbb027c5957c939a1a/src/pins.rs)).

### 2.2 Clocking: no MCLK, the codec runs from BCLK

- [SRC] M5Unified sets no `pin_mck` for this board and its codec init says `0x01, 0xB5 // CLOCK_MANAGER/ MCLK=BCLK` and `0x02, 0x18 // CLOCK_MANAGER/ MULT_PRE=3`.
- [3P] Meshtastic: `#define DAC_I2S_MCLK 45 // dummy`.
- [3P] espp: "ES8311 ... has no MCLK pin: it is clocked from BCLK" ([cardputer_adv_codec.hpp](https://github.com/esp-cpp/espp/blob/351825c16377173ff9fd6716a0f089eaf461d7d2/components/m5stack-cardputer/src/cardputer_adv_codec.hpp)).
- [3P hw] Rust BSP: "the board has no MCLK line", and for register 0x02: "MCLK = BCLK x 8 (256 fs at 32 bits a frame)" ([codec.rs L138-L168](https://github.com/tszarzynski/cardputer-adv/blob/2de55783bc6c33f4d4e221cbb027c5957c939a1a/src/codec.rs#L138-L168)).

Practical rule: keep the I2S frame at **16-bit slots x 2 slots = 32 BCLK per sample**. The codec multiplies BCLK by 8 to get its 256 fs internal clock, so the sample rate follows whatever BCLK you generate and no register change is needed when the rate changes. A 32-bit slot width would double BCLK and need a different register 0x02 value (UNVERIFIED which).

Sample rates seen working on the device with this scheme: 16 kHz (cardputer-studio), 22.05 kHz (Cardputer-Adv-Tracker, MicroGroove), 44.1 kHz (C.P.S., Rust BSP), 48 kHz (M5Unified default). **32 kHz was not found in any project.** It should work because the clock is ratio-locked, but that is UNVERIFIED.

The absence of an MCLK trace is inferred from code, not read from the schematic.

### 2.3 How M5Unified configures the board

Board enum: `board_M5CardputerADV = 24` ([M5GFX boards.hpp L35](https://github.com/m5stack/M5GFX/blob/0.2.36/src/lgfx/boards.hpp#L35)).

Speaker pins and callback, [M5Unified.inl L3105-L3116 @0.2.29](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L3105-L3116):

```cpp
      case board_t::board_M5Cardputer:
      case board_t::board_M5CardputerADV:
        if (cfg.internal_spk)
        {
          spk_cfg.pin_bck = GPIO_NUM_41;
          spk_cfg.pin_ws = GPIO_NUM_43;
          spk_cfg.pin_data_out = GPIO_NUM_42;
          spk_cfg.magnification = 16;
          spk_cfg.i2s_port = I2S_NUM_1;
          if (_board == board_t::board_M5CardputerADV) {
            spk_enable_cb = _speaker_enabled_cb_cardputer_adv;
          }
        }
        break;
```

Codec init, [M5Unified.inl L1045-L1068 @0.2.29](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L1045-L1068). Added in commit [5931f66](https://github.com/m5stack/M5Unified/commit/5931f6697fdb3864860118f01b5db15669e1eb32) (2025-08-06), first released in 0.2.8, unchanged through 0.3.0:

```cpp
  bool M5Unified::_speaker_enabled_cb_cardputer_adv(void* args, bool enabled)
  {
    (void)args;
    (void)enabled;
#if defined (CONFIG_IDF_TARGET_ESP32S3)
    static constexpr const uint8_t enabled_bulk_data[] = {
      2, 0x00, 0x80,  // 0x00 RESET/  CSM POWER ON
      2, 0x01, 0xB5,  // 0x01 CLOCK_MANAGER/ MCLK=BCLK
      2, 0x02, 0x18,  // 0x02 CLOCK_MANAGER/ MULT_PRE=3
      2, 0x0D, 0x01,  // 0x0D SYSTEM/ Power up analog circuitry
      2, 0x12, 0x00,  // 0x12 SYSTEM/ power-up DAC - NOT default
      2, 0x13, 0x10,  // 0x13 SYSTEM/ Enable output to HP drive - NOT default
      2, 0x32, 0xBF,  // 0x32 DAC/ DAC volume (0xBF == ±0 dB )
      2, 0x37, 0x08,  // 0x37 DAC/ Bypass DAC equalizer - NOT default
      0
    };
    static constexpr const uint8_t disabled_bulk_data[] = {
      0
    };

    in_i2c_bulk_write(es8311_i2c_addr0, enabled ? enabled_bulk_data : disabled_bulk_data);
#endif
    return true;
  }
```

Each entry is `length, register, value`. Writes go through `M5.In_I2C.writeRegister()` at 100 kHz ([L504-L522](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L504-L522)).

Other facts from the same code:

- `M5.begin()` only stores the speaker config and callback ([L3316-L3321](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L3316-L3321)). The I2S driver and the codec are started lazily by the first `play*()`/`tone()` call or by `Speaker.begin()`.
- `Speaker.begin()` runs the codec callback first, then installs I2S, then starts the mixer task ([Speaker_Class.inl L1079-L1150](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Speaker_Class.inl#L1079-L1150)).
- The "disabled" sequence is empty, so `Speaker.end()` leaves the DAC and output stage powered. [3P hw] guitar-audio-tools reports the idle amplifier then hums and powers it down by hand with `0x32=0x00, 0x12=0x02, 0x13=0x40` ([es8311.cpp L21-L25](https://github.com/c5e3/guitar-audio-tools/blob/46b3d0dada4008d24a6d992eaf8f7298b91763f5/src/hw/es8311.cpp#L21-L25)).
- Output format: 16-bit, Philips I2S, port `I2S_NUM_1`, `stereo = false`. In mono mode M5Unified sets `tx_mono` and `tx_chan_equal`, so the same sample goes out in both slots ([Speaker_Class.inl L447-L453](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Speaker_Class.inl#L447-L453)).
- Default output rate is 48,000 Hz ([Speaker_Class.hpp L48](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Speaker_Class.hpp#L48)).
- The official factory demo does nothing more than `M5.begin(); M5.Speaker.begin();` and then `playRaw`/`playWav` ([hal.cpp L26-L39](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/main/hal/hal.cpp#L26-L39)). It has no amp-enable or headphone-detect code.

ES8311 register meanings above are the comments of the cited code. The ES8311 datasheet was not read. Volume register 0x32: 0x00 = -95.5 dB, 0xBF = 0 dB, 0.5 dB per step [3P, Rust BSP `codec.rs`].

### 2.4 Speaker amplifier, headphone jack, mono or stereo

| Question | Answer | Confidence |
|---|---|---|
| Amplifier chip | NS4150B, mono, drives the 8 ohm 1 W speaker | [SNIPPET] product page; named in espp and C.P.S. comments |
| How is the amp enabled? | No firmware examined toggles a GPIO for it. The only steps are the codec register writes above (0x12 = DAC power, 0x13 = output driver). | [SRC] for "no GPIO is used" |
| Headphone detection | Hardware only. "When a 3.5mm headphone is inserted, the speaker amplifier will be disabled." | [SNIPPET] product page |
| Mechanism | "The NS4150B CTRL pin (AMP_EN) is not a GPIO. On the v1.0 schematic it is pulled up by R23, and Q4 pulls it down when the PJ-342 jack asserts HP_DET." | [3P] [MothDeck PR #1](https://github.com/shard76-dark/MothDeck-ambient-house-groovebox-firmware-for-M5Stack-Cardputer-ADV-/pull/1). Its author did not test on a device, and this session could not open the schematic. **UNVERIFIED.** |
| Can firmware tell if headphones are in? | No. "The jack mute is hardware (the MCU has no detect pin)." | [3P] [bigbag player README L319-L323](https://github.com/bigbag/cardputer_adv_player/blob/f170216b2d2a0cf40fe65f888f452ab27bf33c7e/README.md#L319-L323); consistent with all code read |
| Mono or stereo | The ES8311 has one DAC channel, so the signal is **mono**. Whether the jack feeds that one signal to both tip and ring is **UNVERIFIED** (schematic). | M5Unified uses `stereo = false`; espp: "The speaker is mono"; bigbag: "mono ES8311 output" |

Design consequence: one software volume has to serve both the speaker and headphones, and the firmware cannot switch curves automatically. bigbag's player notes that headphones need low levels and the speaker high levels.

One project (MothDeck) claims the internal speaker stays silent with its firmware. Its own README says the image "has not been tried on a Cardputer". M5Stack's factory firmware and device-tested projects (Rust BSP, guitar-audio-tools, Cardputer-Adv-Tracker) use the same register sequence for the speaker, so that claim is not used here.

### 2.5 Microphone (only matters if sampling is planned)

- Analog MEMS mic into the ES8311 ADC, read over I2S on GPIO46 with the same BCLK/WS [SRC M5Unified mic config; espp header comment].
- M5Unified's mic callback resets the codec (`0x00=0x80`) and enables only the ADC clocks (`0x01=0xBA`) ([L1537 onwards](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L1537)). `M5.Mic` and `M5.Speaker` therefore reconfigure the codec against each other. Full duplex needs `0x01=0xBF` and one shared I2S port, as espp does ([cardputer_adv_codec.cpp](https://github.com/esp-cpp/espp/blob/351825c16377173ff9fd6716a0f089eaf461d7d2/components/m5stack-cardputer/src/cardputer_adv_codec.cpp)).
- Open ESP-IDF issue: on IDF 5.5.x and later the ADV mic can return constant samples ([espressif/esp-idf#18621](https://github.com/espressif/esp-idf/issues/18621), open as of this research). It concerns RX, not TX.

---

## 3. Producing a continuous audio stream

### 3.1 Option A: `M5.Speaker.playRaw` with queued buffers

What the class does, from [Speaker_Class.hpp](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Speaker_Class.hpp) and [Speaker_Class.inl](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Speaker_Class.inl) at 0.2.29:

- 8 virtual channels (`sound_channel_max = 8`, hpp L275). Each holds the request being played plus a small queue (`wavinfo[2]`, hpp L335). `isPlaying(ch)` returns 0, 1 or 2 (hpp L113).
- `playRaw()` stores only a **pointer**. The buffer must stay untouched until the task has finished reading it. The header says to rotate three buffers, or two if you use `setBufferReleaseCallback()` (hpp L208, callback added in 0.2.22, commit [77575cf](https://github.com/m5stack/M5Unified/commit/77575cf41060fc143ab0b84ac256c673edb175e9)).
- With an explicit channel, `playRaw()` blocks in 1 ms steps until a queue slot is free (inl L1346-L1400). That is what paces a generator loop.
- A background task `spk_task` (default priority 2, not pinned; hpp L73-L76) mixes all channels in **float**, with per-channel **linear-interpolation resampling** from each request's rate to `cfg.sample_rate` (inl L821-L944). Gain is `magnification x master_volume^2 x channel_volume^2` (inl L550, L718). The result is clipped to int16 and written to I2S with a blocking write.
- If the request rate equals `cfg.sample_rate`, the interpolator reduces to a one-sample delay with no blending (my reading of the loop at inl L906-L944; not measured).
- On ESP32-S3 M5Unified programs the I2S clock dividers itself and uses the achieved rate in the resampler (inl L400-L470).
- Defaults: `sample_rate 48000`, `dma_buf_len 256`, `dma_buf_count 8` (hpp L48, L67, L70). DMA depth alone is 8 x 256 / 48000 = 43 ms.

Latency is roughly: DMA depth (`dma_buf_count x dma_buf_len / rate`) + the queued user buffers + the buffer being rendered. Projects that tuned it use `dma_buf_count = 4`, `dma_buf_len = 256`, `task_priority = 3` at 22.05 kHz ([Cardputer-Adv-Tracker L1061-L1069](https://github.com/qwertyuu/Cardputer-Adv-Tracker/blob/5c7e4f7afb910c714b0dc0e0dd0186603d127bc7/CardputerTracker/CardputerTracker.ino#L1061-L1069)), which is about 46 ms of DMA plus about 23 ms of queued buffers.

Version traps:

- **M5Unified older than 0.2.20 swaps every pair of 16-bit samples on ESP32-S3.** "mono output produced audible fs/2 image noise ... most audible at lower sample rates" (commit [1fd73f3](https://github.com/m5stack/M5Unified/commit/1fd73f3ee71101ab23ccc9d5156e71db1faab81a), 2026-08-16). Use 0.2.20 or newer.
- 0.2.23 serialised `play*()`, `stop()` and `end()` against the task (release notes).

It is realistic: these synths on the Cardputer or Cardputer-Adv all stream through `playRaw`.

| Project | Rate, block | Notes |
|---|---|---|
| [qwertyuu/Cardputer-Adv-Tracker](https://github.com/qwertyuu/Cardputer-Adv-Tracker/blob/5c7e4f7afb910c714b0dc0e0dd0186603d127bc7/CardputerTracker/CardputerTracker.ino#L345-L390) [3P hw] | 22,050 Hz, 256 | Two buffers alternated, `playRaw(buf, 256, 22050, false, 1, 0)` retried until accepted. Render task on core 0 at priority 1. Arduino IDE build, board `m5stack:esp32:m5stack_cardputer`. |
| [matoslav/MicroGroove](https://github.com/matoslav/MicroGroove/blob/f57b62952f4161a4230165198966d3559e256d6b/audio_engine.cpp#L51-L59) [3P] | 22,050 Hz | Same pattern, derived from the tracker. |
| [oldtokage-collab/C.P.S.](https://github.com/oldtokage-collab/C.P.S.--Cardputer-SYHNTH--/blob/1931da267bdec85ccbd6ccec8be1a4ff6d195878/src/main.cpp#L15486-L15505) [3P hw] | 44,100 Hz, `dma_buf_len 512` | Speaker task pinned to the app core, render task on the other core at priority 5. One buffer period is about 23 ms. PlatformIO `espressif32@6.7.0`. |
| [urtubia/miniacid](https://github.com/urtubia/miniacid/blob/d6b8ab50c5b78802f62da29bd166fbd70e83a5a3/miniacid.ino#L29-L49) [3P] | | Waits for `isPlaying()` to clear, then queues the next buffer. |
| [shard76-dark/MothDeck](https://github.com/shard76-dark/MothDeck-ambient-house-groovebox-firmware-for-M5Stack-Cardputer-ADV-/blob/fb4660c9155cdb6066870f72d3ef120dd3696a98/src/AudioEngine.cpp) [3P, not run on hardware] | 44,100 Hz, 256 | Three buffers, keeps two queued. |

### 3.2 Option B: drive I2S directly after initialising the codec ourselves

Projects that do this on the ADV, and exactly how:

| Project | Codec init | I2S setup |
|---|---|---|
| [Cardenza-Community/cardputer-studio](https://github.com/Cardenza-Community/cardputer-studio/blob/0315a43538fc571e6292965d56ecfa3bd38ae849/studio/synth_engine.cpp#L176-L210) [3P; its register comments describe tuning by ear], a synth/groovebox, no M5Unified at all | `Wire.begin(8, 9, 400000)`, then `0x00=0x80, 0x01=0xB5, 0x02=0x18, 0x09=0x0C, 0x0A=0x0C, 0x0D=0x01, 0x12=0x00, 0x13=0x10, 0x32=vol, 0x37=0x08` ([ES8311Audio.cpp L33-L57](https://github.com/Cardenza-Community/cardputer-studio/blob/0315a43538fc571e6292965d56ecfa3bd38ae849/studio/ES8311Audio.cpp#L33-L57)). The two extra writes set 16-bit word length. | **Legacy** driver (`driver/i2s.h`), `I2S_NUM_1`, master TX, 16-bit, `I2S_CHANNEL_FMT_RIGHT_LEFT`, `I2S_COMM_FORMAT_STAND_I2S`, `mck_io_num = I2S_PIN_NO_CHANGE`, BCLK 41, WS 43, DOUT 42, `dma_buf_count 6`, `dma_buf_len 128`, `tx_desc_auto_clear`. Synth task pinned to core 0 at priority 5 writes interleaved L=R with `i2s_write(..., portMAX_DELAY)`. pioarduino 54.03.20. |
| [bigbag/cardputer_adv_player](https://github.com/bigbag/cardputer_adv_player/blob/f170216b2d2a0cf40fe65f888f452ab27bf33c7e/src/audio_out.cpp#L10-L97) [3P], MP3/WAV player | The eight M5Unified register writes, sent with `M5.In_I2C.writeRegister(0x18, ...)`, 2 ms apart. | Legacy driver, `I2S_NUM_0`, same format and pins, `dma_buf_count 8`, `dma_buf_len 256`. Uses M5Unified for display and I2C only. `espressif32@6.7.0` (Arduino core 2.x), where M5Unified also uses the legacy driver. |
| [esp-cpp/espp](https://github.com/esp-cpp/espp/blob/351825c16377173ff9fd6716a0f089eaf461d7d2/components/m5stack-cardputer/src/audio.cpp#L73-L101) [3P], ESP-IDF BSP | `0x00=0x80, 0x01=0xBF, 0x02=0x18, 0x0D=0x01`, then DAC `0x12=0x00, 0x13=0x10, 0x32=0xBF, 0x37=0x08`. | New `i2s_std` driver, one full-duplex port, BCLK/WS/DOUT/DIN, no MCLK, one playback channel, 44.1 kHz default. |
| [tszarzynski/cardputer-adv](https://github.com/tszarzynski/cardputer-adv/blob/2de55783bc6c33f4d4e221cbb027c5957c939a1a/src/speaker.rs#L155-L161) [3P hw], Rust | Starts the I2S stream first, then the eight M5Unified writes. | Philips, `Data16Channel16`, stereo frames with the same sample left and right, 44.1 kHz, 4096-byte DMA ring = 1024 frames = 23 ms. |
| [meshtastic/firmware](https://github.com/meshtastic/firmware/blob/bb98c8ab6bbd6cfee332c9656fdce47ee79efbc5/src/platform/extra_variants/m5stack_cardputer_adv/variant.cpp#L59-L92) [3P] | Generic ES8311 driver, then overwrites with the eight M5Unified writes. | Through arduino-audio-driver / ESP8266Audio, with a dummy MCLK pin. |

Both init orders are in use: M5Unified, bigbag and cardputer-studio write the registers before I2S runs; the Rust BSP starts the clock first. The Rust author notes the codec clicks when BCLK stops and starts, and powers the DAC down before stopping the clock.

**Legacy and new I2S drivers cannot coexist in one binary on Arduino core 3.x.** ESP-IDF 5 aborts at startup with "CONFLICT! The new i2s driver can't work along with the legacy i2s driver" if both are linked ([i2s_legacy.c L1989-L2003 @v5.5.1](https://github.com/espressif/esp-idf/blob/v5.5.1/components/driver/deprecated/i2s_legacy.c#L1989-L2003)). M5Unified uses the new driver whenever `<driver/i2s_std.h>` exists ([Speaker_Class.inl L184 onwards](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Speaker_Class.inl#L184)). The installed Arduino core 3.3.12 does not define `CONFIG_I2S_SKIP_LEGACY_CONFLICT_CHECK` [BUILD environment]. So on core 3.x use `driver/i2s_std.h`; on core 2.x use `driver/i2s.h`.

Compile-checked sketch of option B [BUILD only, never run on a device]:

```cpp
#include <M5Cardputer.h>
#include <driver/i2s_std.h>

static i2s_chan_handle_t s_tx;

static bool es8311_dac_on() {                       // M5Unified's sequence, section 2.3
  static const uint8_t seq[][2] = {
    {0x00, 0x80}, {0x01, 0xB5}, {0x02, 0x18}, {0x0D, 0x01},
    {0x12, 0x00}, {0x13, 0x10}, {0x32, 0xBF}, {0x37, 0x08},
  };
  bool ok = true;
  for (auto &r : seq) ok &= M5.In_I2C.writeRegister8(0x18, r[0], r[1], 100000);
  return ok;
}

static bool i2s_start(uint32_t rate) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.dma_desc_num  = 4;                           // 4 x 128 frames = 11.6 ms at 44.1 kHz
  chan.dma_frame_num = 128;
  chan.auto_clear    = true;                        // silence, not stale data, on underrun
  if (i2s_new_channel(&chan, &s_tx, nullptr) != ESP_OK) return false;
  i2s_std_config_t cfg = {};
  cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;              // no MCLK on this board
  cfg.gpio_cfg.bclk = GPIO_NUM_41;
  cfg.gpio_cfg.ws   = GPIO_NUM_43;
  cfg.gpio_cfg.dout = GPIO_NUM_42;
  cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;              // GPIO_NUM_46 for the mic
  if (i2s_channel_init_std_mode(s_tx, &cfg) != ESP_OK) return false;
  return i2s_channel_enable(s_tx) == ESP_OK;
}

// setup():
//   auto cfg = M5.config();
//   cfg.internal_spk = false;   // M5.Speaker never claims I2S_NUM_1
//   cfg.internal_mic = false;
//   M5Cardputer.begin(cfg, true);
//   i2s_start(44100); es8311_dac_on();
//   xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
//
// audio_task: render N frames, write int16 L,R pairs with L == R:
//   i2s_channel_write(s_tx, buf, frames * 4, &written, portMAX_DELAY);
```

Notes on the sketch:

- Stereo framing with the sample duplicated is what cardputer-studio, bigbag's player and the Rust BSP send. `I2S_SLOT_MODE_MONO` should give the same wire format with half the memory traffic (it is what M5Unified effectively does), but that path is UNVERIFIED with the IDF driver on this board.
- Latency is the DMA depth plus one render block. With 4 x 128 frames: 11.6 ms at 44.1 kHz, 16 ms at 32 kHz.
- `i2s_channel_write` with `portMAX_DELAY` blocks until space is free, so it paces the render loop.
- Put the audio task on core 0. The Arduino `loop()` task, and so display pushes and key polling, runs on core 1 (`ARDUINO_RUNNING_CORE=1` in the board definition). With Wi-Fi and Bluetooth off, core 0 has little else to do. Device-tested projects use the same split.

### 3.3 Recommendation

Use **option B** for the groovebox engine: the latency is set by two numbers you choose, there is no float mixer/resampler pass or pointer-lifetime rule in the way, and the wire format matches what other direct-I2S projects on this board send. Keep **option A** as the fallback and as the first hardware smoke test, because it is the path M5Stack's own factory firmware exercises: if `M5.Speaker.tone()` is silent on a unit, the problem is not in our I2S code. For option A set `cfg.sample_rate` equal to the engine rate, use channel 0, three rotating buffers, and M5Unified 0.2.20 or newer.

What nobody has shown, including this research: glitch-free playback at 32 kHz on this board, and measured latency for either option.

---

## 4. Keyboard

### 4.1 Controller

| | Original Cardputer | Cardputer-Adv |
|---|---|---|
| Method | GPIO matrix through a 74HC138 | **TCA8418** keypad controller on I2C |
| Pins | outputs 8, 9, 11; inputs 13, 15, 3, 4, 5, 6, 7 ([IOMatrix.h L29-L30](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/KeyboardReader/IOMatrix.h#L29-L30)) | SDA 8, SCL 9, **INT 11**, address **0x34** |

ADV sources [SRC]: `#define DEFAULT_TCA8418_INT_PIN 11` ([TCA8418.cpp L13](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/KeyboardReader/TCA8418.cpp#L13)); `TCA8418_DEFAULT_ADDR 0x34` on `m5::In_I2C` at 400 kHz ([Adafruit_TCA8418.h L28, L74-L75](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Adafruit_TCA8418/Adafruit_TCA8418.h#L28)); factory demo `HAL_PIN_KEYBOARD_INT 11` ([hal_config.h L8](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/main/hal/hal_config.h#L8)). The controller is configured as 7 rows x 8 columns (`matrix(7, 8)`, TCA8418.cpp L39). INT is active low while events are queued (Launcher and the Rust BSP both rely on that).

### 4.2 Library versions

- **M5Cardputer 1.1.0** is the first release with ADV support ("Merges PR #10 ... adds CardputerADV support", [releases](https://github.com/m5stack/M5Cardputer/releases); tag dated 2025-09-04).
- **1.1.1** (2025-09-28) fixes modifier handling order; the release note says uppercase Z, C, B and M did not work in 1.1.0. Use 1.1.1 or newer.
- Git tag **1.2.0** (2026-06-08) adds an Fn layer. Its `library.json` still says `"version": "1.1.1"`, so whether the PlatformIO registry serves it as 1.2.0 is UNVERIFIED. Pin it by git tag if you want it.
- The reader is chosen at run time from `M5.getBoard()` ([Keyboard.cpp L15-L31](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/Keyboard.cpp#L15-L31)). That needs M5Unified 0.2.8+ and M5GFX 0.2.10+ for the `board_M5CardputerADV` value.

### 4.3 API (M5Cardputer 1.1.1, [Keyboard.h](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/Keyboard.h))

```cpp
M5Cardputer.begin(cfg, /*enableKeyboard=*/true);
M5Cardputer.update();            // M5.update(); Keyboard.updateKeyList(); Keyboard.updateKeysState();

Keyboard_Class &kb = M5Cardputer.Keyboard;
uint8_t  kb.isPressed();         // number of keys currently held
bool     kb.isChange();          // true if that number differs from the previous call
bool     kb.isKeyPressed(char c);// is a key producing c held (honours shift/ctrl/caps)
const std::vector<Point2D_t>& kb.keyList();   // held keys as {x = column 0..13, y = row 0..3}
KeyValue_t kb.getKeyValue(p);    // {value_first, value_second} for a coordinate
uint8_t  kb.getKey(p);           // character for a coordinate with current modifiers
Keyboard_Class::KeysState& kb.keysState();
//   bool tab, fn, shift, ctrl, opt, alt, del, enter, space;  uint8_t modifiers;
//   std::vector<char> word;            // printable keys currently held, shift applied
//   std::vector<uint8_t> hid_keys, modifier_keys;
bool kb.capslocked();  void kb.setCapsLocked(bool);
void kb.begin(std::unique_ptr<KeyboardReader>);  // inject your own reader
```

Behaviour that matters for an instrument, all read from the source:

- It reports **state, not events**. There is no key-down/key-up callback. You get the set of held keys and diff it yourself.
- `isChange()` compares only the **count** of held keys ([Keyboard.cpp L66-L75](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/Keyboard.cpp#L66-L75)).
- On the ADV, **each `update()` consumes at most one TCA8418 event** ([TCA8418.cpp L52-L73](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/KeyboardReader/TCA8418.cpp#L52-L73)). A three-key chord takes three `update()` calls to appear. At a 1 ms loop that is invisible; at one call per 33 ms frame it smears a chord over 100 ms.
- In 1.1.1 `keysState().del` is the **backspace** key. In tag 1.2.0 the same key sets `backspace`, `del` means Fn+backspace, and while Fn is held `word` stays empty (the function returns after the Fn pass). 1.2.0 also adds `esc`, `f1`..`f12`, `up`, `down`, `left`, `right`.
- It uses `std::vector` on every update. Keep it out of the audio task.
- The bundled header comment says "bit 7 ... press = 0 or release == 1". The code treats bit 7 set as pressed, and that is what works on the device.

**Rollover.** The TCA8418 queues key events in a FIFO (10 deep per the TI datasheet, which was not fetched here) and the library drains it one event per `update()`, so several held keys are tracked. Whether the ADV matrix has per-key diodes, and so whether three keys on the corners of a matrix rectangle produce a ghost fourth key, is **UNVERIFIED**. If it has none, a+s+d would register f (see the formula in 4.5). The original Cardputer example carries the comment "max press 3 button at the same time". Test this on hardware before designing chord input. Debounce time inside the TCA8418 is also unchecked (datasheet not read).

### 4.4 Getting real key-down and key-up events

Compile-checked reader that drains the whole FIFO per poll and uses no heap [BUILD only]. Pass `enableKeyboard = false` to `M5Cardputer.begin()` if you use it instead of the library's reader.

```cpp
#include <M5Unified.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>            // ships inside M5Cardputer >= 1.1.0
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418_registers.h>

struct AdvKeyEvent { uint8_t x, y; bool down; };    // x = column 0..13, y = row 0..3 (0 = top)

class AdvKeys {
public:
  bool begin(int int_pin = 11) {
    _int = int_pin;
    if (!_tca.begin()) return false;                // 0x34 on M5.In_I2C
    _tca.matrix(7, 8);
    _tca.flush();
    pinMode(_int, INPUT);
    _tca.enableInterrupts();
    return true;
  }
  size_t poll(AdvKeyEvent *out, size_t max) {
    if (digitalRead(_int) != LOW) return 0;         // INT low = events pending
    size_t n = 0;
    while (n < max) {
      uint8_t ev = _tca.getEvent();                 // 0 = FIFO empty
      if (!ev) break;
      uint8_t k = (ev & 0x7F) - 1, r = k / 10, c = k % 10;
      out[n++] = { (uint8_t)(r * 2 + (c > 3)), (uint8_t)(c & 3), (ev & 0x80) != 0 };
    }
    _tca.writeRegister8(TCA8418_REG_INT_STAT, 0x01);
    return n;
  }
private:
  Adafruit_TCA8418 _tca;
  int _int = 11;
};
```

### 4.5 Physical layout: 4 rows x 14 columns

Coordinates are `Point2D_t{x, y}` with x = column from the left, y = row from the top. Base and shifted characters are from M5Cardputer ([Keyboard.h L18-L73 @1.1.1](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/Keyboard.h#L18-L73)); key names and the Fn layer are from the factory firmware's table ([keyboard.cpp L164-L224](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/main/hal/keyboard/keyboard.cpp#L164-L224)). Keycap artwork was not inspected.

| y \ x | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **0** | `` ` `` | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 0 | - | = | del (backspace) |
| shift | ~ | ! | @ | # | $ | % | ^ | & | * | ( | ) | _ | + | |
| fn | esc | F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8 | F9 | F10 | F11 | F12 | delete |
| **1** | tab | q | w | e | r | t | y | u | i | o | p | [ | ] | \ |
| shift | | Q | W | E | R | T | Y | U | I | O | P | { | } | \| |
| **2** | **fn** | **shift (Aa)** | a | s | d | f | g | h | j | k | l | ; | ' | enter |
| shift | | | A | S | D | F | G | H | J | K | L | : | " | |
| fn | | | | | | | | | | | | up | | |
| **3** | **ctrl** | **opt** | **alt** | z | x | c | v | b | n | m | , | . | / | space |
| shift | | | | Z | X | C | V | B | N | M | < | > | ? | |
| fn | | | | | | | | | | | left | down | right | |

- Modifier positions: fn (0,2), shift/Aa (1,2), ctrl (0,3), opt (1,3), alt (2,3).
- Arrows on Fn: `;` up, `,` left, `.` down, `/` right. Fn+`` ` `` is esc. Fn+del is forward delete.
- F1..F12 on Fn+number row exist only in M5Cardputer tag 1.2.0. The factory firmware maps no F-keys.
- In M5Cardputer 1.1.1 holding **ctrl** also selects the shifted character (`getKey`, Keyboard.cpp L39-L52).
- Internal codes in the map: `KEY_FN 0xFF`, `KEY_LEFT_CTRL 0x80`, `KEY_LEFT_SHIFT 0x81`, `KEY_LEFT_ALT 0x82`, `KEY_OPT 0x00`, `KEY_BACKSPACE 0x2A`, `KEY_TAB 0x2B`, `KEY_ENTER 0x28`.
- Raw TCA8418 event: `k = (event & 0x7F) - 1`, `tcaRow = k / 10`, `tcaCol = k % 10`, pressed = bit 7. Then `x = tcaRow * 2 + (tcaCol > 3)`, `y = tcaCol % 4` ([TCA8418.cpp L75-L101](https://github.com/m5stack/M5Cardputer/blob/1.1.1/src/utility/Keyboard/KeyboardReader/TCA8418.cpp#L75-L101)). Inverse: `tcaRow = x / 2`, `tcaCol = y + 4 * (x & 1)`.

**G0 button:** GPIO0, active low, exposed as `M5Cardputer.BtnA` / `M5.BtnA` ([M5Unified.inl L3573-L3576](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L3573-L3576)). It is not part of the key matrix. It is also the boot strapping pin.

---

## 5. Display

| Item | Value | Source |
|---|---|---|
| Controller | ST7789V2 (driver class `Panel_ST7789`) | [SRC] M5GFX |
| Size | 1.14", 240 x 135 in landscape. Panel memory is 135 x 240 with offsets x 52, y 40, colours inverted. | [SRC] [M5GFX.cpp L2694-L2795 @0.2.36](https://github.com/m5stack/M5GFX/blob/0.2.36/src/M5GFX.cpp#L2694-L2795) |
| Pins | SCLK 36, MOSI 35, DC 34, CS 37, RST 33, no MISO (3-wire), backlight 38 | same; Meshtastic variant.h L3-L11 |
| Bus | `SPI3_HOST`, 40 MHz write, 16 MHz read | same |
| Backlight | PWM on GPIO38, 256 Hz, LEDC channel 7. `M5Cardputer.Display.setBrightness(0..255)`. | same |

**Rotation trap.** Up to M5GFX 0.2.36 the library sets rotation 1 for this board, so `getRotation()` is 1 and every Cardputer example calls `setRotation(1)`. **M5GFX 0.3.0 (2026-11-05) renumbers rotations**: "Rotation 0 is the board's upright orientation on every board", old `setRotation(n)` is now `setRotation((n + 3) % 4)`, and for the Cardputer the start-up picture is unchanged but `getRotation()` reads 0 ([0.3.0 release notes](https://github.com/m5stack/M5GFX/releases/tag/0.3.0), commit [a1e39fe](https://github.com/m5stack/M5GFX/commit/a1e39fe02abce9f85d4e326b8f537d3f8af34b6a)). Safest: never call `setRotation()` with a literal; the default is already landscape in both lines.

Flicker-free drawing uses an off-screen sprite pushed in one transfer. From the official examples ([inputText.ino L19-L40](https://github.com/m5stack/M5Cardputer/blob/1.1.1/examples/Basic/keyboard/inputText/inputText.ino#L19-L40)) and the factory demo ([hal.h L38-L54](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/main/hal/hal.h#L38-L54)):

```cpp
M5Canvas canvas(&M5Cardputer.Display);
canvas.setColorDepth(16);
canvas.createSprite(240, 135);       // 64,800 bytes of internal RAM; check the return value
// each frame:
canvas.fillSprite(TFT_BLACK);
// ... draw into canvas ...
canvas.pushSprite(0, 0);
```

- The factory demo uses three smaller sprites (main 204 x 109, a side bar and a top bar) and pushes only the one that changed.
- An 8-bit sprite (`setColorDepth(8)`) halves the RAM to 32,400 bytes.

**Frame rate.** A full frame is 240 x 135 x 16 = 518,400 bits, which takes 13.0 ms at 40 MHz. That caps full-screen updates near 77 fps before any drawing cost. [3P hw] the Rust BSP measured 13 ms per full frame with DMA and about 28 ms when converting pixel by pixel. A 30 fps UI therefore spends roughly 40% of each frame on the bus if it pushes the whole screen. Measured frame rates for M5GFX `pushSprite` on this board are UNVERIFIED. Keep display work on core 1 so a 13 ms push cannot stall audio.

---

## 6. Other peripherals

### microSD (SPI)

| SCK | MISO | MOSI | CS |
|---|---|---|---|
| 40 | 39 | 14 | 12 |

[SRC] M5Unified SD table ([L222](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L222)), factory demo ([hal_config.h L10-L13](https://github.com/m5stack/M5Cardputer-UserDemo/blob/9d7190317d49ff87fa98db345b049ed77c5a1780/main/hal/hal_config.h#L10-L13)), official example ([sdcard.ino L21-L54](https://github.com/m5stack/M5Cardputer/blob/1.1.1/examples/Basic/sdcard/sdcard.ino#L21-L54)):

```cpp
SPI.begin(40, 39, 14, 12);                 // SCK, MISO, MOSI, CS
if (!SD.begin(12, SPI, 25000000)) { /* no card */ }
```

- Arduino's `SPI` object on the S3 is SPI2; M5GFX uses SPI3 for the LCD, so the two do not share a controller [SRC M5GFX; BUILD environment `SPI.cpp`].
- With board `m5stack-stamps3` the default `SS/MOSI/MISO/SCK` are -1, so always pass the pins.
- The same SCK/MISO/MOSI lines go to the EXT header; the LoRa cap uses them with CS on GPIO5 [SRC factory demo]. [3P hw] Launcher drives GPIO 3, 4, 5, 6, 13 and 15 high before mounting, "Set GPIO5 HIGH for SD card compatibility" ([interface.cpp L78-L95](https://github.com/bmorcelli/Launcher/blob/2f43aa7bd3ef03e770c0a97135cbcfcc79c15aba/boards/m5stack-cardputer/interface.cpp#L78-L95)). Whether that is needed with nothing on the header is UNVERIFIED.
- [3P hw] `SD.begin()` cost about 27 KB of heap in C.P.S.

### IMU

BMI270 on the internal I2C bus. API: `M5.Imu.isEnabled()`, `M5.Imu.getAccel(&x,&y,&z)`, `M5.Imu.getGyro(...)`. Set `cfg.internal_imu = false` to skip it.

Address is **conflicting**: two on-device bus scans show 0x18, 0x34 and **0x69** ([Launcher CardputerADV.md L129-L140](https://github.com/bmorcelli/Launcher/blob/2f43aa7bd3ef03e770c0a97135cbcfcc79c15aba/boards/m5stack-cardputer/CardputerADV.md#L129-L140); C.P.S. `main.cpp` L1980), while espp's header says 0x68. M5Unified tries 0x69 first and then 0x68 ([IMU_Class.inl L79-L81](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/IMU_Class.inl#L79-L81)), so `M5.Imu` works either way. [3P hw] its interrupt pins do not reach the ESP32 (Rust BSP README).

### Battery

GPIO10, ADC1, through a 2:1 divider [SRC [Power_Class.inl L583-L590](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Power_Class.inl#L583-L590)].

- `M5.Power.getBatteryVoltage()` returns millivolts.
- `M5.Power.getBatteryLevel()` returns 0..100, computed as `(mV - 3300) * 100 / (4150 - 3350)` and clamped ([L2493-L2587](https://github.com/m5stack/M5Unified/blob/0.2.29/src/utility/Power_Class.inl#L2493-L2587)).
- Needs M5Unified 0.2.9+ ("Fixed an issue where CardputerADV battery level could not be obtained correctly", commit 05377f0).
- There is no charger IC on the bus and no charge-status line; M5Unified has no charging case for this board, and the Rust BSP says charging cannot be detected.
- Capacity: retailers say 1750 mAh, a search summary of the official spec table said 1500 mAh. UNVERIFIED.

### Grove port (HY2.0-4P, "Port A")

GPIO **2 = SDA, GPIO 1 = SCL**, plus 5 V and GND. [SRC] M5Unified external I2C pins (same table as 2.1), UIFlow `MICROPY_HW_I2C0_SCL (1)`, `MICROPY_HW_I2C0_SDA (2)`. Use `M5.Ex_I2C` or `Wire.begin(2, 1)`. Physical pin order on the connector is UNVERIFIED.

### EXT header (2.54 mm, 14 pins)

GPIOs present, from official code [SRC factory demo `hal_config.h` L14-L19; M5GFX comment "G5/G6 External" for the ADV]: 3, 4, 5, 6 (LoRa cap RST, DIO1, NSS, BUSY), 13 and 15 (UART to the GPS), 14/39/40 (SPI shared with the SD card), plus the internal I2C bus on 8/9 (espp: the cap's IO expander at 0x43 sits on the internal bus).

Pin numbering, from the product page via search summary [SNIPPET], partly corroborated by a user's wiring notes ([AndyAiCardputer/cardputer-adv-tests README L257-L265](https://github.com/AndyAiCardputer/cardputer-adv-tests/blob/263cbb4453dcc3b44dcdb9b510c26d31cd19a7af/README.md#L257-L265), pins 1, 2, 4, 5, 7, 9, 11, 13):

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | G3 (RESET) | 2 | 5VIN |
| 3 | G4 (INT) | 4 | GND |
| 5 | G6 (BUSY) | 6 | 5VOUT |
| 7 | G40 (SCK) | 8 | G8 (I2C SDA) |
| 9 | G14 (MOSI) | 10 | G9 (I2C SCL) |
| 11 | G39 (MISO) | 12 | G13 (UART) |
| 13 | G5 (CS) | 14 | G15 (UART) |

The summary reported that two M5Stack pages disagree on which of G13/G15 is TX. In the factory demo the ESP32 transmits on 13 and receives on 15. Pins 3, 6, 8, 10, 12, 14 of this table are UNVERIFIED.

### Others

| Peripheral | GPIO | Source |
|---|---|---|
| IR transmitter | 44 | [SRC] `HAL_PIN_IR_TX 44`; example `ir_nec.ino` L22 |
| RGB LED (WS2812 on the Stamp) | 21 | [SRC] [M5Unified.inl L259](https://github.com/m5stack/M5Unified/blob/0.2.29/src/M5Unified.inl#L259). [3P] powered from the backlight rail, so lit only while the backlight is on (Meshtastic variant.h L24; Rust README). |
| G0 button | 0 | section 4.5 |
| USB | native USB-Serial/JTAG, VID 303A PID 1001 | PlatformIO board `hwids` |

---

## 7. PlatformIO setup

### 7.1 Recommended `platformio.ini`

This exact configuration **compiled and linked in this session** [BUILD] with a sketch that uses the keyboard API, `M5Canvas`, `M5.In_I2C`, `i2s_std`, `SD` and `M5.Power`. It has not run on hardware.

```ini
[env:cardputer-adv]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip
board = m5stack-stamps3
framework = arduino
board_build.partitions = default_8MB.csv
board_upload.flash_size = 8MB
board_build.arduino.memory_type = qio_qspi
monitor_speed = 115200
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=1
lib_deps =
    https://github.com/m5stack/M5GFX.git#0.2.36
    https://github.com/m5stack/M5Unified.git#0.2.29
    https://github.com/Arduino-IRremote/Arduino-IRremote.git#v4.7.1
    https://github.com/m5stack/M5Cardputer.git#1.1.1
```

Why each line:

- **Platform.** pioarduino 55.03.312-1 ships Arduino core 3.3.12 on ESP-IDF 5.5.5 ([platform.json](https://github.com/pioarduino/platform-espressif32/blob/2959e98a2750832bca32fd2f849c0c274a4cdd63/platform.json)). It is the platform already installed in this project's sandbox and all its packages come from github.com.
- **Board `m5stack-stamps3`.** 8 MB flash, QIO, 240 MHz, `default_8MB.csv`, `ARDUINO_USB_MODE=1`. Meshtastic, cardputer-studio, guitar-audio-tools and bigbag's player all use it for the ADV. pioarduino has no `m5stack-cardputer` board. The stock platform gained one in v8.0.0, but its JSON says `"flash_size": "4MB"` ([m5stack-cardputer.json @v8.0.0](https://github.com/platformio/platform-espressif32/blob/v8.0.0/boards/m5stack-cardputer.json)) and would need overriding.
- **`ARDUINO_USB_CDC_ON_BOOT=1`.** Required for `Serial` to be the USB port. The stamps3 board does not set it. Without it `Serial` is UART0 on GPIO43/44, and GPIO43 is the I2S word clock.
- **`memory_type = qio_qspi`** and no `BOARD_HAS_PSRAM`: there is no PSRAM.
- **Library order and git URLs.** M5Unified declares a dependency on M5GFX, and M5Cardputer on M5Unified, M5GFX and IRremote. With registry names PlatformIO resolves those itself. In this sandbox the registry is blocked, and the build failed at "Installing IRremote @ *" until IRremote was listed as a git URL before M5Cardputer. The form above works with and without registry access. IRremote is not compiled unless you include it.
- **Pin all three M5 libraries.** M5Cardputer's manifest asks for `"M5Unified": "*"`, so an unpinned build picks up 0.3.0 and its breaking changes.

Build output [BUILD]: `firmware.bin` 601,664 bytes for the probe, static RAM 28,684 bytes, plus a merged `firmware.factory.bin`.

The project's root `platformio.ini` (written separately) uses the same platform, board and library pins, so this section and that file agree. Two remarks on choices outside this section's tested set:

- Optimisation level: [3P hw] C.P.S. tried `-O2` and went back to `-Os` because average DSP time improved but worst-case per-buffer time got worse, which its author attributes to the larger hot path fitting the instruction cache less well ([platformio.ini comment](https://github.com/oldtokage-collab/C.P.S.--Cardputer-SYHNTH--/blob/1931da267bdec85ccbd6ccec8be1a4ff6d195878/platformio.ini)). That is one firmware's measurement; measure ours before settling.
- `lib_ignore = IRremote` is fine: M5Cardputer's `src/` never includes it.

### 7.2 Library versions

| Library | First with ADV support | Recommended pin | Reason for the pin |
|---|---|---|---|
| M5GFX | **0.2.10** (commit d86a383 "add support CardputerADV", 2025-08-05) | **0.2.36** | 0.2.27 fixes the ADV being detected as a plain Cardputer when a Cap or Unit with pull-ups is on the EXT header, which made every internal I2C device fail (commit [5f8a783](https://github.com/m5stack/M5GFX/commit/5f8a783f7dbc07e8ce5c19cf8779829d1eefcde1)). |
| M5Unified | **0.2.8** (commits b21936b and 5931f66, 2025-08-05/06). Its `library.json` requires M5GFX >= 0.2.10. | **0.2.29** (requires M5GFX >= 0.2.35) | 0.2.9 battery fix; 0.2.20 sample-order fix on S3; 0.2.22 buffer-release callback; 0.2.23 speaker race fixes. |
| M5Cardputer | **1.1.0** (2025-09-04) | **1.1.1** | Modifier fix. Used by every ADV project examined. |

Why not 0.3.0 yet: M5Unified 0.3.0 and M5GFX 0.3.0 were released on 2026-11-05/06 with breaking changes (rotation renumbering, a new board-detection framework, many `void` functions now returning `bool`). The same sketch also compiled against M5GFX 0.3.0 + M5Unified 0.3.0 + M5Cardputer tag 1.2.0 [BUILD], so moving later is possible. No ADV project examined uses 0.3.0.

### 7.3 Arduino core 2.x or 3.x, stock platform or pioarduino

The three libraries build on **both** core lines. Evidence: C.P.S. and bigbag's player build them on `espressif32@6.7.0` (core 2.0.16); this session, guitar-audio-tools and MothDeck build them on core 3.x. M5Unified selects the I2S driver with `__has_include(<driver/i2s_std.h>)`.

Stock `platformio/espressif32` by tag (read from each tag's `platform.json`):

| Stock platform | Date | `framework-arduinoespressif32` | Arduino core |
|---|---|---|---|
| v6.7.0 | 2024-05-14 | ~3.20016.0 | 2.0.16 |
| v6.13.0 | 2026-02-26 | ~3.20017.0 | 2.0.17 |
| v7.1.3 | 2026-09-11 | ~4.20017.0 | 2.0.17 |
| **v8.0.0** | 2026-10-19 | ~4.30312.0 | **3.3.12** ("Arduino-ESP32 v3.3.12 ... based on IDF v5.5.5", [release notes](https://github.com/platformio/platform-espressif32/releases/tag/v8.0.0)) |

So: core 3.x needed pioarduino until October 2026; from stock v8.0.0 it no longer does. Neither is required by the libraries.

Which combinations have run ADV audio on a device, according to each repository's own notes:

| Platform | Core | Projects |
|---|---|---|
| `espressif32@6.7.0`, board `esp32-s3-devkitc-1` or `m5stack-stamps3` | 2.0.16 | C.P.S. (M5Unified ^0.2.8), bigbag player (M5Unified 0.2.18) |
| pioarduino `54.03.20` | 3.2.0 / IDF 5.4.1 (per guitar-audio-tools' comment) | guitar-audio-tools (M5Cardputer 1.1.1, audio through M5Unified); cardputer-studio (no M5 libraries, legacy I2S driver) |
| pioarduino `55.03.312-1` | 3.3.12 / IDF 5.5.5 | MothDeck only, and its author did not run it on a device |

The recommended platform is therefore compile-verified here but **not** hardware-proven for audio output by anyone I could find. If output misbehaves on a real unit, the first thing to try is the same `platformio.ini` with `platform = https://github.com/pioarduino/platform-espressif32/releases/download/54.03.20/platform-espressif32.zip` (not compiled in this session). guitar-audio-tools warns against core 3.3.x for the **microphone** only (esp-idf#18621).

---

## 8. Flashing

**Download mode** [SNIPPET, docs.m5stack.com/en/core/Cardputer-Adv and /en/guide/restore_factory/cardputer_adv]: set the side power switch to OFF, hold the **G0** button while plugging in the USB-C cable, then release. The device is then in download mode. A third-party note adds that OFF only cuts the battery and USB still powers the board. This matches GPIO0 being the ESP32-S3 boot strap, but the wording was not read on the page itself.

In normal use esptool resets the chip into the bootloader over the native USB-Serial/JTAG port, so the button is only needed when firmware has crashed or disabled USB. [3P] Launcher's notes say "Put device in bootloader mode (hold GPIO0, press reset) may be needed in some cases" ([CardputerADV.md L115-L116](https://github.com/bmorcelli/Launcher/blob/2f43aa7bd3ef03e770c0a97135cbcfcc79c15aba/boards/m5stack-cardputer/CardputerADV.md#L115-L116)); bigbag's player notes that high upload baud rates can drop the USB device and uses 115200 or `--no-stub`. Whether the ADV has a reset button is UNVERIFIED.

**Offsets** [BUILD output of pioarduino 55.03.312-1; also arduino-esp32 `platform.txt` L186 and L349 with `m5stack_cardputer.build.bootloader_addr=0x0`]:

| Offset | File |
|---|---|
| 0x0000 | `bootloader.bin` |
| 0x8000 | `partitions.bin` |
| 0xE000 | `boot_app0.bin` |
| 0x10000 | `firmware.bin` |

The pioarduino platform writes a merged image automatically: `.pio/build/cardputer-adv/firmware.factory.bin`. Flash it at offset 0:

```sh
esptool --chip esp32s3 --port <PORT> --baud 460800 write-flash 0x0 firmware.factory.bin
```

(`write_flash` with an underscore on esptool older than v5.) To build a merged image by hand, keep the header settings the build already wrote, as arduino-esp32 does ([platform.txt L186](https://github.com/espressif/arduino-esp32/blob/5e1f0e15b9ed02f429b705cb49b18b78fc083691/platform.txt#L186)):

```sh
esptool --chip esp32s3 merge-bin -o merged.bin --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Do not pass `--flash-mode qio` there. The images this platform builds carry **DIO** in the header even though the board is QIO: the probe's `bootloader.bin` and `firmware.factory.bin` both start with `e9 03 02 3f` (mode byte 0x02 = DIO, 0x3f = 8 MB at 80 MHz) [BUILD]; QIO is selected through the bootloader variant (`memory_type = qio_qspi`), not through the header. arduino-esp32's Cardputer entry does the same (`build.flash_mode=dio`, `build.boot=qio`).

`default_8MB.csv`: nvs 0x9000, otadata 0xE000, app0 0x10000 (3.19 MB), app1 0x340000, spiffs 0x670000 (1.5 MB), coredump 0x7F0000.

---

## 9. Gotchas: ADV versus the original Cardputer

1. **Keyboard.** Original firmware drives GPIO 8, 9, 11 as outputs and reads 13, 15, 3-7. On the ADV 8/9 are the I2C bus and 11 is the keyboard interrupt, so old matrix code both fails and disturbs the codec and IMU bus. Use M5Cardputer 1.1.1+.
2. **Audio needs codec init.** The original has an I2S amplifier (NS4168 per espp) that plays as soon as I2S runs. The ADV is silent until the ES8311 is configured over I2C. The I2S pins are the same (41/43/42).
3. **Microphone.** Original: PDM mic, clock on 43, data on 46. ADV: I2S from the codec, BCLK 41, WS 43, data 46.
4. **Old libraries see the ADV as a plain Cardputer.** Before M5GFX 0.2.10 / M5Unified 0.2.8 there is no ADV board value. Check `M5.getBoard() == m5::board_t::board_M5CardputerADV` at boot and show an error otherwise.
5. **M5GFX before 0.2.27** misdetects the ADV when something with pull-ups sits on G5/G6 (EXT header).
6. **M5Unified before 0.2.20** swaps sample pairs on the S3 (image noise through `M5.Speaker`).
7. **M5GFX 0.3.0** renumbers rotations. `setRotation(1)` from older sketches turns the picture.
8. **Do not mix `driver/i2s.h` with M5Unified on Arduino core 3.x** (startup abort, section 3.2).
9. **`Speaker.end()` leaves the DAC on**; power it down by hand if idle hum matters.
10. **No PSRAM, tight heap.** Allocate I2S before big sprites; `SD.begin()` costs heap.
11. **Default Arduino I2C pins are wrong for this board.** Both the `m5stack_stamp_s3` and `m5stack_cardputer` variants define `SDA 13 / SCL 15` ([pins_arduino.h](https://github.com/espressif/arduino-esp32/blob/5e1f0e15b9ed02f429b705cb49b18b78fc083691/variants/m5stack_cardputer/pins_arduino.h)). Use `M5.In_I2C` for internal devices. [3P hw] C.P.S. found that re-pointing `Wire1` took the bus away from the keyboard and IMU.
12. **GPIO43 is both UART0 TX and the I2S word clock.** Keep `ARDUINO_USB_CDC_ON_BOOT=1`. The ROM boot log on UART0 may still toggle the pin at reset (inference, not measured).
13. **M5Cardputer `del` changed meaning** between 1.1.1 and tag 1.2.0 (section 4.3).
14. **M5Cardputer pulls in IRremote** as a declared dependency.
15. **Freed pins.** G3, G4, G5, G6, G13, G15 were keyboard matrix lines on the original and are EXT-header pins on the ADV. G7 is "Internal FPC" on the ADV per M5GFX's comment.
16. **Headphones cannot be detected** in software; the speaker mute is in hardware.
17. **Battery percentage needs M5Unified 0.2.9+** on the ADV, and charging state is not available.

---

## 10. UNVERIFIED list

Not confirmed from a primary source in this session:

1. **Anything from the schematic.** Absence of an MCLK trace (inferred from four codebases), the NS4150B enable circuit and HP_DET transistor, whether the jack carries the mono signal on both tip and ring, whether the jack has a mic contact, pull-ups on INT and I2C.
2. **Everything tagged [SNIPPET]** from docs.m5stack.com: module name Stamp-S3A, the download-mode procedure, EXT header pin numbers (pins 3, 6, 8, 10, 12, 14 have no second source), UART direction on G13/G15, "amplifier disabled when headphones are inserted", mic part and SNR, battery capacity (1750 vs 1500 mAh).
3. **BMI270 address**: 0x69 in two device scans, 0x68 in espp's header.
4. **Key rollover and ghosting** beyond two keys, and TCA8418 debounce latency.
5. **32 kHz output** on this codec configuration; no project uses it.
6. **Real latency and glitch behaviour** of both audio options. The option B code is compile-checked only.
7. `I2S_SLOT_MODE_MONO` with the IDF driver on this board.
8. **Measured display frame rate** with M5GFX.
9. **ES8311 register semantics** (taken from code comments; datasheet not read), including what the default word-length register does with 16-bit slots.
10. Whether exact versions (`m5stack/M5Unified@0.2.29`, `m5stack/M5GFX@0.2.36`, M5Cardputer 1.2.0) exist under those names in the PlatformIO registry. The git-URL form avoids the question.
11. Builds on the stock platform (6.x, 8.0.0) and on pioarduino 54.03.20 were not run here.
12. Need for driving GPIO5 high before SD mount when nothing is on the EXT header.
13. Grove connector pin order; presence of a reset button; keycap artwork.
14. ESP32-S3 core count and total SRAM are from general knowledge of the Espressif datasheet, not fetched here.

To close most of these: allow docs.m5stack.com and the M5Stack document host in the session's network settings and read the product page and schematic PDF; items 3-8 need a device.
