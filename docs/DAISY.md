# Pocket Groove on the Daisy Seed

The target device is built around an Electrosmith Daisy Seed: an STM32H750 (Cortex-M7, 480 MHz),
64 MB SDRAM, 8 MB QSPI flash and a stereo codec. This document covers the firmware for it in
`daisy/`, how to build and flash it from a Mac, what to wire, and what has been checked so far.
Read the first-power-on checklist before plugging anything in.

**Status (2026-11-14): nothing has run on a Seed yet.** The firmware compiles and links for the
STM32H750 without warnings, in both stages. Everything portable runs on the host: the scheduler,
the serial protocol, /int, the sample pool, the display path and the BMI270 driver are
unit-tested, and the firmware's whole `App` runs in `daisy_sim` against the real computer tools.
Only the hardware can show the rest (see "What only the hardware can show").

## Contents
1. Layout and choices
2. Setting up on macOS, building
3. Flashing from macOS
4. Stage 0: the first hardware (Seed3 + headphone amp, or the Erica Synths Labor)
5. First power-on checklist (and what to paste back)
6. Driving it from the computer (stage 0's control protocol)
7. Memory map
8. Pin map
9. How the firmware runs (no operating system)
10. Stage 1: the breadboard prototype
11. Audio load (an estimate)
12. What is verified, what only the hardware can show
13. libDaisy facts relied on, and assumptions not verified
14. Seams in the core, risks, licences

## 1. Layout and choices

| Where | What |
|---|---|
| `daisy/Makefile` | The build: `make -C daisy` (stage 0) or `make -C daisy STAGE=1`. Uses `lib/pg` unchanged. |
| `daisy/pg_daisy.lds` | The linker script (BOOT_SRAM, derived from libDaisy's). |
| `daisy/libdaisy.lock` | The pinned libDaisy commit (and the BMI270 configuration file's source). |
| `daisy/core/` | Portable pieces: scheduler, serial demux, key script, load meter, LED, /int RAM file system with flash snapshots, sample pool mirror, screen mirror, pots, panel driver, BMI270, button, layout, pin map. |
| `daisy/app/` | The whole firmware above the hardware (`pgd::App` behind a `Hal`), shared by the device and the host simulation. |
| `daisy/src/` | The device: libDaisy glue (main, USB serial, QSPI, memory, stage 1 drivers). |
| `host/daisy/` | The host tests (`ctest -L daisy`) and `daisy_sim`. |
| `tools/daisy-setup.sh`, `tools/daisy-flash.sh`, `tools/pg-daisy`, `tools/daisy_size.py` | Fetch and build libDaisy, flash, drive the device, report the memory map. |

Choices made (the lead left them open):
- **libDaisy comes in through a fetch script, not a git submodule.** `tools/daisy-setup.sh`
  clones it into `daisy/libDaisy` (ignored by git) at the commit in `daisy/libdaisy.lock`
  (`1289a000bc29dae1bc0ed4129cfa2df40ef71574`, 2026-11-03). It also fetches the submodules that
  libDaisy needs (CMSIS, the H7 HAL, the USB device library; not googletest) and builds
  `libdaisy.a` with libDaisy's own Makefile. Why not a submodule: libDaisy has nested
  submodules, so every clone, worktree and CI run of this repository would need
  `--recursive`. The Cardputer and desktop builds have no use for it. A script keeps the pin
  in one file and the owner's setup to one command.
- **Make, not CMake**: libDaisy's own build and all its examples use Make, and macOS already
  has Make. The firmware Makefile is our own (it does not include libDaisy's
  `core/Makefile`). That way our sources build with the repository's warning flags and stay
  warning-free, libDaisy's headers are `-isystem`, `lib/pg` gets no libDaisy include paths,
  objects keep their paths (no name clashes) and code that only the main loop runs is built
  `-Os`.
- **48 kHz, stereo, 96-frame blocks.** `Instrument::init(48000, stereo)`, using
  `PG_MAX_SAMPLE_RATE`'s default. The engine writes interleaved int16, and the audio callback
  turns it into float with the output level. The block is 96 frames, which is 6 engine quanta
  or 2.0 ms. libDaisy double-buffers the SAI DMA, so the block written in a callback plays
  during the next one: output latency is 2.0 to 4.0 ms. Key to sound is at most about 5 ms
  (the main loop polls continuously, and commands apply at the next quantum edge). Compare
  48 frames: 1-2 ms latency, but a block's heavy quantum (an instrument evaluation every 32
  quanta, a note-on's coefficient work) takes a third of the block budget instead of a sixth,
  and any interrupt at the same priority costs twice as much of the margin. 64 frames is not a
  whole number of 16-frame quanta per ms. 96 keeps the latency far below the Cardputer's
  ~14.5 ms and doubles the tolerance to spikes.
- **Stage 0 starts the demo by itself** (the transport is posted Play once booted), so a
  freshly flashed board makes sound with nothing else connected. `>tap spc` stops it.
- **The four pots** move the selected track's four instrument macros, with pickup (a pot does
  nothing until it is turned past the macro's current value, so the sound never jumps). At
  boot the selected track is BASS (the demo's ACID: OPEN, BITE, ...). `p` on the serial port
  turns them off. Why this: it needs no UI, it is the instrument's own "four controls" design,
  and it is audible at once.

## 2. Setting up on macOS, building

Copy and paste, one line at a time:

```sh
xcode-select --install                         # git, make, python3 (skip if already there)
brew install --cask gcc-arm-embedded           # the Arm GNU toolchain (arm-none-eabi-gcc)
brew install dfu-util                          # USB flashing
python3 -m pip install --user pyserial         # for tools/pg-daisy and tools/pg (or use PlatformIO's Python)
```

Open a new terminal (so `arm-none-eabi-gcc` is on the PATH), then in the repository:

```sh
tools/daisy-setup.sh                           # once: libDaisy at the pinned commit, ~1 minute
make -C daisy                                  # stage 0 -> build/daisy/stage0/pocket-groove-daisy.bin
make -C daisy size                             # the memory map by component
```

`make` ends with a line like `daisy_size: image 440472 of 491520 bytes (49.9 KB free), ROM part
197056 bytes ... ok`, and the linker refuses a program that does not fit.

This was verified in the sandbox with arm-none-eabi-gcc 13.2.1 (Ubuntu's package) and
libDaisy 1289a00. The Homebrew cask installs the current Arm release (14.x). A newer GCC can
warn where 13.2 did not; if it does, paste the warning back.

## 3. Flashing from macOS

The Seed has two USB boot modes, and both show up as USB device `0483:df11`:
- **The STM32's own DFU** (in the chip's ROM): **hold BOOT, tap RESET, let go of BOOT.** It
  writes the 128 KB internal flash. You need it **once**, to install the Daisy bootloader.
- **The Daisy bootloader** (in the internal flash once installed): it runs at every power-on.
  For ~2.5 s the **LED pulses slowly**: that is its grace period, when it listens for DFU.
  Pressing BOOT during the pulsing makes it wait forever (a few fast blinks acknowledge). Then
  it copies the program from QSPI to RAM and starts it. If it rejects an image it **blinks
  SOS** once and carries on.

`tools/daisy-flash.sh` tells the two apart (it reads the memory layout that `dfu-util -l`
lists) and runs libDaisy's own commands (`core/Makefile`: `program-boot`, `program-dfu`).

```sh
tools/daisy-flash.sh bootloader    # once per board: hold BOOT, tap RESET, let go of BOOT, then this
```
What it runs: `dfu-util -a 0 -s 0x08000000:leave -D daisy/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin -d ,0483:df11`.
What you should see afterwards: at every power-on or RESET the LED pulses slowly for ~2.5 s. With
no program in QSPI yet, it keeps pulsing.

```sh
tools/daisy-flash.sh               # the firmware (stage 0); `tools/daisy-flash.sh 1` for stage 1
```
If the firmware is already running, the script asks it over serial to restart into the
bootloader (`B` then `y`). Otherwise it asks you to tap RESET, and it waits for the pulsing
bootloader. What it runs: `dfu-util -a 0 -s 0x90040000:leave -D build/daisy/stage0/pocket-groove-daisy.bin -d ,0483:df11`.
`dfu-util` may print `Error during download get_status` at the very end, after the `:leave`.
That is common with DfuSe devices and harmless if the board then starts.
What you should see: the LED goes steady (booting, a fraction of a second), then
**blinks slowly, 0.5 s on, 0.5 s off: the demo is playing.** A serial port
`/dev/cu.usbmodem...` appears (`ls /dev/cu.usbmodem*`).

The .bin is one file: the 480 KB RAM image, then the ROM part. The bootloader copies exactly
the first 480 KB to RAM and leaves the rest in QSPI, where the firmware finds it (section 7).
**Always flash the whole .bin.** A firmware built without its ROM part, or with an old one,
shows the LED's ROM-missing pattern and plays silent built-in sounds.

The board's LED, in this firmware (`daisy/core/led.h`):

| LED | Meaning |
|---|---|
| slow pulse, ~2.5 s, at power-on | the Daisy bootloader's grace period (not ours) |
| SOS once | the bootloader rejected the image (re-flash; check that it is the `.bin` from `build/daisy`) |
| steady | booting, or stuck before the main loop (see the checklist) |
| two short flashes every second | running, transport stopped |
| 0.5 s on, 0.5 s off | running, playing |
| fast blink (5 Hz) | the audio did not start |
| three short flashes, pause | the ROM part is missing: flash the whole `.bin` again |

A hard fault is recorded in a RAM area that survives a reset, and the board restarts. The next
report's `[crash]` line has the address and the `addr2line` command to decode it.

Without the script: `make -C daisy` builds the files; the two `dfu-util` lines above are all
there is to it.

## 4. Stage 0: the first hardware

What the owner will have first: a **Daisy Seed3** (the 65 MB version, with headers), and either
a **TPA6132 headphone amplifier board** or the **Erica Synths EDU DIY Labor** (a breadboard
platform with a built-in output amplifier, a headphone output and a line output), plus pots.
Stage 0 needs no other parts: the codec's line outputs make the sound, USB does everything else,
and the pots are optional.

### The Seed's pins for stage 0
(Header pin numbers as printed on the Seed's pinout, libDaisy `doc/Daisy_Seed_Rev4_Pinout.csv`;
the Seed3 is pin-compatible per libDaisy's `daisy_seed.h`.)

| Seed pin | Name | Connect to |
|---|---|---|
| 18 | AUDIO OUT L | the amplifier's left input |
| 19 | AUDIO OUT R | the amplifier's right input |
| 20 | AGND | the amplifier's ground, and the pots' low ends |
| 21 | 3V3A | the pots' high ends |
| 22, 23, 24, 25 | A0, A1, A2, A3 | the wipers of pots 1-4 (macros 1-4 of the selected track) |
| 40 | DGND | digital ground (whether the Seed joins it to AGND on the board is not verified): join every board's ground to the Seed's grounds |
| USB | | the Mac: power and everything else |

Pots: 10 k linear, one end to 3V3A (pin 21), the other end to AGND (pin 20), the wiper to the A
pin. **Never** wire a pot to anything but the Seed's 3.3 V and ground.

### With a TPA6132 board (the standalone option)
Seed pins 18 / 19 into the board's L / R inputs, pin 20 to its ground. Power the TPA6132 board
from the Seed's 3V3D (pin 38) and DGND (pin 40) **if its documentation says 3.3 V is fine**
(the TPA6132A2 chip runs from 1.62 to 3.6 V; check the board's own regulator or jumpers). It has
its own volume or gain setting: start low.

### On an Erica Synths Labor
Nothing about the Labor below comes from its manual: none of it is verified. **Check each point
on the Labor's silkscreen or in its manual before connecting.**

Safety rules, plainly:
1. **The Seed is a 3.3 V device.** No pin may see more than 3.3 V (or less than 0 V). The Labor's
   supply is Eurorack-style ±12 V. **Nothing from the ±12 V rails may touch any Seed pin.**
2. **Power the Seed from USB only.** Do not power it from the Labor. libDaisy's source says
   nothing about the Seed's VIN pin (39) or its range; Electrosmith's datasheet does, and it has
   not been read here. A +5 V rail on the Labor is not confirmed either. So: USB.
3. **Join the grounds**: the Seed's AGND (pin 20) to the Labor's ground. Without a common ground
   the audio signal has no reference. With the Mac on USB and the Labor on its own supply you
   may hear hum (a ground loop). That is a nuisance, not a danger: a battery-powered laptop
   avoids it.
4. **Pots between the Seed's 3V3A (pin 21) and AGND (pin 20) only, never to the Labor's ±12 V
   rails.** If the Labor's pot slots come pre-wired to its rails, do not use those slots for the
   Seed: use loose pots, or check the slots' wiring first.
5. **Never feed a Eurorack-level signal (up to ±10 V or more) into a GPIO, an ADC pin or the
   codec's inputs (pins 16 / 17)** without scaling it down to the 3.3 V range first. Stage 0
   uses no inputs.
6. **Audio out → the Labor's output amplifier input:** Seed pin 18 (L) and pin 19 (R) to the
   amplifier's input(s), pin 20 to ground. Whether the Seed's outputs are AC-coupled on the
   board is not verified. A 10 µF capacitor in series with each output (+ towards the Seed if it
   is polarised) is harmless and protects both sides. If the amplifier has one input, use L, or
   both through 1 k resistors. Turn its gain **down** before the first sound: the codec's line
   level is not known here and the Labor's amplifier gain is variable.

What to check on the Labor first: where its output amplifier's input is, and whether it expects
line level; whether its pot slots are wired to its rails; where its ground is; whether it has a
+5 V rail (only interesting later).

The **Labor Scope**'s hard-wired channel shows the Labor's audio output: use it to see a
waveform and a spectrum **before** trusting your ears or putting headphones on (section 5).

## 5. First power-on checklist

Do these in order. Stop at the first step that does not do what it says, and paste back what is
listed at the end.

1. **The Seed alone on USB** (nothing else connected). An unflashed Seed3 runs whatever it came
   with (Electrosmith usually ships a blink program).
2. **Install the Daisy bootloader**: `tools/daisy-flash.sh bootloader` (hold BOOT, tap RESET, let go
   of BOOT first). Expect: the LED pulses slowly; tapping RESET makes it pulse again for ~2.5 s.
3. **Flash the firmware**: `tools/daisy-flash.sh`. Expect: LED steady for a moment, then
   0.5 s on / 0.5 s off (playing).
4. **The serial port**: `ls /dev/cu.usbmodem*` shows one. `tools/pg-daisy report`
   prints a report. Check: `[board] Daisy Seed3 (TAC5242 codec)`, `running from AXI SRAM
   (BOOT_SRAM, as intended)`, `[audio] RUNNING, 48000 Hz, block 96 frames`, `AUD now` some
   percent, `underruns ... 0`, `[mem] SDRAM ... ROM copy ... (ok ...)`, `[crash] no fault`.
5. **Sound, measured first.** Connect the outputs (section 4), amplifier gain at minimum. On the
   **Labor Scope**, the hard-wired channel should show the demo: a busy waveform and, in its
   spectrum mode, energy from the low bass up to the hats. No signal there means nothing comes
   out of the Seed: check the wiring and the report's `[audio]` line before raising any gain.
   Then headphones, gain up slowly.
6. **Keys from the Mac**: `tools/pg-daisy tap spc` stops the demo (the LED goes to two flashes a
   second); again starts it. `tools/pg-daisy mirror` opens a window with the screen. Its keyboard
   plays the device as the desktop app's does (F1 = G0). Without tkinter: `tools/pg-daisy keys`.
7. **Pots** (if wired): turn pot 1 slowly across its range. Nothing happens until it passes the
   macro's value, then the bass's first macro follows it (a `[pots]` line in the report shows
   the positions and `engaged 1...`).
8. **Projects and samples**: `tools/pg ls` lists `_last` (the session autosave) once the UI has
   written it; `tools/pg push FILE.pgv` / `tools/pg pull NAME` work as on the Cardputer.
9. **Power-cycle** (unplug, plug). The session comes back (the report's `[fs] boot load: loaded`).

**Paste back if anything is off:** the output of `tools/pg-daisy report`; the boot log
(`tools/pg-daisy log`, then send `l`, or `screen /dev/cu.usbmodem* 115200`, type `l`); `dfu-util -l`
with the board in each mode; what the LED does; any `[crash]` line with the ELF it came from
(`build/daisy/stage0/pocket-groove-daisy.elf`).

If the LED stays **steady** and no serial port appears: the firmware stopped before its main
loop (SDRAM, QSPI or USB init). Re-flash once. If it persists, report it; the next step is a
debug build with a probe (the Seed3 has pads for an SWD header).

## 6. Driving it from the computer (stage 0's control protocol)

One USB serial port carries everything, as on the Cardputer:
- **Transfer lines** `~PGX ...`: `tools/pg`, the protocol in `lib/pg/src/pg/io/xfer.h`, unchanged.
  `tools/pg` finds the Daisy by its USB id (libDaisy's CDC device, `0483:5740`, "Daisy Seed Built
  In") as well as the Cardputer's.
- **Key lines** `>...`: the desktop app's key-script format (`host/app/script.h`, which is
  `host/tools/ui_shot`'s). It already existed, so no new protocol was made:
  `>down KEY`, `>up KEY`, `>tap KEY [ms]`, `>wait MS`, `>release`, `>shot NAME`, `>sync TAG`,
  `>expect-released`, `>mirror on|off`. KEY is a Cardputer legend (`` ` 1 .. = del tab q .. \ fn
  sft a .. ent ctl opt alt z .. / spc G0``) or an alias that `pg::ui::keyFromName` accepts. The
  keys go through the same path as physical keys (G0 handling included). `ui_shot`'s setup lines
  (`battery`, `sd`, ...) are skipped, so `host/ui_scripts/*.txt` run unchanged:
  `tools/pg-daisy script host/ui_scripts/firstboot.txt` (its `shot` lines save PNGs into `./shots`).
- **Letters**: `r` report, `h` help, `l` boot log, `f` one screen frame, `m` screen mirror on/off,
  `p` pots on/off, `S` save /int to flash now, `B` then `y` restart into the Daisy bootloader,
  `R` then `y` restart. A stats line comes every 5 s.
- **The screen** comes back as `~PGF` lines: a frame's begin, the palette, one line per
  framebuffer row (base64), the end (`daisy/core/fb_stream.h`). A full frame is ~44 KB; the
  mirror sends only the rows the UI changed. It costs nothing while off.

`tools/pg-daisy` wraps all of it (section 2 for pyserial). It can be tried without the board:
`cmake --build build/host --target daisy_sim && build/host/daisy/daisy_sim` prints a pseudo-terminal
path; then `tools/pg-daisy --port /dev/ttys... report` (and `tools/pg --port ...`). It runs the
firmware's own App code, with a simulated flash (`--flash FILE` keeps it between runs) and
`--autoplay` as the device does.

**Samples** over the same port already work: `tools/pg sample put / get / ls / rm` (the SPUT / SGET
/ SLS / SRM requests). The pool's image lives in SDRAM and is written there at once, with a copy
on the QSPI kept up to date in the background (section 7), so nothing else was needed. pg still
refuses a put while the transport plays (ERR PLAYING): a Cardputer rule (its flash writes stall
both cores) that the Daisy does not need; relaxing it is a core change, not made (section 14).

## 7. Memory map

From the real link (`make -C daisy size`, stage 0 at commit 515cf67 merged with the 48 kHz core):

| Region | Size | What is there | Used | Free |
|---|---|---|---|---|
| AXI SRAM 0x24000000 | 512 KB | the **image** the bootloader copies (vectors, code, constants, .data's initial values) | 430.2 KB of 480 | 49.8 KB |
| | | after the image, before the end (incl. the 32 KB the bootloader used while it ran) | 0 | 81.8 KB |
| DTCM 0x20000000 | 128 KB | `.data` 1.8 KB, `.bss` 104.6 KB: **the engine's state block (77.1 KB: delay, reverb, queue, sequencer)**, libDaisy's statics, the audio callback's block; then the **stack** | 103.8 KB | stack 24.2 KB |
| D2 SRAM 0x30000000 | 32 KB | libDaisy's audio DMA buffers (not cached) | 16.3 KB | 15.7 KB |
| D2 SRAM 0x30008000 | 256 KB | **the App**: Instrument (voices + Project, 71 KB), UI (19 KB), framebuffer (33 KB), transfer, file table | 128.6 KB | 127.4 KB |
| D3 SRAM | 64 KB | the fault record (kept over a reset) | 40 B | |
| ITCM | 64 KB | nothing yet (a tuning option: hot DSP code) | 0 | 64 KB |
| SDRAM 0xC0000000 | 64 MB | the ROM part's copy (192 KB); /int's files (48 x 96 KB), its snapshot staging (1 MB); the sample pool's image (3.2 MB); the serial rings (136 KB) | 9.0 MB | 55 MB |
| QSPI 0x90000000 | 8 MB | see below | | |

QSPI flash (`daisy/core/layout.h`):

| Address | Size | What |
|---|---|---|
| 0x90000000 | 256 KB | left alone (the bootloader's; "may be used in the future") |
| 0x90040000 | 480 KB | the image (copied to AXI SRAM at every boot) |
| 0x900B8000 | 288 KB | the ROM part: `PG_BULK_ATTR` data (ROM sounds 138 KB, instrument library 59 KB; stage 1 adds Bosch's 8 KB IMU file), 192 KB used; copied to SDRAM at boot |
| 0x90100000 | 2 x 1 MB | /int: two snapshot banks |
| 0x90300000 | 3.2 MB | the sample pool's copy (12 slots, pg's partition layout) |
| 0x90630000 | 1.8 MB | free |

A firmware update writes only the image and the ROM part, so /int and the pool survive it.

Per component (bytes; text and rodata are in the image, rom in the ROM part, bss anywhere;
strings that the linker merged across files are counted with the first file that has them):

```
  component             text    rodata       rom      data       bss
  daisy/src             6300      9398        32       201   9386584   (bss: the SDRAM buffers and the App)
  pg/sampler           17048       995    138102         0         0
  pg/engine            48696      1289         0         0     77116
  libDaisy             83692       365         0      1387     41248
  pg/ui                66240      3060         0         0         0   (-Os)
  pg/macro              5444         6     58900         0         4
  pg/lead              21220     40826         0         0         0   (40 KB: the wavetables, hot)
  pg/bass              28396      4876         0         0         0
  pg/demo               8506     16086         0         0         0   (-Os)
  pg/drums             22352      1414         0         0         0
  C/C++ runtime        18994      2517         0        92       740
  pg/io                12204       270         0         0         0   (-Os)
  daisy/core            8320        98         0         0         0
  daisy/app             7500        14         0         8         0
  total               355668     81214    197034      2364   9505720
```
Stage 1 (`STAGE=1`) adds libDaisy's SPI, I2C, SDMMC and FatFS, and the drivers. Its image is
466.7 KB of 480 (13.3 KB free); 32 KB of statics go after the image in AXI SRAM (the display's
strip buffers, FatFS), leaving 13 KB there.

How it fits, and what was decided: the program **is RAM-booted (BOOT_SRAM)**, so erasing and
writing the QSPI never stalls running code. The 480 KB limit was the constraint. With the big
cold data in QSPI (the core's `PG_BULK_ATTR`, defined by the Makefile as the `.qspi_rom`
section), and the main-loop-only code built `-Os`, stage 0 has 50 KB to spare and stage 1 has
13 KB, so BOOT_QSPI was not needed. The ROM part is not read in place: it is copied to SDRAM at
boot (~0.2 MB, a few ms). Nothing reads the QSPI afterwards, so the flash can be in its indirect
(write) mode while the audio plays. The next levers, if stage 1 grows: the UI's help text and
demo builders into the ROM part (a second section macro in the core, ~30 KB), the engine's
state into AXI SRAM to free DTCM, ITCM for code (64 KB).

The order things happen at boot: the bootloader copies the image, the startup code copies `.data`
and zeroes `.bss` (DTCM), and our `.preinit_array` hook (`daisy/src/memory.cpp`) sets the vector
table, enables the D2 SRAM clocks, zeroes `.axi_bss` and `.d2_bss` and paints the stack. After
that come the constructors (the App's), then `main`: `DaisySeed::Init` (480 MHz, SDRAM, QSPI,
codec), the ROM copy, USB, the App's boot (/int and the pool loaded from QSPI), and the audio
last. Nothing in SDRAM is constructed before `main`, because SDRAM works only after
`DaisySeed::Init`.

## 8. Pin map

Seed D numbers are libDaisy's (`seed::D0..D30`, `DaisySeed::GetPin`). All of them exist on the Seed3
(31 GPIO) and the Seed2 DFM; D31 / D32 (Seed2 DFM only) are not used. The table is
`daisy/core/pins.h`, and `test_daisy_core` checks it: no pin used twice, analog functions on ADC
pins, the peripherals where the chip routes them.

| Function | Seed | Header pin | STM32 | Stage | Note |
|---|---|---|---|---|---|
| POT1..POT4 (macros 1-4) | D15..D18 (A0..A3) | 22..25 | PC0, PA3, PB1, PA7 | 0 | ends to 3V3A / AGND |
| BATTERY | D19 (A4) | 26 | PA6 | 1 | LiPo through a 100k / 100k divider |
| SD D3, D2, D1, D0, CMD, CK | D1..D6 | 2..7 | PC11, PC10, PC9, PC8, PD2, PC12 | 1 | SDMMC1, 4-bit (Adafruit 4682) |
| LCD CS, SCK, DC, MOSI | D7, D8, D9, D10 | 8, 9, 10, 11 | PG10, PG11, PB4, PB5 | 1 | SPI1 (TX only); DC on the MISO pin as a GPIO |
| LCD BL | D27 | 34 | PG9 | 1 | GPIO, high = on (no PWM near the audio) |
| I2C SCL, SDA | D11, D12 | 12, 13 | PB8, PB9 | 1 | I2C1: BMI270 (Qwiic), touch later |
| TOUCH INT | D24 | 31 | PA1 | 1 | reserved |
| G0 BUTTON | D25 | 32 | PA0 | 1 | pull-up, to ground when pressed |
| AUDIO IN L / R | | 16 / 17 | codec | 1 | line in (the first recording path); later an analog MEMS capsule |
| AUDIO OUT L / R | | 18 / 19 | codec | 0 | line out |
| free | D0, D13, D14, D20..D23 (A5..A8), D26, D28 (A11) | | | | for the undecided pads (force sensors through a multiplexer) and buttons |

No key matrix is wired: the key layout is undecided (velocity-sensitive pads plus function buttons
are being considered). The input layer stays general: everything becomes events into the core
(`Hal::pollKey`, `Hal::pollPointer`).

## 9. How the firmware runs (no operating system)

libDaisy is bare metal, so there are two contexts:
- **The audio interrupt** (libDaisy's SAI DMA callback, every 2 ms): `App::render` runs the
  engine, then the int16 → float conversion with the output level. It is the only code that
  touches the engine.
- **The main loop**: `App::loopOnce` runs a cooperative scheduler (`daisy/core/scheduler.h`). Its
  tasks: `serial` (every pass: bytes in, transfer replies), `keys` (the key script, physical keys,
  tilt), `ui` (every 33 ms: `Ui::update`, the changed rows to the display and the mirror), `screen`
  (the mirror's lines as the transmit buffer has room), `store` (every 5 ms: one flash operation
  for /int's snapshot or the pool's copy), `led`, `stats`, `pots` (every 10 ms). Each task is a
  state machine that returns quickly; the report lists each one's run time. The engine's command
  queue is the only path from here to the audio.

The Cardputer's FreeRTOS tasks map onto these as follows: the key reader becomes `keys`, the IMU
task becomes a step in `keys` (100 Hz while G0 is held), the recorder's writer is not needed (a
recording lands in the SDRAM pool at once and is copied to flash by `store`), the SD mount
becomes a retry in the stage 1 service, and the UI loop becomes `ui`. Every piece except the
libDaisy glue is plain C++ tested on the host (`ctest -L daisy`).

/int is a RAM file system (`daisy/core/ramfs.h`) with the semantics pg::io relies on. It gets
written to flash as whole snapshots, alternating between two banks with CRCs
(`daisy/core/fs_persist.h`), 1.5 s after the last change. A power cut at any flash operation of
a save leaves the previous snapshot intact; `test_daisy_fs` cuts at every one. The sample pool
(`daisy/core/pool_mirror.h`) is pg's FlashPool and SampleWriter, unchanged, over an SDRAM image
with NOR rules. Its dirty 4 KB sectors are copied to QSPI one per step, each slot's header last.
At boot every slot's audio is checked against its header's CRC (pg's rescan checks the header
only), so a cut mid-copy leaves a slot old, new or empty, never mismatched (`test_daisy_pool`).

Interrupt priorities are libDaisy's: the SAI DMA, USB, SPI, I2C and SDMMC interrupts are all at
priority 0, so none preempts another. A USB or SPI-DMA interrupt can delay the audio callback by
its own length (microseconds), which a 2 ms block absorbs. The report's "late starts" counts
callbacks that began more than 1.5 periods after the previous one.

## 10. Stage 1: the breadboard prototype

`make -C daisy STAGE=1` links the drivers (`daisy/src/stage1.cpp`). It is compile-checked, and
the portable halves are host-tested. Nothing has run on hardware.
- **Display**: not chosen yet. The likely first one is a 3.5" 480x320 HX8357D (Adafruit 5846,
  with an FT5336 capacitive touch controller on I2C at 0x38); the 1.14" 240x135 ST7789 (Adafruit
  6113, the Cardputer's panel) is the small fallback. `daisy/core/panel.h` keeps the panel
  separate from the UI's 240x135 canvas: the controller, the panel's size, its RAM offsets and an
  integer scale are configuration. 480x320 shows the UI pixel-doubled at 480x270 with 25 rows
  spare above and below; 240x135 shows it at 1. The UI's dirty rows are what gets sent, as on the
  Cardputer. The strip expander is the Cardputer's (palette lookup into byte-swapped RGB565,
  double-buffered DMA), now a main-loop state machine. `test_daisy_panel` runs it against a
  simulated controller: the panel ends up with the Cardputer path's pixels exactly, at both
  scales. Default build: HX8357D (`-DPG_DAISY_PANEL_HX8357D=0` for the ST7789). Software reset
  (neither board has a reset pin wired). The SPI bit rate (prescaler 8) and the HX8357D's
  orientation byte are starting points, to be tuned on the panel. Touch: a `Hal::pollPointer`
  hook exists; the UI takes keys only, so nothing uses it yet.
- **Tilt**: a SparkFun BMI270 Qwiic board on I2C1 (D11 / D12), address 0x68 (0x69 with its jumper).
  `daisy/core/bmi270.h` probes it, soft-resets it and uploads Bosch's 8 KB configuration file in
  64-byte chunks across main-loop steps. It then waits for "init ok" and sets the Cardputer's
  register values. `test_daisy_bmi270` runs it against a register model. Readings feed
  `pg::tilt::Tracker` at 100 Hz while G0 is held, as on the Cardputer.
- **G0**: a debounced GPIO (D25) producing the same key events as the Cardputer's G0.
- **microSD** (Adafruit 4682, SDMMC, 4-bit, 25 MHz): libDaisy's SdmmcHandler and FatFS behind a
  `pg::FileSystem` (/sd). The card is mounted at boot, with a retry every 10 s. FatFs cannot
  replace a file on rename, so the target is removed first; pg::io's power-loss recovery covers
  that gap, as it does for the Cardputer's FAT card.
- **Recording**: the first path is line in through the codec's stereo input (pins 16 / 17); an
  analog MEMS capsule with fixed gain is to be chosen. **Not built yet.** The audio callback
  already hands its input block to `stage1::audioInput`. What is missing is the ring it would
  fill, plus a `pg::sampler::MicInput` over pg's Recorder (48 kHz in, `recRatesFor(48000, ...)`).
- **Battery**: A4 through a divider. **Pots** stay as in stage 0.

## 11. Audio load (an ESTIMATE, not a measurement)

From the two measured or counted figures in the repository:
- **The Cardputer, measured on hardware**: AUD 41-50 % of one 240 MHz ESP32-S3 core at 44.1 kHz,
  stereo (the USB audio build; jack-only mono ~30 %). That is roughly 108 M cycles/s, ~2450
  cycles per frame. At 48 kHz the same work is 8.8 % more per second (per-frame cost is
  unchanged, as measured by the 48 kHz core work: callgrind 2130 x86 instructions per frame for
  the busy pattern in stereo either way).
- **The x86 instruction counts** (callgrind, `docs/STATE.md` "48 KHZ CORE BUILT"): busy stereo
  2130 instructions per frame, the demos 838-1093. At 1.0-1.6 Cortex-M7 cycles per x86
  instruction and 480 MHz: busy 21-34 % of the core, the demos 8-18 %.
- Scaling the Cardputer's measurement instead: an M7 needing 0.5-1.0x the LX7's cycles per frame
  (dual issue, a full single-precision FPU, code in RAM instead of a flash cache) gives
  **12-25 %** for what the Cardputer measured.

**Estimate: 10-35 % of the core, 0.2-0.7 ms of each 2 ms block**, depending on how busy the
pattern is. The report's `AUD now / peak` and `longest block` will replace this number.

What could make it worse:
- **Code placement and the caches.** The audio path's code (engine 49 KB, lead 21, bass 28, drums
  22, sampler 17 KB) runs from AXI SRAM through a 16 KB instruction cache. Its working set per
  quantum is larger than that, so misses go to AXI SRAM (fast, but not zero wait). ITCM (64 KB,
  unused) is the remedy for the hottest voices if the report shows it.
- **Data.** The engine's state is in DTCM (no cache needed). The voices and the Project are in D2
  SRAM, which is cached but further away than AXI. The lead wavetables are in AXI (40 KB against
  a 16 KB data cache). The ROM sounds and the pool are in SDRAM (cached; misses cost tens of
  cycles, but reads are mostly sequential).
- **One core.** The UI, the display's strip expansion (a full 480x270 frame is ~130 k pixels), USB
  and the flash writes all run on the same core as the audio. The audio interrupt preempts them,
  so they take what the audio leaves: a heavy audio load slows the UI, not the reverse. The
  exception is interrupts at the audio's priority (USB, SPI DMA), which add their own length to a
  callback's start.
- Denormals: the FX tails decaying in silence produced subnormal floats on the host. The M7's FPU
  handles them in hardware, and libDaisy's system code does not touch FPSCR (no flush-to-zero
  set there); the cost on the M7 is unmeasured.

## 12. What is verified, what only the hardware can show

Verified here (sandbox, no hardware):
- The firmware compiles and links for the STM32H750 with arm-none-eabi-gcc 13.2.1, stages 0 and 1,
  **with no warnings in our sources** (libDaisy's are its own; none appear).
- The image fits the bootloader's 480 KB (the linker asserts it). The `.bin`'s vector table
  points into AXI SRAM, its stack into DTCM, and its ROM part sits at the QSPI address the
  firmware reads (`tools/daisy_size.py --check`, run by `make`).
- **The bootloader's copy size**, from its own binary (`core/dsy_bootloader_v6_4-intdfu-2000ms.bin`,
  disassembled): it checks the reset vector's region, and for SRAM it copies exactly 0x78000 bytes
  from 0x90040000 to 0x24000000. That is why the ROM part can follow the image in one file.
- Its DFU memory map, also from the binary: `@Flash /0x90000000/64*4Kg/0x90040000/60*64Kg/0x90400000/60*64Kg`
  (the whole QSPI is writable over DFU).
- On the host: everything in section 9, the display path, the BMI270 driver, and the App itself
  (`tools/test_pg_daisy.py` against `daisy_sim`: report, keys, screen, scripts, project and
  sample transfer, /int and the pool surviving a restart).
- The Cardputer firmware and the host build are untouched and pass (`docs/STATE.md`).

Only the hardware can show: that the board boots (clocks, SDRAM, QSPI init in BOOT_SRAM), that
the TAC5242 plays with libDaisy's reset-only codec setup, the real audio load, USB CDC on a Mac
(enumeration, throughput, no lost bytes), QSPI erase/write timing and its effect on the main
loop, the ADC's pots (noise, range), the LED patterns, stage 1's panel, card, IMU and timing,
and any interrupt-priority effects.

## 13. libDaisy facts relied on, and assumptions not verified

Read in libDaisy's source at 1289a00:
- `DaisySeed::Init(true)` = 480 MHz (VOS0); it initialises SDRAM and QSPI (memory-mapped) when
  not running from QSPI, and the codec by board version (`CheckBoardVersion`: PD3 low = Seed 1.1
  WM8731, PD4 = Seed2 DFM PCM3060, PD5 = Seed 1.2, PH6 = Seed3; otherwise rev4). **The Seed3's
  TAC5242 gets the same treatment as the AK4556 (a reset-pin toggle, no I2C setup)**, per its
  comment. Rates (SaiHandle): 8, 16, 32, 48, 96, 192 kHz: no 44.1. Block size at most 256 frames.
- The audio callback runs in the DMA interrupt. The interleaved callback's `size` counts samples
  (frames x 2). libDaisy converts float ↔ 24-bit itself.
- USB: `UsbHandle` is a CDC device only (VID 0x0483, PID 0x5740, "Daisy Seed Built In").
  `CDC_Transmit_FS` returns BUSY while the previous packet is in flight, and **dereferences
  `pClassData` without a check**, so before enumeration it would read from and write to ITCM at
  address 0. `usb_serial.cpp` transmits only once the device is configured. `CDC_Receive_FS`
  calls the receive callback with no null check, so the callback is set right after `Init`.
- QSPI: `EraseSector` and `Write` return to memory-mapped mode; `Erase` with 64 KB blocks does
  **not**. `qspi_flash.cpp` re-initialises the mapping before a read that follows a block erase.
  Both refuse to run while the program executes from QSPI. Memory-mapped QSPI is cached:
  reads invalidate the D-cache first.
- The startup code (`core/startup_stm32h750xx.c`, compiled with `BOOT_APP`) zeroes only
  `.bss` and skips `SystemInit`. Anything else must be zeroed by us, and SDRAM objects must not
  have constructors.
- `HardFault_Handler` stops at a breakpoint (a lockup without a debugger), so ours replaces the
  fault vectors in the RAM vector table.
- SPI DMA does no cache maintenance; I2C takes 7-bit addresses; `FatFSInterface` uses "0:/".
- libdaisy.a is built **without** `-fshort-enums`, while libDaisy's `core/Makefile` builds apps
  **with** it. Plain enums in libDaisy's config structs (e.g. `QSPIHandle::Config`) then differ
  in size between an app and the library. This build leaves `-fshort-enums` out to match the
  library.

Not verifiable from the source (assumptions):
- That the bootloader enables the D2 SRAM clocks (ours enables them again anyway) and sets VTOR
  (ours sets it again).
- That a fresh Seed3 is detected as `DAISY_SEED_3` (PH6 low) and that the TAC5242's hardware
  default is I2S, 24-bit, matching SAI1's configuration.
- The Seed's VIN range, its 3V3D current budget, whether its audio outputs are AC-coupled, its
  line-out level.
- That the STM32 DFU's alternate setting names contain "Internal Flash" (the script relies on it
  to tell the modes apart; the Daisy bootloader's contains "0x90000000", which was read in its
  binary).
- RAM D3 is not touched by the bootloader between runs (the fault record lives there; it has a
  magic and a check word, so garbage is ignored).
- The ST7789 offsets on the 6113 (assumed the Cardputer's 40 / 53), the HX8357D's MADCTL for
  landscape, and the SPI1 kernel clock (and so the real bit rate).
- That the Daisy's CDC enumerates on macOS without a driver. It is a standard CDC ACM device,
  which macOS supports natively.

## 14. Seams in the core, risks, licences

Seams in the core (none were needed for this work; these would help later):
- **A float output from the engine** (`Engine::process(float*, ...)`). The engine's output is
  int16, so the 24-bit codec gets 16-bit audio (with dither or truncation as the master does). A
  float path would also save the conversion.
- **A second section macro for the UI's and demos' constant text** (help text 12 KB, demo
  builders' data 16 KB), to move them into the ROM part if stage 1 needs room.
- **XferEnv: "flash writes do not stall"**. Samples and projects could then be put while the
  transport plays on a platform where that is true (the Daisy); today pg refuses with ERR PLAYING.
- **Pointer events in the UI** for a touch panel (the Hal has the hook).

Risks for the first power-on, most likely first:
1. **The firmware does not get as far as USB** (LED steady, no serial port). Causes:
   something in the boot path that the host cannot exercise, such as the D2/AXI zeroing in
   preinit, the SDRAM/QSPI init in BOOT_SRAM, or the ROM copy. The report would not even come
   up. The fault record helps if it faults; a hang needs an SWD probe.
2. **USB CDC behaving differently from the plan** (output lost at boot, the port not
   appearing until a terminal opens, a stuck first packet). The boot log replay (`l`) and the
   TX ring are there for this.
3. **No or distorted sound from the Seed3's TAC5242**: libDaisy treats it like the AK4556. The
   wiring into the amplifier (levels, ground) is the more likely cause.
4. **Audio load higher than estimated** (caches, D2 latency): underruns in the report. Remedies:
   ITCM for hot code, engine state stays in DTCM, voices into AXI.
5. **QSPI writes misbehaving** (a block erase leaving the mapping off, timing). This shows up
   as `[qspi]` failures or a /int that does not survive a power cycle. The snapshots and the CRCs
   keep the previous state safe.
6. **The Labor wiring**: the wrong pin into the amplifier, a missing common ground, or a pot
   slot pre-wired to a rail. Section 4's rules and checking the silkscreen first cover this.
7. **dfu-util's DFU mode detection** in the script: the libDaisy commands work without it.

Licences: libDaisy (MIT) is fetched, not copied; the linker script is derived from libDaisy's
VisualGDB-generated script (BSD). Bosch's BMI270 configuration file (BSD-3-Clause) is extracted
by `tools/daisy-setup.sh` into `daisy/vendor/` with its notice (`daisy/vendor/BMI270_LICENSE`).
A distributed firmware binary that contains it must carry that notice. It is the same 8192
bytes M5Unified uploads on the Cardputer, compared byte for byte.
