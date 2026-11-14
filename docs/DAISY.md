# Pocket Groove on the Daisy Seed

The target device is built around an Electrosmith Daisy Seed: an STM32H750 (Cortex-M7, 480 MHz),
64 MB SDRAM, 8 MB QSPI flash and a stereo codec. This document covers the firmware for it in
`daisy/`, how to build and flash it from a Mac, what to wire, and what has been checked so far.
Read the first-power-on checklist before plugging anything in.

**Status (2026-11-14): nothing has run on a Seed yet.** The firmware compiles and links for the
STM32H750 without warnings, in both stages. Everything portable runs on the host: the scheduler,
the serial protocol, /int, the sample pool, the display path (now the Adafruit 2090's ILI9341),
the BMI270 and FT6206 drivers on their shared I2C bus, the force-sensor pad scanner and the
line-in recorder are unit-tested, the pads play through the core's pad API (velocity, pressure,
the drums / notes job) and can be hit from the computer without sensors (`tools/pg-daisy pad`), and the firmware's whole `App` runs in `daisy_sim` and in
`test_daisy_stage1` against the real computer tools. Only the hardware can show the rest (see
"What only the hardware can show").

**Stage 1 parts (owner's order, 2026-11-14):** Daisy Seed3 65 MB with headers on the Erica
Synths Labor; the **Adafruit 2090** screen (2.8" 320x240 ILI9341 over SPI, FT6206 capacitive
touch on I2C at 0x38, EYESPI revision; its microSD slot is not used); an **Adafruit 4682**
microSD breakout on SDMMC; **four Adafruit 1075** square force sensors (Alpha MF02A-N-221-A01),
each with a 10 k resistor; the **SparkFun BMI270** Qwiic board; no microphone yet: **the first
recording path is the codec's stereo line input** (section 10).

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
10. Stage 1: the breadboard prototype (display, touch, pads, line in: parts, wiring, algorithms)
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
| `daisy/core/` | Portable pieces: scheduler, serial demux, key script, load meter, LED, /int RAM file system with flash snapshots, sample pool mirror, screen mirror, pots, panel driver (ILI9341 / ST7789 / HX8357D), I2C bus guard, BMI270, FT6206 touch, force-sensor pads, line-in recorder, Daisy settings file, button, layout, pin map. |
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

`make` ends with a line like `daisy_size: image 431612 of 491520 bytes (58.5 KB free), ROM part
197056 bytes ... ok`, and the linker refuses a program that does not fit. Stage 1:
`make -C daisy STAGE=1` (another panel: `PANEL=1` HX8357D 480x320, `PANEL=2` ST7789 240x135;
run `make -C daisy STAGE=1 clean` when you change PANEL).

**libDaisy is built for size (`-Os`)** since 2026-11-14 (its own Makefile says `-O3`): stage 1
did not fit otherwise. If you ran `tools/daisy-setup.sh` before that, `make` stops with "libDaisy
was built by an older tools/daisy-setup.sh": run `tools/daisy-setup.sh` once more (it rebuilds
the library, about a minute).

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
   codec's inputs (pins 16 / 17)** without scaling it down first. The codec's inputs take line
   level (about 1 Vrms at most, section 10 "Line in"); the ADC pins take 0..3.3 V. The Labor's
   Eurorack-level outputs must never go straight into either.
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

### Stage 1, one part at a time

Do stage 0 first. Then **unplug USB before every wiring change**, add one part, flash stage 1
(`make -C daisy STAGE=1`, `tools/daisy-flash.sh 1`) and check it before the next part. Wiring is
in section 10. Each step: what you should see, and what to paste back if you do not.

10. **Stage 1 with nothing new wired.** Expect: as stage 0 but the demo does not start by itself
    (`tools/pg-daisy tap spc` starts it). The report has `[display] ILI9341 320x240, the UI at
    scale 1 at (40,52), MADCTL 0x28, SPI 12.50 MHz`, `[i2c] ... the bus free` (or held, with
    nothing on it: that is expected without pull-ups), `[touch] FT6206 NOT FOUND at 0x38`,
    `[imu] BMI270 not found`, `[pads] ...` four lines, `[linein] codec line in, 48000 Hz`,
    `[fpu] ... flush-to-zero on, default NaN on`. **Paste back:** the whole report.
