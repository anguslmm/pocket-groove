# Pocket Groove: hardware bring-up on the Cardputer-Adv

You have the device; nobody else on the team does. This guide gets the **smoke test** firmware onto it, walks you through every page, and tells you exactly what to send back. One flash should answer every open question in `docs/hardware-notes.md` section 10. Plan on 30-45 minutes with headphones.

Things marked **UNVERIFIED** come from documentation summaries or other people's projects, not from anyone on this team running them. If one turns out wrong, please say so in your report.

---

## 1. What you need

- An M5Stack **Cardputer-Adv** (the version with the TCA8418 keyboard and the ES8311 codec, not the original Cardputer).
- A USB-C **data** cable. Charge-only cables are the usual reason "nothing shows up".
- Wired headphones with a 3.5 mm plug, ideally plus a cheap mono/stereo adapter or a TRS breakout if you have one (for the left/right test).
- Optional: a microSD card (FAT32, any size). Only a scratch file is written to it and then deleted.
- `git`. PlatformIO clones the M5Stack libraries straight from GitHub (see `platformio.ini`), so git must be on your `PATH`. On Windows install "Git for Windows", on a Mac `xcode-select --install`, on Linux your package manager's `git`.

**Data loss warning:** flashing replaces the factory firmware (or Launcher). On first boot the smoke test formats the 1.5 MB internal data partition as LittleFS. The microSD card is not formatted. Section 6 covers getting back to stock.

---

## 2. Install PlatformIO

You need **PlatformIO Core 6.2.0 or newer**. The pinned platform (pioarduino 55.03.312-1, i.e. Arduino core 3.3.12 / ESP-IDF 5.5.5) refuses older cores.

### Option A: VS Code extension (easiest on every OS)

1. Install Visual Studio Code.
2. Extensions panel: search for **PlatformIO IDE** and install it. It brings its own Python and Core.
3. Open the `pocket-groove` folder (*File > Open Folder*). The first open downloads the toolchain (about 1 GB, several minutes).
4. Make sure the Core is current: the PlatformIO sidebar (the ant icon) > *Quick Access > Miscellaneous > Upgrade PlatformIO Core*.
5. The commands below go into the PlatformIO terminal: sidebar > *Quick Access > Miscellaneous > New Terminal*. That terminal has `pio` on its `PATH`.

### Option B: PlatformIO Core on the command line

macOS / Linux, with `pipx` (Homebrew: `brew install pipx`; Debian/Ubuntu: `sudo apt install pipx`):

```bash
pipx install platformio
```

Windows (PowerShell, Python 3.10+ from python.org with "Add to PATH" ticked):

```bash
py -m pip install --user pipx
```

```bash
py -m pipx install platformio
```

```bash
py -m pipx ensurepath
```

(then open a new terminal so `pio` is on the `PATH`)

Then on any OS, check the version (must say 6.2.0 or later):

```bash
pio --version
```

If an older Core is already installed:

```bash
pio upgrade
```

### Linux only: serial port permissions

The board appears as `/dev/ttyACM0` (native USB-Serial/JTAG, VID 303A PID 1001). Your user needs access to it. Install PlatformIO's udev rules:

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
```

```bash
sudo udevadm control --reload-rules
```

On Debian/Ubuntu also add yourself to `dialout` (`plugdev` on some distros), then log out and back in:

```bash
sudo usermod -a -G dialout $USER
```

Port names: Linux `/dev/ttyACM0`, macOS `/dev/cu.usbmodem*`, Windows `COM3` or similar (Device Manager > Ports). PlatformIO usually finds it by itself.

---

## 3. Build and flash the smoke test

From the repository root (the folder containing `platformio.ini`):

```bash
pio run -e smoke -t upload
```

The first build downloads the platform and libraries. The upload goes over the native USB port. esptool resets the chip into its bootloader by itself, so normally you press nothing.

Then open the serial monitor. Leave it open for the whole session: every result is also printed there, and you will paste that text back to us.

```bash
pio device monitor -e smoke
```

(`monitor_speed = 115200` comes from `platformio.ini`. Quit with `Ctrl+C`. The native USB port disappears for a moment on every reset, and the monitor reconnects by itself.)

To capture everything to a file as well (macOS/Linux):

```bash
pio device monitor -e smoke | tee smoke-log.txt
```

### If the upload cannot connect: download mode

Usual causes: a charge-only cable, a crashed firmware that disabled USB, or (rarely) a flaky auto-reset. To enter the ROM bootloader by hand:

1. Unplug USB. Slide the side power switch to **OFF**.
2. **Hold the G0 button** (the small button on the top edge; it is GPIO0) while you plug the USB cable in.
3. Release G0 and run the upload again.

**UNVERIFIED:** this exact procedure comes from M5Stack's documentation, seen only through search summaries. GPIO0 is the ESP32-S3 boot strap, so "hold G0 while powering up" is the standard method; the role of the power switch (it reportedly cuts only the battery) is not confirmed. Whether the Adv has a reset button is also unconfirmed. In download mode the screen stays dark. That is expected. After flashing, unplug and replug (or toggle the power switch) to start the new firmware.

If PlatformIO picks the wrong port, name it:

```bash
pio run -e smoke -t upload --upload-port /dev/ttyACM0
```

If uploads start but drop out half-way, add `upload_speed = 115200` under `[env:smoke]` in `platformio.ini` and retry. Another project reported that the native USB port drops at high baud rates on some hosts.

### Alternative: flash the prebuilt image (no build needed)

`build/firmware/pocket-groove-smoke.bin` is a **merged** image (bootloader + partition table + app), so it goes to offset **0x0**.

With esptool (any OS, Python 3):

```bash
pip install esptool
```

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write-flash 0x0 build/firmware/pocket-groove-smoke.bin
```

Replace the port with yours (`/dev/cu.usbmodem1101`, `COM5`, ...). esptool older than v5 spells it `esptool.py ... write_flash`. If it drops out, use `--baud 115200`.

Browser alternative (Chrome or Edge, uses Web Serial): open **https://espressif.github.io/esptool-js/**, click *Connect*, choose the board's port, set **Flash Address `0x0`**, pick `pocket-groove-smoke.bin` and click *Program*. Then unplug and replug the device.

Any serial terminal at 115200 works as the monitor (the baud rate is actually ignored by the native USB port): PlatformIO's monitor, `screen /dev/ttyACM0 115200`, PuTTY, the Arduino IDE Serial Monitor.

---

## 4. Using the smoke test

At power-on you should see a boot log on screen (board name, keyboard, internal flash mount, audio start), then the **KEYS** page. **The first boot can sit on "mounting /int" for 5-20 s** while it formats the internal data partition. That happens only once.

**Audio is silent until you choose a signal.** The codec starts at register value `0x90` (-23.5 dB), a safe, fairly quiet headphone level. **Put the headphones on the desk, not your head, the first time you start a signal or turn the volume up.** Each `=` press adds 2 dB. Values above `0xBF` are positive digital gain (shown in red): only use those on the speaker.

### Navigation (all pages)

| Key | Action |
|---|---|
| `Fn` + `1`..`5` | go to page 1 KEYS, 2 AUDIO, 3 CPU, 4 SYSTEM, 5 REPORT |
| `G0` button | next page (also works if the keyboard controller is dead) |
| `Tab` | next page (not on the KEYS page, where every key is under test) |
| `Fn` + `Enter` | print the full report to serial |
| over serial | type a key's character to "press" it, `>` = next page, `!` = full report |

The title bar always shows the page, the underrun count (`uN`) and the audio CPU %.

### Page 1: KEYS (mapping, key-up, rollover, ghosting)

The screen shows the 4x14 keyboard in the same layout as the device. Held keys turn **yellow**; a key that has produced both a down and an up event turns **green**. Every event is also logged on serial as `[key] down q id=15 raw=0x..`.