11. **The 2090 display** (SPI pins, Lite, power; **its IM1, IM2, IM3 jumpers soldered closed**, section
    10). Expect: at power-on the backlight comes on, the panel goes black for about half a
    second, then the Pocket Groove screen appears in the middle with a black border (40 pixels
    left and right, 52 / 53 above and below). Upside down: `tools/pg-daisy cmd display turn`
    (saved). Garbage or stripes: `tools/pg-daisy cmd display spi 4` (6.25 MHz), then 8. White
    screen: no data reaches it (the IM jumpers, MOSI, CLK, D/C, CS). Colours swapped red/blue:
    report it (MADCTL's BGR bit). **Paste back:** the `[display]` line, a photo, what you tried.
12. **Touch** (SDA, SCL, IRQ of the 2090; the display already powered). Expect: `[touch] FT6206
    present (chip id 0x06 ...)`, `IRQ high (no touch)`; touching the screen and sending `r`:
    `touches 1 ...`, `IRQ LOW` while a finger is on it, `last canvas (x,y) raw (x,y)`. Touch near
    the top-left corner of the UI: canvas near (0,0); bottom right: near (239,134).
    `tools/pg-daisy mirror` shows a red dot where you touch. Mirrored or swapped: try
    `tools/pg-daisy cmd touch map N` for N = 0..7 until the dot follows the finger (saved;
    `touch map auto` undoes it), and tell me which N. **Paste back:** the `[touch]` and `[i2c]`
    lines, and the raw coordinates at the four corners of the panel.
13. **The BMI270** (Qwiic lead into the same SDA / SCL). Expect: `[imu] BMI270 ready @0x68`, the
    touch still `present`, `[i2c] ... failed 0` growing only by the probes of absent devices.
    Hold G0 (if wired) and tilt: as on the Cardputer. **Paste back:** `[imu]`, `[i2c]`, `[touch]`.
14. **One force sensor** (pad 1, with its 10 k resistor; section 10's diagram). Then
    `tools/pg-daisy pads`: at rest the pad's `uS` reads below 1; pressing it lightly gives tens,
    hard a few hundred to over a thousand. Tap it: the kick plays (louder for a harder tap) and
    `vel` shows the velocity and the note velocity it makes (`0.79=101`). Ctrl-C, then wire the
    other three (snare, closed hat, perc: the drums job) and check each the same way. Then tune the
    velocity (section 10, "Tuning the velocity"): hard hits should read 1.00, medium ones about
    0.79. Other tuning, while `pads` runs in one terminal and `cmd` in another: a pad that triggers
    too easily, `cmd pad N thr 20`; one that needs too much force, `cmd pad N thr 5`; a hard hit
    also playing the pad next to it, `cmd pads xtalk 0.5`. All saved at once. Squeeze a held pad
    on PERC with TAP on it: the pitch bends (`press`); `tools/pg-daisy cmd pads job notes` and the
    pads play the selected track up the scale. **Paste back:** about ten seconds of
    `tools/pg-daisy pads` output with one soft, one medium and one hard tap and one slow press on
    each pad, and the `cmd pads` lines.
15. **microSD** (Adafruit 4682). Expect: `[sd] mounted (FatFS result 0)`, `tools/pg ls` lists
    `/sd` too. **Paste back:** the `[sd]` line.
16. **Line in** (a phone or a computer's headphone output, volume at about half, section 10).
    Put a sampler instrument on PERC (on the screen or: `tools/pg-daisy tap ...` as in
    `host/ui_scripts/mic.txt`), hold L (`tools/pg-daisy down l`), play something, let go (`up
    l`). Expect: the waveform grows on the screen while L is held, a toast `SLOT 1 REC01 ...`, the
    sample plays from PERC, at the right pitch. The report: `[linein] last: slot 1 DONE (commit
    OK) ... at 48000 Hz`, `mono L alone (R silent)` with a mono cable or `(L+R)/2` with a stereo
    one, input peaks between about -20 and -3 dBFS. Peaks at 0 dBFS: turn the source down. Then
    power-cycle: the sample is still there. **Paste back:** the `[linein]` lines and
    `[samples]`.

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
- **Touches** (stage 1) come back while the mirror is on as `~PGT D X Y` (down or moved) and
  `~PGT U X Y` (lifted), in UI canvas coordinates (outside 0..239 x 0..134 on the border).
  `tools/pg-daisy mirror` draws a red dot.
- **Stage 1 commands** (key-script lines too; `daisy/app/app_settings.cpp`), each change saved at
  once to `/int/pg/daisy.cfg` and printed back:
  `>pads` (the [pads] lines: the job, what each pad plays and what the engine hears of its
  macros, then the sensors), `>pads watch [MS|off]`, `>pads job [drums|notes]` (alone: the other
  job), `>pads window N`, `>pads release N`, `>pads hyst R`, `>pads xtalk R [MS]`, `>pads reset`,
  `>pad N|all [thr US] [ceil US] [curve X] [on|off]`,
  `>hit N VEL [hold MS] [press P]`, `>hit N press P`, `>hit N up` (a pad from the computer),
  `>touch X Y [MS]` (a touch from the computer), `>touch map N|auto`,
  `>display turn`, `>display spi 2|4|8|16`. One key more than the Cardputer's in `>down` /
  `>up` / `>tap`: `padjob` (or `F4`, the desktop app's key for it), the pads' job.
  `tools/pg-daisy pads` watches the pads; `tools/pg-daisy pad N VEL [hold MS] [press P]` hits one
  from the computer; `tools/pg-daisy cmd WORDS` sends one command (`tools/pg-daisy cmd pad 2 thr
  15`).

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
| SDRAM 0xC0000000 | 64 MB | the ROM part's copy (192 KB); /int's files (48 x 96 KB), its snapshot staging (1 MB); the sample pool's image (3.2 MB); the serial rings (136 KB); the line input's ring (128 KB, 1.37 s) | 9.1 MB | 55 MB |
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
(The table above is from stage 0 at 515cf67, before stage 1's work.) **Measured on the
2026-11-14 stage 1 commit (`make`'s daisy_size line):** stage 0 image **431,612 bytes, 58.5 KB
free**; stage 1 image **463,108 bytes, 27.7 KB free** (ROM part 205,248 bytes). Stage 1 adds
libDaisy's SPI, I2C, SDMMC and FatFS, the drivers, the pads, the line-in recorder and their
commands. **After the pads moved onto the core's pad API** (the velocity and pressure branch's
UI and engine code now linked in, `>hit`, the [pads] play lines, the output peak): stage 0
**436,748 bytes, 53.5 KB free**, stage 1 **468,232 bytes, 22.7 KB free** (measured, `make`'s
daisy_size line). It first came out ~3 KB over the 480 KB; what was done about it:
- **libDaisy built `-Os`** (section 2): about 20 KB in both stages. libDaisy is the HAL, USB,
  FatFS: main-loop code; its audio-path part is the per-block float conversion (~400 samples).
- **The Daisy's own main-loop code `-Os`** (`daisy/app`, and `daisy/core` and `daisy/src` except
  the pads' scan, the line input's fold-down, the panel's strip expander, main.cpp's callback
  and the USB serial), and **pg's recorder** (on the Daisy it runs from the UI frame): ~6 KB.
- **The display's two strip buffers (30 KB) moved from AXI SRAM to D2 SRAM** (`.d2_bss`: DMA1/2
  reach it, it is cached like AXI SRAM and cleaned the same way), because the AXI SRAM after
  a near-full image no longer had room for them. D2 SRAM: 166 KB of statics, 90 KB free.
Nothing was dropped. The next levers are the same as before (a second section macro in the core
for the UI's help text and the demos, ~30 KB; ITCM).

How it fits, and what was decided: the program **is RAM-booted (BOOT_SRAM)**, so erasing and
writing the QSPI never stalls running code. The 480 KB limit was the constraint. With the big
cold data in QSPI (the core's `PG_BULK_ATTR`, defined by the Makefile as the `.qspi_rom`
section), and the main-loop-only code built `-Os`, stage 0 has 58 KB to spare and stage 1 has
28 KB, so BOOT_QSPI was not needed. The ROM part is not read in place: it is copied to SDRAM at
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
| LCD CS, SCK, DC, MOSI | D7, D8, D9, D10 | 8, 9, 10, 11 | PG10, PG11, PB4, PB5 | 1 | the 2090: SPI1 (TX only); DC on the MISO pin as a GPIO |
| LCD BL | D27 | 34 | PG9 | 1 | the 2090's Lite: GPIO, high = on (no PWM near the audio) |
| I2C SCL, SDA | D11, D12 | 12, 13 | PB8, PB9 | 1 | I2C1, 400 kHz: BMI270 (Qwiic, 0x68) and the 2090's FT6206 (0x38) |
| TOUCH INT | D24 | 31 | PA1 | 1 | the 2090's IRQ: input with pull-up, only reported (polling works without it) |
| G0 BUTTON | D25 | 32 | PA0 | 1 | pull-up, to ground when pressed |
| **PAD1..PAD4** (drums job: kick, snare, closed hat, perc) | **D20..D23 (A5..A8)** | **27..30** | PC1, PC4, PA5, PA4 | 1 | force sensors, each a divider with 10 k to AGND; ADC1 INP11, 4, 19, 18; sampled 32.5 cycles |
| AUDIO IN L / R | | 16 / 17 | codec | 0 and 1 | line in: the first recording path (both stages); later an analog MEMS capsule |
| AUDIO OUT L / R | | 18 / 19 | codec | 0 | line out |
| free | D0, D13, D14 (I2C4), D26, D28 (A11) | 1, 14, 15, 33, 35 | | | more pads through a multiplexer, function buttons |

Why A5..A8 for the pads: they are the four ADC pins left (A9 / A10 are the touch IRQ and G0, A11
stays free for a multiplexer's common pin), all four on ADC1 (libDaisy's AdcHandle reads ADC1
only; its pin table is checked by `test_daisy_core`), and next to each other on the header (pins
27-30), beside 3V3A / AGND (pins 21 / 20). A7 / A8 (PA5 / PA4) are also the DAC's outputs:
the firmware does not start the DAC.

No key matrix is wired: the key layout is undecided (velocity-sensitive pads plus function buttons
are being considered). The input layer stays general: everything becomes events into the core
(`Hal::pollKey`, `Hal::pollPointer`, the pads' events).

## 9. How the firmware runs (no operating system)

libDaisy is bare metal, so there are two contexts:
- **The audio interrupt** (libDaisy's SAI DMA callback, every 2 ms): `App::render` scans the
  pads (four ADC values, a few microseconds), folds the codec's input block into the line-in
  ring while a recording is armed, runs the engine, then the int16 → float conversion with the
  output level. It is the only code that touches the engine. It also checks that
  flush-to-zero / default-NaN are set in its FPSCR (section 11).
- **The main loop**: `App::loopOnce` runs a cooperative scheduler (`daisy/core/scheduler.h`). Its
  tasks: `serial` (every pass: bytes in, transfer replies), `keys` (the key script, physical keys,
  tilt), `ui` (every 33 ms: `Ui::update`, the changed rows to the display and the mirror), `screen`
  (the mirror's lines as the transmit buffer has room), `store` (every 5 ms: one flash operation
  for /int's snapshot or the pool's copy; the pool's copy waits while a recording runs), `led`,
  `stats`, `pots` (every 10 ms), `pads` (every pass: the pads' events into the UI's pad API). Each task is a
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
the portable halves are host-tested (`ctest -L daisy`: `test_daisy_panel`, `test_daisy_touch`,
`test_daisy_pads`, `test_daisy_linein`, `test_daisy_bmi270`, and `test_daisy_stage1`, which runs
the whole App with simulated pads, line input and touches). Nothing has run on hardware.

**Wiring rules for every part below.** Unplug USB before changing wires. Everything runs from the
Seed's 3.3 V (3V3D pin 38 for the boards, 3V3A pin 21 for the force sensors) and its grounds
(DGND pin 40, AGND pin 20). **Nothing from the Labor's +-12 V rails touches any of it.** Join
every board's GND to the Seed's DGND.

### Display: Adafruit 2090 (2.8" 320x240, ILI9341, SPI)

`daisy/core/panel.h`: `PanelConfig::ili9341_320x240()`, the stage 1 default (`PANEL=0`). The
UI's 240x135 canvas is shown at scale 1, **centred: x 40..279, y 52..186**; `begin()` clears the
whole panel to black once, so the border stays black, and after that only the rows the UI
reports dirty are sent (as on the Cardputer: palette lookup into byte-swapped RGB565 strips,
double-buffered DMA). The 320x240 redraw of the UI is not part of this work.

The setup is Adafruit_ILI9341 1.7.0's (`initcmd[]`, fetched for reference 2026-11-14): software
reset and 150 ms (the reset pin is not wired), its power / VCOM / frame rate / gamma table,
then MADCTL, 16-bit pixels, sleep-out and 150 ms, display on and 150 ms. Orientation, from its
`setRotation`: **MADCTL 0x28 = MV | BGR (its rotation 1, landscape)**, or **0xE8 = MY | MX | MV
| BGR (rotation 3, turned over)**: `tools/pg-daisy cmd display turn` switches and saves it.
Colour order: BGR, as Adafruit sets it for every rotation (the panel's filters are BGR, so our
big-endian RGB565 shows the right colours with the BGR bit set). No inversion (unlike the
Cardputer's IPS panel). SPI1's clock is 25 MHz (PLL2P, libDaisy's system.cpp), so the default
prescaler 2 gives **12.5 MHz**: a full UI frame (240x135x2 bytes) is ~41 ms on the wire, a few
changed rows well under 1 ms. The ILI9341's datasheet rates writes at 10 MHz (100 ns cycle);
Adafruit's drivers run it at 24-40 MHz. On long breadboard wires: `cmd display spi 4` (6.25 MHz)
or 8.

**Wiring, the 2090's SPI-side header to the Seed:**

| 2090 pin | to | Seed pin | Note |
|---|---|---|---|
| Vin | 3V3D | 38 | the board's regulator takes 3-5 V; at 3.3 V in it runs just below its regulation (the ILI9341 works at that) |
| 3Vo | **leave unconnected** | | the board's own 3.3 V output |
| GND | DGND | 40 | |
| CLK | D8 (SPI1 SCK) | 9 | |
| MOSI | D10 (SPI1 MOSI) | 11 | |
| MISO | **leave unconnected** | | D9 (the MISO pin) is the D/C output: never wire the 2090's MISO to it |
| CS | D7 | 8 | held low (the only device on the bus) |
| D/C | D9 | 10 | |
| RST | **leave unconnected** | | the board resets itself at power-on; the firmware sends a software reset |
| Lite | D27 | 34 | high = backlight on (the board pulls it up anyway) |
| SDCS / CCS (the card's CS) | **leave unconnected** | | its microSD slot is not used (the Adafruit 4682 on SDMMC is) |
| Y+, X+, Y-, X- | **leave unconnected** | | resistive-touch pins, unused on the capacitive board |
| SCL (touch) | D11 (I2C1 SCL) | 12 | shared with the BMI270 |
| SDA (touch) | D12 (I2C1 SDA) | 13 | shared with the BMI270 |
| IRQ (touch) | D24 | 31 | optional: reported only, polling works without it |
| D0-D7, RD, WR, IM0-IM3 (the 8-bit side) | **leave unconnected** | | |
| the EYESPI connector | not used | | same signals as the headers |

**Before wiring: SPI mode.** The v2 breakouts ship in 8-bit parallel mode; SPI mode is set with
the IM1 / IM2 / IM3 solder jumpers on the back (solder them closed). This is from memory of
Adafruit's guide, which this sandbox could not reach: **check the guide's "Pinouts" / "SPI
Wiring" page and the silkscreen** before soldering, and check where the capacitive touch's SDA /
SCL / IRQ pins are on your revision (the EYESPI revision keeps the same headers).

### Touch: the 2090's FT6206 (I2C1, 0x38)

`daisy/core/touch.h`. Probe: vendor id (0xA8) 0x11 and chip id (0xA3) 0x06 / 0x36 / 0x64, as
Adafruit_FT6206 1.1.1 checks; then the touch threshold (0x80) is set to 40, the value Adafruit's
examples for this board use. Every 16 ms (~60 Hz) one 15-byte read of 0x00..0x0E (~0.4 ms at
400 kHz) is parsed as Adafruit's `readData` does (TD_STATUS, per point XH/XL/YH/YL, the event
flag and the touch id); the first point becomes down / move / up events. No answer: `NOT FOUND`,
probed again every 2 s (a NAK costs microseconds). Three failed reads: `LOST`, an up event, probed
again. The IRQ pin (low while touched) is only shown in the report.

Coordinates: the FT6206 reports in the panel's native portrait frame (x 0..239, y 0..319). At
rotation 1 (MADCTL 0x28) the landscape panel point is **x = 319 - ty, y = tx**, as Adafruit's
CapTouch_onoffbutton example maps it at rotation 1 (`x = 320 - p.y, y = p.x` after its flips);
at 0xE8, x = ty, y = 239 - tx. Then the canvas: (x - 40, y - 52) at scale 1; a touch on the
border lands outside 0..239 x 0..134. `Hal::Pointer` carries the canvas point and the raw one.
If the owner finds the axes wrong, `cmd touch map N` picks one of the eight orientations (bit 0
swap, bit 1 mirror x, bit 2 mirror y; 3 is the default, 5 turned over). The UI does not take
pointer events yet: the report's `[touch]` line has present or not, chip, IRQ, the touch count,
the last point (canvas and raw), reads and errors; the mirror gets `~PGT` lines (section 6).

**Both on one bus with the BMI270** (`daisy/core/i2c_bus.h`). The drivers are main-loop state
machines, one blocking transaction at a time (3 ms timeout; the longest is a 64-byte BMI270
chunk, ~1.7 ms), so they never overlap. A missing device NAKs at once. A stuck bus (SDA held
low, or no pull-ups at all: the STM32 HAL then waits up to 25 ms for the busy flag on every
call) is caught by a guard: the first slow failure holds every call for 1 s, failing at once, so
the main loop loses at most one timeout per second. The `[i2c]` line says whether it is held.
Pull-ups: libDaisy sets none on the pins. The SparkFun Qwiic board and the 2090 bring their own
(values not verified here); with neither connected the bus reads as held, which is harmless.

### Force-sensor pads: four Adafruit 1075 (Alpha MF02A-N-221-A01)

**Wiring**, the same for each pad (pad 1 = A5 / pin 27, pad 2 = A6 / pin 28, pad 3 = A7 / pin 29,
pad 4 = A8 / pin 30; in the drums job they play kick, snare, closed hat, perc):

```
  Seed 3V3A (pin 21) ----+---- FSR ----+---- Seed A5 (pin 27)      (A6 / A7 / A8 for pads 2-4)
                         |             |
                   (the other pads)  10 k
                                       |
  Seed AGND (pin 20) ------------------+                           (optional: 10 nF from the
                                                                    ADC pin to AGND, see below)
```

An FSR has two legs, either way round. Wire the 10 k to AGND for every pad **even before its
sensor is there**: a pin left open floats and plays random notes (a pad that is not wired at all
can be switched off: `cmd pad 4 off`). Do not solder to the FSR's legs (the plastic melts):
use a breadboard, female jumpers or Adafruit's clincher connectors.

**What the ADC sees.** The divider gives x = V / 3.3 = 10 k / (10 k + R_fsr). An FSR's conductance
is roughly proportional to force, so `daisy/core/pads.h` turns each reading into conductance,
**G = 100 uS * x / (1 - x)**, and works only in that (linear in force, where the divider voltage
bends towards 3.3 V). At rest (> 1 M) G < 1 uS; 100 k is 10 uS; 10 k is 100 uS; 1 k is 1000 uS.

**The scan.** Once per audio block, **500 Hz (every 2.0 ms), in the audio interrupt**, so the
interval never jitters with the main loop. The ADC converts continuously (DMA, oversampling 32,
16 bits); with the pads sampled 32.5 cycles each the nine channels take about 8,000 ADC cycles
per sweep, 0.65 ms at libDaisy's 12.3 MHz ADC clock (1.3 ms if the chip halves it again
internally): computed, not measured. Under the 2 ms scan, so every scan sees a fresh value.
The pads' sampling time is longer than the pots' (8.5 cycles) because the divider's 10 k is a
high source impedance for the ADC: with too short a sampling time the previous channel's
charge leaks into the reading, which looks like crosstalk between pads. If the `[pads]` line
still shows one pad moving with another, a 10 nF ceramic capacitor from each pad's ADC pin to
AGND fixes it (10 k x 10 nF = 0.1 ms: fast enough for the velocity).

**A hit** (per pad, `daisy/core/pads.cpp`):
- **Onset**: the first scan with G at or above the pad's **threshold** (default 10 uS).
- **Velocity window**: that scan and the next (**2 scans: the note is decided 2.0 ms after the
  onset scan**, from 2-4 ms of force history). Velocity = the fastest rise seen in the window
  (the onset scan's rise counted from the threshold), scaled so that rising the whole span,
  threshold to **ceiling** (default 650 uS), within the window is 1.0; then the pad's **curve**
  (velocity^curve, default 0.5; "The velocity curve" below). A slope, not a level: where the crossing fell between two scans
  does not change it (the test moves a hit by 0.5 ms steps: the same velocity). Reaching the
  ceiling ends the window at once (velocity 1). Falling below the release level inside the
  window is a blip (noise, a spike): no note.
- **Held**: the note plays; **pressure** 0..1 = (G - threshold) / (ceiling - threshold) follows
  as events when it moves by 1/128.
- **Release**: below **60 % of the threshold** (hysteresis) for **2 scans in a row** (4 ms). A
  one-scan bounce does not end the note, so it cannot double-trigger.
- **Crosstalk**: when a pad's window ends, a pad whose velocity is below **0.4 x** that of a
  neighbour hit within **5 scans (10 ms)** is a ghost and plays nothing. A hard hit leaks part of
  its force into the next pads (through the board, and through the ADC); a flam across two pads
  hit about as hard plays both, a soft hit 40 ms after a hard one plays.

**Latency, touch to note (estimated from the design, not measured):** the crossing is seen 0-2 ms
after it happens, the note is decided 2 ms later, the main loop hands it to the UI's padDown,
which posts it to the engine at once (within one pass: normally well under 1 ms) and the engine plays it in the next block, which the codec
outputs one block later: **6-8 ms from the force crossing the threshold to sound** (velocity
window 2 ms + 0-2 ms scan phase + 4 ms of engine and output buffering). Two things can make it
worse: a long main-loop pass (a UI frame, a QSPI erase of tens of milliseconds while /int is
saved; the pool's copy waits during a recording) delays the post, and a window of 3 or 4 scans
(`cmd pads window N`) adds 2 ms each. A seam in the core would take 2 ms and the main-loop risk
away (section 14).

**Calibration**, per pad: threshold (uS), ceiling (uS), curve, on/off; global: window, release
scans, hysteresis, crosstalk ratio and time. `cmd pad N thr .. ceil .. curve ..`, `cmd pads ...`
(section 6); saved at once in `/int/pg/daisy.cfg` with the Daisy's other settings (display
orientation, SPI speed, touch map), which goes into /int's flash snapshots with the UI's own
settings. `tools/pg-daisy pads` prints every 100 ms, per pad: the raw reading x, G in uS, the
peak since the last line, the pressure, the state, the last velocity and the note velocity it
makes (`0.79=101`): enough to choose a threshold (above the idle value, below a light touch).

**What it plays: the core's pad API** (`lib/pg/src/pg/ui/pads.h`, docs/UX.md 10.3 "Pads"), in
one function, `App::padEvent` (`daisy/app/app.cpp`): a hit is `Ui::padDown(pad, velocity)`, the
pressure of a held pad `Ui::padPressure`, the release `Ui::padUp`. From there it is the core's:
- **The job**, drums or notes (saved with the Daisy's settings, so it survives a restart).
  **Drums**: pad 1-4 play KICK, SNARE, CHH, PERC (note 60), whatever track is selected.
  **Notes**: the pads play the selected track up its scale (degree 1-4; a synth with the
  octave and the lead's chord mode as the note keys, a second pad held on a synth slides).
  Switch: `tools/pg-daisy tap padjob` (the desktop app's F4: a toast says which), or
  `tools/pg-daisy cmd pads job drums|notes`.
- **Velocity**: the note's, 1..127, from the hit (below). Live record records it with the note.
  An instrument with `vel` lines (CRACK, ALLOY, TAP, PLUCK) also moves those macros by it, from
  the note-on to the track's next note: velocity 100 leaves them where they are.
- **Pressure**: the held pad's pressure moves the macro the instrument's `press` line names
  (TAP: PRESS, ACID: BITE, HANDS; others: the macro tilt X moves), after a small dead zone, and
  glides back in 120 ms when the last pad of the track is let go. Not recorded.
- **The hand-over**: the scanner runs in the audio interrupt and queues its events (64 places;
  pressure never takes the last 8, so a stalled main loop drops pressure values, never a hit or a
  release). The main loop takes them every pass: hits and releases at once, in order; pressure
  merged to the newest value per pad and handed over at most once per pad every 8 ms (the UI's
  own limit per track, so nothing waits for the UI's next frame). A sensor sends a pressure
  event at nearly every scan while the force moves (500 a second): the UI gets about 125.
`tools/pg-daisy cmd pads` shows, per pad, what it plays now and what the engine hears of the
track's four macros (`heard`, velocity, pressure and tilt included), and the pressure macro and
its offset (`press 1 +160`).

**Pads from the computer, without sensors** (testing before the sensors are wired, scripted
tests): `tools/pg-daisy pad N VEL [hold MS] [press P]` hits pad N (1-16) at velocity VEL (0..1,
what a sensor gives after its curve: 0.786 is note velocity 100, 1 is 127), lets it go after MS
(default 100; `hold 0` keeps it down until `tools/pg-daisy cmd hit N up`), with pressure P (0..1)
while held, and prints what it plays and what the engine hears, held and after. Over serial it
is `>hit N VEL [hold MS] [press P]`, `>hit N press P`, `>hit N up` (key scripts too:
`tools/pg-daisy script` passes them through). They go through `App::padEvent` like a sensor's
events, so everything above applies. Examples, with `daisy_sim` or the board:
```
  tools/pg-daisy pad 1 0.3                 # a soft kick
  tools/pg-daisy pad 1 1                   # a hard one
  tools/pg-daisy pad 4 0.8 hold 600 press 1    # PERC held 0.6 s, squeezed hard
  tools/pg-daisy tap padjob                # the notes job: the selected track up the scale
  tools/pg-daisy pad 2 0.786               # its second degree, at velocity 100
```

**The velocity curve.** The core makes 0..1 into the note's 1..127 linearly and treats 100 as
neutral for `vel` lines: a hit below 100 pulls those macros the other way. With a linear curve
most hits would land below 100 and a pad-played instrument would sound darker or softer than its
knobs say, so the platform's curve does the work. The defaults (threshold 10 uS, **ceiling 650
uS, curve 0.5**, window 2 scans) put:
- a **medium** hit, the force's steepest rise about **100 uS per ms** (from nothing to 400 uS,
  the FSR at about 2.5 k, in 4 ms), at **0.79 = note velocity 101**;
- a **hard** hit, **160 uS per ms or more** across the window (640 uS in 4 ms) or **650 uS
  (about 1.5 k) reached inside it**, at **1.00 = 127**;
- a soft tap (120 uS in 8 ms, 15 uS per ms) at 0.31 = 40; a slow press at about 0.1.
`test_daisy_pads` checks these on synthetic hits at four phases of the scan. **What "medium"
means on real pads is a guess** (a finger on the sensor, or on rubber over it, has not been
measured); a rise shorter than one 2 ms scan is seen only in part.

**Tuning the velocity** (per pad, saved at once):
1. `tools/pg-daisy pads` in one terminal. Hit pad N as hard as you would play it, several times.
   The `vel` column should read `1.00=127` for most of them. If not, lower the ceiling: `cmd pad
   N ceil 500` (or 400). Reaching 1.00 on hits you would call medium: raise it (`ceil 900`).
   (The ceiling also sets full pressure: a held pad reaches pressure 1 at the ceiling.)
2. Then medium hits: they should read about `0.79=100`. Lower: a smaller curve lifts them
   (`cmd pad N curve 0.4`); higher: a larger one (`curve 0.6`). The curve does not move a hit
   that is already 1.00. The `[pads]` lines of `cmd pads` give the last hit's velocity before
   the curve too (`linear`).
3. `cmd pad all ceil X curve Y` sets all four at once; `cmd pads reset` returns to the defaults.

### Line in: the first recording path

`daisy/core/line_mic.h`: a `pg::sampler::MicInput` over the codec's stereo input, in both stages
(the input is on every Seed). The gesture is the Cardputer's: **hold L on a sampler page** (over
serial `>down l`, `>up l`); the same Recorder (the room's level, threshold start with 40 ms of
pre-roll, the last 60 ms dropped, the playback gain), into the track's pool slot through pg's
SampleWriter. On the Daisy that writes the SDRAM image at once; the QSPI copy follows in the
background after the recording (it waits while one runs), so **the sample survives a restart**
(`test_daisy_stage1` and `tools/test_pg_daisy.py` reboot and find it).

- **48 kHz in, 48 kHz stored** (`recRatesFor(48000, kPoolRate)`, no filter). A slot holds
  137,216 samples: **2.86 s at 48 kHz** (the Cardputer's 22,050 Hz slot holds 6.22 s). The slot's
  header says 48,000 Hz, so the voices play it at its pitch.
- **Mono**: the core's recorder takes mono. The fold-down is **(L + R) / 2, unless the right
  input is silent: then L alone.** Silent means every sample of a block below -60 dBFS, what a
  mono (TS) plug or a right input tied to AGND gives. Why: a stereo source keeps both sides in
  the sum; a mono source on L alone would lose 6 dB in a sum and gain the right input's noise. A
  right input left open on a breadboard picks up hum and is not silent: for a mono source tie pin
  17 to AGND. The choice is made per recording and frozen once the recording starts, so a sample
  never changes level half-way. The report's `[linein]` lines say which it was and give both
  input peaks.
- The input runs in the audio interrupt into a 1.37 s ring in SDRAM; the main loop feeds the
  recorder every UI frame. A stall longer than the ring is counted (`ring overruns`).
- The commit is the core's `SampleWriter::commitRecording(samples, name, rate)` with the
  recorder's stored rate, so the header says 48,000 (`test_daisy_linein`, and
  `itest_rate48_record` plays such a recording back at its pitch and length).

**Wiring:** source left to Seed pin 16 (AUDIO IN L), right to pin 17 (AUDIO IN R), the source's
ground to AGND (pin 20). A mono source: into pin 16, and pin 17 to AGND. A 3.5 mm jack breakout:
tip = L, ring = R, sleeve = ground. Start with the source's volume at about half.

**Levels.** libDaisy says only that the inputs take "typically a line level signal" (its
Getting-Started-Audio doc) and does nothing to the codec but reset it (no gain set over I2C, its
postgain is 1). TI's TAC5242 datasheet (SLASF27A) gives the ADC's full scale as **1 Vrms
single-ended** (2 Vrms differential) for AC-coupled inputs at 3.3 V AVDD, i.e. about 2.8 V peak
to peak single-ended. How the Seed3 routes its input pins to the codec (single-ended or
differential, any attenuation or coupling on the board) is not in libDaisy and has not been
verified. Consumer line level (-10 dBV, ~0.32 Vrms) and a phone's headphone output fit well;
pro line level (+4 dBu, ~1.23 Vrms) can clip at the loudest. **Never Eurorack levels: a module
puts out up to +-10 V (about 20 V peak to peak), seven times the full scale; it clips hard and
can damage the codec's input. The Labor's outputs are Eurorack level: do not wire them to pins
16 / 17** without an attenuator (for example 10 k in series and 1 k to AGND per channel: +-10 V
becomes about +-0.9 V, inside the 1.4 V peak of full scale) and only after checking its levels
on the Labor Scope.

### Tilt, G0, microSD, battery

- **Tilt**: a SparkFun BMI270 Qwiic board on I2C1 (D11 / D12) with the touch, address 0x68 (0x69
  with its jumper). `daisy/core/bmi270.h` probes it, soft-resets it and uploads Bosch's 8 KB
  configuration file in 64-byte chunks across main-loop steps. It then waits for "init ok" and
  sets the Cardputer's register values. `test_daisy_bmi270` runs it against a register model.
  Readings feed `pg::tilt::Tracker` at 100 Hz while G0 is held, as on the Cardputer. Qwiic
  wiring: black GND to DGND (40), red 3.3V to 3V3D (38), blue SDA to D12 (pin 13), yellow SCL to
  D11 (pin 12). (A BMI270 that is not found at boot is not probed again: power-cycle.)
- **G0**: a debounced GPIO (D25, pin 32) to ground through a push button, producing the same key
  events as the Cardputer's G0.
- **microSD** (Adafruit 4682, SDMMC, 4-bit, 25 MHz): libDaisy's SdmmcHandler and FatFS behind a
  `pg::FileSystem` (/sd). The card is mounted at boot, with a retry every 10 s. FatFs cannot
  replace a file on rename, so the target is removed first; pg::io's power-loss recovery covers
  that gap, as it does for the Cardputer's FAT card.
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
- **Denormals (checked and changed 2026-11-14).** The FX tails decaying in silence produced
  subnormal floats on the host. libDaisy does not touch FPSCR anywhere (its only FPU code is
  `SystemInit`'s CP10/CP11 enable, which a `BOOT_APP` build skips), so the firmware ran with
  flush-to-zero off. It now **sets FZ (bit 24) and DN (default NaN, bit 25)** in the preinit hook
  (`daisy/src/memory.cpp`), before any constructor: in FPSCR (the main loop) and in FPU->FPDSCR,
  the value the core gives an interrupt handler's FPSCR, so the audio callback gets them too. The
  same hook enables CP10/CP11 again (harmless if the bootloader did). The audio callback reads its
  own FPSCR, sets FZ / DN if they are missing and counts it; the report's `[fpu]` line shows the
  main loop's, the callback's and FPDSCR, so the first power-on proves it. Effect on the sound:
  values below 1.2e-38 become 0, about -750 dBFS. The M7 handles subnormals in hardware without a
  trap, so the speed gain is likely small; the point is that both contexts compute the same way.

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
- Stage 1's portable halves (2026-11-14), on the host: the ILI9341 setup sequence and the
  centred, dirty-row picture against a simulated controller (`test_daisy_panel`); the FT6206's
  register parsing, probe, events, loss and return, the coordinate mapping for both rotations,
  and the I2C guard on a stuck bus shared with the BMI270 (`test_daisy_touch`); the pad scanner
  on synthetic force curves: soft and hard hits, the crossing's phase, a slow press, hover and
  bounce, noise and spikes, flams, crosstalk, calibration, the default velocity curve, pressure
  never crowding out a hit in the queue, the settings file with the pads' job (`test_daisy_pads`);
  line-in recording through the Daisy's pool: the fold-down cases, 48 kHz headers, cancel,
  overrun, the mirror's flash copy read back (`test_daisy_linein`); the whole App with all of
  it, recording by the UI's gesture over serial and finding it after a reboot, a sensor hit
  through the core's padDown / padUp, a slow press's pressure merged on its way to the UI and
  returning to 0, hits from the computer, the job by `padjob` and `>pads job` kept over a
  reboot, settings surviving a reboot, touches to the mirror (`test_daisy_stage1`);
  `daisy_sim --line-in mono` with `tools/test_pg_daisy.py` (record, restart, `tools/pg
  sample get` returns REC01); and pads from the computer in `daisy_sim` (`tools/test_pg_daisy.py`:
  TAP hit at 0.3 and 1.0 hears its HIT macro below and above its base and the hard hit is
  louder in the report's output peak; a squeeze moves PRESS by the full 160 and it returns at the
  release; the job switch; `tools/pg-daisy pad` itself).
- The 2090's init values and orientations against Adafruit_ILI9341 1.7.0 and
  Adafruit_FT6206 1.1.1 (read, not run). The TAC5242's input full scale from TI's datasheet
  (search results quoting SLASF27A; the PDF itself was not opened here).
- The Cardputer firmware and the host build are untouched and pass (`docs/STATE.md`).

Only the hardware can show: that the board boots (clocks, SDRAM, QSPI init in BOOT_SRAM), that
the TAC5242 plays with libDaisy's reset-only codec setup, the real audio load, USB CDC on a Mac
(enumeration, throughput, no lost bytes), QSPI erase/write timing and its effect on the main
loop, the ADC's pots (noise, range), the LED patterns, stage 1's panel, touch, card, IMU and
timing, the force sensors' real curves and the pads' latency, the line input's level and noise,
the [fpu] line, and any interrupt-priority effects.

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
- Clocks (`src/sys/system.cpp`): SPI1/2/3's kernel clock is PLL2P = 25 MHz (HSE 16 MHz, M 1,
  N 12 + 4096/8192, P 8), so SPI prescaler 2 = 12.5 MHz; the ADC's is PLL3R = 24.6 MHz, with
  libDaisy's ASYNC_DIV2.
- ADC (`src/per/adc.cpp`): ADC1 only, 16 bits, oversampling 32 by default, continuous DMA
  (circular) when no multiplexer is used; `GetFloat` reads the latest value from the DMA buffer;
  sampling time per channel (`InitSingle`'s ConversionSpeed, default 8.5 cycles). The pins it
  can read are its own table of 16 (ADC1 INP3-INP19 without INP6), copied into `pins.h`
  `kAdc1Pins` and checked against the pin map by `test_daisy_core`.
- I2C (`src/per/i2c.cpp`): blocking HAL calls with the caller's timeout, after an unbounded wait
  for the handle to be READY; the pins are open drain with **no pull-ups**.
- Audio: the Seed's `postgain` is 1; nothing sets the codec's input gain (no I2C setup).
- FPSCR / FPDSCR: not written anywhere in libDaisy; CPACR only in `SystemInit` (skipped).
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
  landscape.
- The 2090: that its IM1-IM3 jumpers select SPI as remembered, where its touch pins are on the
  EYESPI revision, its I2C pull-ups, that it runs from 3.3 V on Vin, its backlight current from
  3V3D. That the FT6206's coordinates follow Adafruit's rotation-1 example on this board
  (`touch map N` corrects it without a new build).
- The force sensors' range on the Alpha MF02A (thresholds and ceiling are starting values), and
  the ADC's settling with a 10 k source at 32.5 cycles.
- How the Seed3 routes its audio inputs to the TAC5242 (single-ended or differential, coupling,
  any attenuation) and so the real input level.
- That the Daisy's CDC enumerates on macOS without a driver. It is a standard CDC ACM device,
  which macOS supports natively.

## 14. Seams in the core, risks, licences

Seams in the core wanted by stage 1 (the commit at the recording's rate and the pad API are in
the core now):
- **An engine command path the audio interrupt can use** (a second single-producer queue the
  engine drains at the start of `process`, or `Engine::postFromAudio`). The pads are scanned in
  the interrupt, but their notes go through the main loop to the UI and `Engine::post` (the
  queue has one producer). With it, a pad's note would play in the same block it is decided in:
  2 ms less latency (4-6 ms instead of 6-8), and no main-loop stall (a UI frame, a QSPI erase)
  could delay a note. Not made: it waits until the latency is measured on a board. What it
  would take: the hit's note decided in the interrupt as the UI decides it (the job, the track,
  the note, the slide), which means the UI's pad state on the interrupt side or a second, smaller
  copy of it; the UI told afterwards (live record, the held pads, pressure) without racing the
  note; and the engine draining the second queue first in `process`.

Seams in the core (none were needed for stage 0; these would help later):
- **A float output from the engine** (`Engine::process(float*, ...)`). The engine's output is
  int16, so the 24-bit codec gets 16-bit audio (with dither or truncation as the master does). A
  float path would also save the conversion.
- **A second section macro for the UI's and demos' constant text** (help text 12 KB, demo
  builders' data 16 KB), to move them into the ROM part if stage 1 needs room.
- **XferEnv: "flash writes do not stall"**. Samples and projects could then be put while the
  transport plays on a platform where that is true (the Daisy); today pg refuses with ERR PLAYING.
- **Pointer events in the UI** for a touch panel (the Hal delivers them now: `Hal::Pointer`,
  canvas coordinates).

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

Stage 1's risks, most likely first:
1. **The 2090 shows nothing (white)**: the SPI-mode jumpers not closed, MISO wired to D9, or
   the wrong header side. The firmware cannot tell: it only writes. Section 5 step 11.
2. **The touch axes mirrored or swapped**: the mapping is from Adafruit's example, not tried
   on this board. `cmd touch map N` fixes it without a build.
3. **The force sensors' numbers**: threshold 10 uS, ceiling 650 uS, curve 0.5 (a medium hit
   at 100 uS per ms), the 4 ms window and the crosstalk ratio are guesses from the FSR family,
   not this part. Expect to tune them with
   `tools/pg-daisy pads` (section 10); a floating pad pin plays random notes.
4. **The I2C bus** without pull-ups, or with a device holding it: `[i2c]` says "IS HELD".
5. **The line-in level**: unknown board routing; start with the source at half volume.
6. **The panel's SPI speed** on breadboard wires at 12.5 MHz: `cmd display spi 4`.
7. **Pad latency in practice**: a long main-loop pass delays a note (the report's `[main]
   longest pass`); the engine seam above removes that.

Licences: libDaisy (MIT) is fetched, not copied; the linker script is derived from libDaisy's
VisualGDB-generated script (BSD). Bosch's BMI270 configuration file (BSD-3-Clause) is extracted
by `tools/daisy-setup.sh` into `daisy/vendor/` with its notice (`daisy/vendor/BMI270_LICENSE`).
A distributed firmware binary that contains it must carry that notice. It is the same 8192
bytes M5Unified uploads on the Cardputer, compared byte for byte.

Adafruit: Adafruit_ILI9341 1.7.0 and Adafruit_FT6206_Library 1.1.1 were fetched for reference
only (not into the repository). No code was copied. Two things were taken from them as data,
recorded here: the ILI9341 setup table (`daisy/core/panel.cpp kIliInit`: the commands and their
parameter bytes from its `initcmd[]`, which are the panel vendor's register values) and the
FT6206's register addresses, ids and threshold (`daisy/core/touch.cpp`). Adafruit_ILI9341's source
header says "BSD license, all text here must be included in any redistribution" and its README
says MIT; Adafruit_FT6206 is MIT. Both are written by Limor Fried / Ladyada for Adafruit
Industries. If the table is treated as copyrightable, the notice to carry is Adafruit's: "Written
by Limor "ladyada" Fried for Adafruit Industries. BSD license, all text here must be included in
any redistribution."