1. Press **every key once**, including `fn`, `Aa`/shift, `ctrl`, `opt`, `alt`, `del`, `Enter`, `space`, and the **G0** button. Check that each one lights **in the right place** and goes green when released. At the end the counter should say `seen 57/57`.
2. Hold one key for a few seconds and release it: one down, one up, nothing else.
3. **3-key chords** that form a rectangle in the controller's matrix: hold `a` + `s` + `d` and watch whether `f` lights up without being touched. Then `2` + `w` + `4` (watch `r`). A key lighting that you are not touching is **ghosting**. When the firmware sees four held keys forming a matrix rectangle it shows `rect ...` (red with `GHOST?` if the fourth key arrived in the same controller read as the third, which is the ghost signature).
4. **4-key chords:** `a+s+d+f` (a rectangle), then `q+w+e+r` and `z+x+c+v` (not rectangles). Do all four light? Does the `max` counter reach 4? Try 5 or 6 keys at once too.
5. Fast typing / rolling over keys: no stuck yellow keys afterwards.
6. `Fn` + `del` clears the key statistics if you want to start over.

### Page 2: AUDIO

Keys: `0` silence, `1` 440 Hz sine on both slots, `2` **left slot only**, `3` **right slot only**, `4` log sweep 20 Hz-20 kHz over 10 s, `5` 55 Hz sine, `6` 1 kHz square (-12 dBFS), `7` click track 120 BPM (accent every 4th), `8` blip mode (press **space**, hear a blip). `-` / `=` volume -2/+2 dB, `[` / `]` -0.5/+0.5 dB (the register value is shown), `s` 44.1/32 kHz, `b` backend (direct I2S / M5.Speaker fallback), `c` move the audio task to the other core, `d` DMA ring depth (3/4/6/8/12 blocks), `w` cycle ES8311 register 0x09 (slot select / word length, see below), `i` re-send the codec init, `r` reset statistics. All test signals are -12 dBFS (peak) unless noted.

Checklist (headphones first, then unplugged for the speaker):

1. `1`: clean 440 Hz tone? Any hiss, hum, buzz, crackle, or a second tone (aliasing / "image noise")?
2. `2` then `3`: **which of the two is audible**, on which side(s) of the headphones, and on the speaker? The ES8311 has a single DAC channel, so we expect one of these two to be silent everywhere. We need to know which.
3. With the **left/right-only** signal that *is* audible: is it on the left earcup, the right, or both? (Tells us how the jack is wired: tip, ring, or both.)
4. Press `w` (cycles reg 0x09: `00` -> `0C` -> `8C` -> `80`) while signal `2` or `3` plays and note which value makes which slot audible. `8C`/`80` should select the right slot if our reading of the datasheet is correct (UNVERIFIED). Also whether `0C` (16-bit word length) sounds any different from `00` (no difference expected).
5. `4` sweep: does it sound smooth from bass to treble with no dropouts, crackles or "birdies"? Roughly where does it become inaudible at the top? Where does the **speaker** stop producing bass?
6. `5` 55 Hz: audible on headphones? On the speaker (probably barely)?
7. `6` square: harsh but steady? Distortion at higher volume?
8. `7` click track: are the clicks perfectly regular (no stumbling)? Leave it running for a minute and watch `undr` (underruns) and `late`: both should stay 0.
9. `8`, then tap **space** about 20 times. The screen shows key->blip render latency; the report adds the buffered audio. Does the blip *feel* immediate? (Under ~10-15 ms usually feels instant.)
10. Volume: from the starting value press `=` up to 0xBF (0 dB). Is the jump between steps even? Is there a click or pop at any step? What is a comfortable headphone level? What is loud enough on the speaker? Note both register values.
11. Plug and unplug the headphones while a tone plays: does the speaker mute with headphones in? Any pop?
12. `s` (switch to 32 kHz): repeat 1, 4 and 7 briefly. Does 32 kHz work at all? Does the pitch stay correct (440 Hz should not move)? Then `s` again back to 44.1 kHz.
13. `b` (M5.Speaker fallback backend): repeat 1, 2/3 and 9. Same sound? Higher latency (expected)? Then `b` again to return to direct I2S.
14. `d`: step the DMA depth down to 3 with the click track playing. Any underruns? (A smaller ring means lower latency but less margin.)
15. Note anything odd at power-on, at signal changes or when changing settings (pops, clicks, hum when idle).

### Page 3: CPU

This page runs synthetic DSP **inside the audio callback** while audio plays, and shows how long each 128-frame block takes to render (`render avg / max`, microseconds) and as a share of one core.

Keys: `1` PolyBLEP saw, `2` TPT state-variable filter, `3` one-pole + tanh approximation, `4` 4-pole nonlinear ladder at 2x oversampling, `5` 8-line FDN reverb, `6` float divide per sample, `7` `sinf`, `8` `expf`, `9` `tanhf`, `0` off. `-` / `=` change the instance count N (1..128), `a` automatic calibration run, `p` primitive op costs in CPU cycles, `c` move the audio task between core 0 and 1, `f` full-rate display redraw on/off, `w` flash-write stress, `e` SD-write stress, `r` reset statistics.

1. Press `a` and wait ~9 s (each kernel runs 0.7 s). The table shows cycles per instance per sample. The same table goes to serial.
2. Press `p`: primitive costs (float mul-add, divide, int16 conversion, interpolated table lookup, `sinf`/`expf`/`tanhf`, and the same for **double**, to show why the engine is float-only).
3. Pick `1` (saws) and press `=` repeatedly until the CPU figure passes ~70%. Note N and the %. If you push it past 100% you will hear dropouts; the firmware deliberately gives the core away for a moment (the `yld` counter) so the device does not reboot. Press `0` to stop.
4. Set N back to a moderate load (~40%), press `f` (full-rate display), wait 20 s, press `f` again. Does `max` or `gap` get worse while the display redraws flat out?
5. `w`: writes 3 x 64 KB to internal flash while audio plays, then reports the longest gap between audio blocks and any underruns. Play the click track (AUDIO `7`) while doing this and listen for a hiccup. Writing to internal flash pauses **both** cores; this measures how long.
6. If an SD card is in: `e` does the same on the card.
7. `c` moves the audio task to core 1 (where the UI runs); repeat `a` there, then `c` back to core 0.
8. Optional: switch to 32 kHz on the AUDIO page and repeat `a`. The cycles per sample should be about the same, and the budget per sample is 37% higher.

### Page 4: SYSTEM

Automatically filled shortly after boot: chip, flash size, **PSRAM** (expected: none), heap, an allocation test of the instrument's expected memory footprint (70 + 55 + 32 + 28 KB blocks), battery, display frame time, an internal-flash file test (64 KB write/read/verify), SD detection + the same test, and an **I2C scan** of the internal bus (expected: codec `18`, keyboard `34`, IMU `68` or `69`).

Keys: `a` allocation test, `l` internal flash test, `s` (re)mount SD and test it, `i` I2C scan, `b` cycle display brightness.

1. Check every line is green. Note anything red.
2. With an SD card: insert it, press `s`. Without a card: press `s` anyway and check that nothing hangs and audio keeps playing (have a tone on).
3. Battery percentage on USB vs on battery (unplug for a moment): plausible?

### Page 5: REPORT

A condensed summary that fits on screen: **photograph it** if serial is not available. `Enter` prints the full report to serial (the block between `===== POCKET GROOVE SMOKE REPORT` and `===== END REPORT =====`).

---

## 5. What to send back

1. The **full serial log** (or at least the last full report block), produced *after* you have gone through all pages. `Fn` + `Enter` or `!` on serial prints it. If you only have the screen, a photo of the REPORT page plus the SYSTEM page.
2. This list, filled in:

```text
Board / setup
  host OS + how flashed (pio upload / esptool / web):
  auto-reset upload worked without pressing G0?  yes / no
  download-mode procedure, if needed, that worked:
  any boot problems (blank screen, reboot loop, long first boot):

KEYS
  all 57 seen (56 keys + G0), all in the right place?  yes / no (which wrong):
  key-up events reliable (no stuck keys)?  yes / no
  a+s+d -> does f light without touching it?  yes / no
  2+w+4 -> does r light?  yes / no
  4-key chords (a+s+d+f, q+w+e+r, z+x+c+v) all four lit?  yes / no
  max simultaneous keys reached:
  anything else (chatter, double events, missed fast presses):

AUDIO (direct I2S, 44.1 kHz unless noted)
  440 Hz both slots: clean? (noise / hum / buzz / second tone):
  LEFT-only (2): audible on  headphone L / headphone R / both / speaker / silent
  RIGHT-only (3): audible on headphone L / headphone R / both / speaker / silent
  reg 0x09 cycling (w): which values make which slot audible:
  sweep: smooth?  top audible frequency on headphones:  speaker bass cut-off approx:
  55 Hz audible on headphones / speaker:
  square: ok?
  click track steady for 1 min, underruns stay 0?
  blip latency: feels instant / slightly late / clearly late
  comfortable headphone volume reg:     loud speaker volume reg:
  pops/clicks at volume steps, signal changes, power-on, idle hum:
  headphones in -> speaker muted?  pop on plug/unplug?
  32 kHz: works? pitch correct? underruns?
  M5.Speaker fallback (b): works? sounds the same?
  DMA depth 3: underruns?

CPU
  (numbers are in the report; note anything audible)
  saws: N reached at ~70 %:
  dropouts during flash-write test (w) audible?  yes / no
  display full-rate (f) changed anything audible?

SYSTEM
  any red lines:
  SD card: type/size, test ok? mount without a card hangs anything?
  battery % plausible?

Free text: anything surprising, photos of anything odd on screen.
```

---

## 6. Back to stock firmware / Launcher

The smoke test does not touch the eFuses or the bootloader's security settings, so anything can be flashed over it.

- **Factory firmware:** M5Stack's **M5Burner** desktop app has the Cardputer-Adv factory image: select the device, download, *Burn*. M5Stack's page "restore factory firmware" for the Cardputer-Adv (docs.m5stack.com, `/en/guide/restore_factory/cardputer_adv`) describes the same with download mode (section 3). **UNVERIFIED:** names and paths come from search summaries; the docs site could not be opened while this guide was written. The factory demo's source is `m5stack/M5Cardputer-UserDemo`, branch `CardputerADV` (ESP-IDF 5.4.x), if you prefer to build it.
- **M5Launcher** (bmorcelli/Launcher, a boot menu that runs other firmwares from the SD card) has a web installer linked from its GitHub README, or is in M5Burner. Its `CardputerADV.md` notes "hold GPIO0, press reset" may be needed.
- To start from a completely clean chip before reinstalling anything:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
```

After an erase the device does nothing until something is flashed. The download mode still works, because it is in ROM.

---

## 7. How audio works on this board (for a C++ engineer new to ESP32 audio)

**The chain.** The ESP32-S3 has no usable audio DAC of its own. It sends digital samples over **I2S**, a 3-wire synchronous serial bus: **BCLK** (bit clock, GPIO41), **WS/LRCK** (word select: low = left slot, high = right slot, GPIO43), and **DOUT** (data, GPIO42). The **ES8311** codec turns that stream into an analog signal for the headphone jack and the NS4150B speaker amplifier. Plugging in headphones mutes the amplifier in hardware. The codec has registers (power, clocking, volume) that we set over **I2C** (address 0x18 on GPIO8/9). The keyboard controller (0x34) and the IMU share that I2C bus. Most codecs want a fourth clock, **MCLK**; this board has none. The codec derives its internal clock from BCLK (register 0x01 = BCLK as source, 0x02 = x8). Our frame is 2 slots x 16 bits = 32 BCLK per sample, x8 = 256 x fs, which is what the codec needs. That ratio is fixed, so changing the sample rate only means changing BCLK. The ES8311 has **one** DAC channel, so the output is mono. The smoke test's left/right tests find out which I2S slot it listens to.

**Who moves the bytes.** The I2S peripheral reads samples from RAM by **DMA**, from a ring of small buffers (here 4 buffers x 128 frames). Our **audio task** is a FreeRTOS thread pinned to core 0 at priority 20, above everything the app runs. It loops: call the render callback for 128 stereo frames of int16 → `i2s_channel_write()` copies the block into the next free DMA buffer, **blocking until one is free**. That blocking write is the clock of the whole engine: the task renders exactly as fast as the DAC consumes, and sleeps the rest of the time. If the render ever takes longer than the ring can cover, the DMA hardware plays silence (we enable `auto_clear`) and the driver's "send queue overflow" interrupt fires. That is the **underrun** counter.

**Block size and latency.** One block of 128 frames lasts 2.90 ms at 44.1 kHz (4.0 ms at 32 kHz). The engine works in 16-frame quanta inside each block, so events land within 0.36 ms. The latency from "render starts" to "sound leaves the DAC" is at most the block being rendered plus the full DMA ring: 5 x 128 = 640 frames = **14.5 ms** at 44.1 kHz (20 ms at 32 kHz). With 3 buffers it is 11.6 ms, but there is less margin against hiccups. The M5.Speaker fallback adds its own mixer and an 8-buffer ring (about 32 ms). Key presses add the keyboard path: the TCA8418 scans and debounces the matrix, pulls an interrupt line, and a priority-10 task drains its event FIFO over I2C. The blip test measures key interrupt → audio render. A block is 2.9 ms, so expect 0-3 ms there plus the buffered audio.

**What must never happen on the audio task:** waiting on anything except the I2S write. That means no I2C (the codec volume is written from the UI thread; the bus is shared and mutex-protected, and a 400 kHz transfer can take hundreds of microseconds when another task holds the bus), no `malloc`, no `printf`, no file access, no locks the UI could hold. A second trap is **flash**. While anything writes or erases the internal flash, the chip turns the instruction cache off and **both cores stop** for up to ~10 ms per chunk. The DMA keeps playing what is already queued, so a 14 ms ring may or may not cover it. The CPU page's `w` test measures exactly this.

**Watchdog.** If the audio task never blocks (render slower than real time), the idle task on core 0 never runs and the task watchdog reboots the chip after 5 s. On core 1 the UI would freeze instead (the watchdog only checks core 0's idle task in this build). The platform layer detects a render that keeps eating the whole period and briefly gives the core away. You hear dropouts, but the device keeps running and shows you why.

**Why float, never double.** The ESP32-S3's Xtensa LX7 cores have a hardware **single-precision** FPU (add/multiply/fused multiply-add in a few cycles). **Double precision is emulated in software** and costs tens to hundreds of cycles per operation. It sneaks in easily in C++: `0.5` is a double literal, `sin(x)` is the double function, and `std::pow(x, 2)` may promote. Engine code writes `0.5f`, `sinf`, `expf`, and builds with `-Wdouble-promotion`. Division is also slow even in float (no single-cycle divider), so hot loops multiply by precomputed reciprocals. Library calls like `sinf`/`expf` cost hundreds of cycles, so they belong at control rate (once per 16-frame quantum), not per sample. The CPU page's `p` measurement shows all of this in real cycles on your unit. Budget per sample: 240 MHz / 44.1 kHz ≈ 5,440 cycles for everything, of which the engine plan (ARCHITECTURE.md) allows ~55%.

**Two cores.** Core 1 runs the Arduino `loop()` (UI, display, files) and the keyboard task. Core 0 is left to audio. The display push (240x135x16 bit at 40 MHz SPI ≈ 13 ms per full frame) uses DMA on core 1 and does not touch the audio core. The smoke test lets you swap the audio task onto core 1 to see what sharing would cost.
