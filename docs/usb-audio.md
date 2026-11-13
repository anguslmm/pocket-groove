# Stereo USB audio: the instrument as a USB microphone

`env:cardputer-adv-usb` is the instrument with one difference: its USB port is a composite device. A Mac sees a serial port (boot log, `r` report, `[stats]`, `tools/pg`) **and** a stereo 16-bit audio input named **Pocket Groove**, which carries the instrument's stereo mix while you play. The headphone jack keeps working and plays the mono sum. `env:cardputer-adv` (the default) is unchanged, so the two can be A/B'd on the same device. How to use it: [MANUAL.md](MANUAL.md) section 16.1. The hardware test for the owner: [section 10](#10-hardware-test-checklist).

Status (2026-11-12): built and checked offline only. The spike (`env:usb-audio-spike`, same USB code) ran on the owner's hardware and Mac (section 2); the instrument build has not. Section 11 lists what is unverified.

Toolchain: pioarduino 55.03.312-1 (arduino-esp32 3.3.12, ESP-IDF 5.5.5, TinyUSB `88c7f0b81`). Paths below: `CORE` = `~/.platformio/packages/framework-arduinoespressif32/cores/esp32`, `TUSB` = TinyUSB at `88c7f0b81` (sources read on GitHub at that commit; the installed headers are byte-identical).

## 1. Summary

| | |
|---|---|
| Build / flash | `pio run -e cardputer-adv-usb -t upload` (from either firmware, section 7) |
| USB device | VID:PID `303A:1001`, product and manufacturer **"Pocket Groove"**, serial number = the chip's MAC as 12 hex digits (fixed per device); CDC serial port + UAC1 stereo input (2 ch, 16 bit, 44.1 kHz or 32 kHz = the boot-menu rate, one rate per boot) |
| Audio path | the engine renders **stereo**; every 128-frame block goes to USB (never blocking; one atomic load while nobody records), then (L+R)/2 goes to the codec's left slot |
| Jack | (L+R)/2: identical to today's mono mode within 0.02 dB for the three demos; hard-panned parts are up to 3 dB quieter (section 4) |
| CPU (estimate) | stereo needs 1.20x the mono render; expect AUD about 36-40 % at 44.1 kHz where the default build shows 30 % (section 5) |
| RAM | +7.7 KB static, about +3.6 KB heap: projected **free 55 KB, minimum-ever 27 KB** (default: 66 / 38 KB measured), section 6 |
| Spike defect fixed | the one-off "148 frames dropped" at stream start (section 3) |
| Code | `src/platform/usb_audio.{h,cpp}` (shared with the spike), `usb_trim.c`, `usb_cdc.h`, `uac1_desc.h`, `usb_desc_check.h`; `src/app/main.cpp` and `boot_menu.cpp` under `#if PG_USB_AUDIO`; `tools/pio_audio_ram.py`, `tools/pio_usb.py` |

The default env's firmware is the same program as before this work: every section of the ELF is byte-identical except the `__DATE__`/`__TIME__` strings (`python3 host/usbspike/elf_same.py old.elf new.elf`).

## 2. What the spike measured on hardware

`env:usb-audio-spike` on the owner's Cardputer-Adv and Mac, about two minutes at 44.1 kHz:

| Item | Result |
|---|---|
| Enumeration, streaming | yes; QuickTime recorded the test tones and the click correctly |
| Packets | 1000/s, 44/45-frame cadence, about 10.3 % 45-frame packets |
| Underruns (empty packets) | 0 |
| Descriptor self-check | PASS |
| I2S rate (ESP32 crystal) | 44102.7 Hz (+61 ppm) |
| Device rate in the host's clock | 44100.3 |
| FIFO | 1356 / 3104 B, target 1552 |
| Overruns | **148 frames dropped, once at stream start**, not growing afterwards (fixed, section 3) |

The spike now uses the instrument's USB module (`src/platform/usb_audio.*`); only its test signal and status screen are its own (`src/usbspike/spike_main.cpp`). It still builds and behaves the same, with the fix of section 3 and a "start discards" counter: `pio run -e usb-audio-spike -t upload`, then the LCD and the serial port show the counters once a second (left 440 Hz, right 660 Hz, a click every second of I2S time; `host/usbspike/analyze_recording.py rec.wav` checks a recording). Serial `h d r b s`; G0 tap switches 44.1 / 32 kHz, G0 held 2 s enters download mode. `env:usb-audio-spike-nousb` is its no-USB RAM baseline.

### The instrument build on hardware (2026-11-12)

`env:cardputer-adv-usb` on the owner's device and Mac, 44.1 kHz, ACIDJAM with everything on, QuickTime recording, no SD card: `AUD` mostly mid-40s % with a few spikes just over 50 (the jack-only build measured about 30 %; the estimate in section 5 was 36-40 %), heap 54 KB free / 34 KB minimum-ever (jack-only: 66 / 38 KB; the projection in section 6 was 55 / 27 KB), no USB underruns or overruns (the start-of-stream overrun is gone), no clicks in the recording. On that result it became the default environment. Still unverified: 32 kHz, unplug and sleep, flashing back to the jack-only build, heap with an SD card mounted.

## 3. The start-of-stream overrun: cause and fix

**Cause.** When the host selects the streaming alternate setting (SET_INTERFACE), TinyUSB opens the endpoint, queues a zero-length packet and from then on refills the endpoint from our FIFO each time a transfer completes (`audio_device.c` `audiod_set_interface`, `audiod_tx_xfer_isr`). But macOS polls the endpoint only some ms later. Until it does, the DWC2 port retries each unpolled ISO packet once and then completes it as failed with 0 bytes (`dcd_dwc2.c` `handle_incomplete_iso_in`, `iso_retry = bInterval = 1`), and the audio driver treats that like any completion: it takes the next packet out of the FIFO. So before the host polls, the FIFO drains at about one packet per 2 ms while the audio task fills it at the full rate. The spike prefilled the FIFO to its target (1552 B) at the first block after the open and then pushed every block, so about 12 ms after the open the FIFO (3104 B) was full, and the rest of each block was dropped and counted as an overrun until the host started. 148 frames is what a host that starts polling about 19-20 ms after SET_INTERFACE produces in the model. After that, flow control drained the FIFO back to its target, which is why the counter never grew again.

**Fix** (`usb_audio.cpp`, shared by both builds). A stream now starts in an OPENING phase. In it, `push()` holds the FIFO at the flow-control target: silence below it, at most one block above it, and frames above that are discarded silently (counted as "start discards": nobody can hear them yet). The ISR callback switches to RUNNING at the first completed packet **with data** (`n_bytes_sent > 0`; a missed frame completes with 0 bytes), i.e. once the host is really receiving. Only then is a full FIFO an overrun. Whenever the host starts, flow control starts on target.

**Covered offline** (`sh host/usbspike/run_checks.sh`, section "stream start"): the consumer model now has the host start delay and the retry-then-fail drain. Results, worst of 8 block phases:

| Host polls after SET_INTERFACE | 0-12 ms | 15 ms | 20 ms | 30 ms | 100 ms | 1 s |
|---|---|---|---|---|---|---|
| spike sequence: overrun frames (44.1 kHz) | 0 | 75 | 165 | 390 | 1897 | 21334 |
| fixed: overrun / empty packets (44.1 and 32 kHz) | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

The fixed producer's FIFO is 1884-2064 B when the first packet reaches the host, for every delay up to 5 s. The steady-state checks are unchanged (0 / ±100 / ±1000 ppm at 44.1 kHz, 0 / ±250 ppm at 32 kHz, 0-2 ms jitter: no empty packet, no overflow, rate within 5 ppm of the I2S rate).

## 4. What the jack plays

The codec is mono. With USB audio the engine renders stereo once (no second render); the jack gets **(L+R)/2** of the int16 stereo output, computed in `plat::usbaudio::pushAndFoldToMono()`. The master limiter's ceiling is 1.0, so |L|, |R| <= 32767 and the sum can never clip.

The engine's mono mode was the alternative only on paper: it renders one bus, so it cannot produce the stereo mix at the same time. Comparison on the host (`render_demo --mono` against (L+R)/2 of `render_demo`, full songs, analysed with `tools/ears.py --compare`):

| Demo | RMS mono mode / sum | difference signal | bands (sub..air) | LUFS | true peak | clipped samples |
|---|---|---|---|---|---|---|
| ACIDJAM | -14.22 / -14.22 dBFS | -62 dB | all within 0.02 dB | -16.13 / -16.13 | -1.67 / -1.67 dBTP | 0 |
| BASSHAUS | -13.32 / -13.32 dBFS | -63 dB | within 0.02 dB | -15.83 / -15.83 | -1.08 / -1.08 dBTP | 0 |
| WAREHOUS | -12.14 / -12.14 dBFS | -57 dB | within 0.02 dB | -14.97 / -14.97 | -2.03 / -2.04 dBTP | 0 |

Nothing cancels: L/R correlation 0.99, side/mid -23 to -25 dB, onsets, ducking depth, kick punch and clicks identical. What differs: the mono mode ignores PAN, the stereo engine uses a constant-power pan law (unity at centre, -3 dB per side at the extremes). So a **hard-panned** track is up to 3 dB quieter on the jack than today (measured: ACIDJAM with bass hard right and lead hard left, -0.7 dB overall and -2.9 dB in the worst 3 s window). Also the master's limiter and DRIV act on L and R separately instead of on their sum, which matters only for strongly panned, limited material. The demos are centred, so they sound as now. A louder sum ((L+R)/sqrt 2) would make centred material 3 dB louder than today and could clip, so it was not used.

## 5. CPU

`host/tools/bench_instrument` (busy pattern, all tracks, every send, DJ filter, drive, ducker):

| Scene | x86 Ir/frame (callgrind) | wall clock ns/frame |
|---|---|---|
| mono 44.1 kHz | 1732 | 192 |
| stereo 44.1 kHz | 2077 (**1.20x**) | 234 (1.22x) |
| stereo 32 kHz | 2080 per frame, 0.73x the frames | |

The default build was measured at about 30 % of a core in mono with the hot path in IRAM. Expected on the device for the USB build: **about 36-40 % at 44.1 kHz** (30 % x 1.2, plus the push and the mono fold, a few us per block) and about 27-29 % at 32 kHz. Above 50 % something is wrong (most likely a hot function left in flash).

- The stereo variants are what is placed in IRAM in this env: `Delay::run<false>`, `Reverb::run<false>`, `MasterBus::run<false>` and `LeadVoice::renderVoiceT<true>` (5.6 KB) instead of the mono `run<true>` ones (`tools/pio_audio_ram.py`, selected by `PG_USB_AUDIO`). The build fails unless, in the final ELF (read with the toolchain's `nm`), the hot path plus these four are at IRAM addresses and the mono variants are in flash (and the other way round in the default env). renderVoiceT<true> calls only IRAM code and ROM (memcpy, `__divsf3`).
- The USB interrupt now runs on **core 1** (the stack is started from `setup()`, which runs there), so its flash-resident driver code (about 4 KB, 1000 times a second) does not take time from the audio task. It still shares the instruction cache with the UI; the audio hot path in IRAM does not use the cache.
- Two priority-24 USB tasks without core affinity (`usbd`, `arduino_usb_events`) handle control requests and serial traffic in short bursts; the audio data path never wakes them.

## 6. RAM

### 6.1 Budget

Static numbers from the link maps (`python3 host/usbspike/map_ram.py A.map B.map`), heap deltas estimated from the sources. Free and minimum-ever for the default env are the owner's measurement.

| | cardputer-adv | cardputer-adv-usb | change |
|---|---|---|---|
| static DRAM (.data + .bss) | 226 796 B | 227 340 B | +544 |
| IRAM (.iram0.vectors + .text) | 115 163 B | 122 107 B | +6 944 (+7 168 at the heap boundary) |
| heap start (`_heap_start`) | 0x3FCD77F0 | 0x3FCD9610 | **+7 712 B static cost** |
| RTC slow memory (not heap) | 0 | 6 180 B (boot log 3 KB, unused audio OUT FIFO 3 KB) | |
| heap: serial port | USJ: 4 KB TX ring + RX 256 + lock, about 4.7 KB | TinyUSB CDC: RX queue 256 + lock, about 0.4 KB | -4.3 KB |
| heap: USB stack | | `usbd` task 4 KB + TCB, event task 2 KB + TCB + queue, handlers, config descriptor: about 7.9 KB | +7.9 KB |
| **free heap** | 66 KB (measured) | **about 55 KB** (projected) | -11.3 KB |
| **minimum-ever free** | 38 KB (measured) | **about 27 KB** (projected) | -11.3 KB |

Where the USB build's static RAM goes, against the default build: audio driver +3196 (the IN FIFO 3104 B; its OUT FIFO is in RTC memory), core descriptor buffers +1221, CDC +776, usbd +543, dcd +248, our code +320; IRAM: the stereo lead loop +5582, the stereo FX variants +440, `push` / `pushAndFoldToMono` / the ISR callback +813, ringbuf/event +84. Reclaimed: display strips -5760, USJ driver -182.

### 6.2 Reclaims done in this env (the default env is unchanged)

| Reclaim | Saves | Why it is safe |
|---|---|---|
| TinyUSB class drivers not linked (`usb_trim.c`, from the spike) | 20.6 KB static | stubs of drivers that find no interface of their class; any other use is a link error |
| TinyUSB host stack not linked (`usb_trim.c`, new: `tuh_inited`, `tuh_rhport_init`, `tuh_deinit`, `usbh_edpt_*`, `hcd_int_handler`) | 2.4 KB static | port 0 is initialised as a device only; the stubs answer "host not initialised"; any other use is a link error |
| The audio driver's unused OUT FIFO in RTC slow memory (`tools/pio_usb.py`: a copy of `audio_device.c.obj` with `.bss.ep_out_sw_buf` renamed into `.rtc_noinit`) | 3.1 KB static | without an OUT endpoint the driver only stores its address and resets its indices; the build fails if the section is missing or lands outside RTC memory |
| Display strips 2 x 3 rows instead of 2 x 9 (`display.cpp`) | 5.8 KB static | still double-buffered; 45 instead of 15 DMA transfers per full frame on the UI core |
| SD card: 3 FatFs file objects instead of 5 (`storage.cpp`; each holds a 4 KB sector buffer) | 8.2 KB heap **while a card is mounted** | `storage` never has more than 3 files open (`kMaxHandles`, static_assert) |
| No 4 KB serial TX ring (the TinyUSB port has its own 64 B FIFO) | 4.1 KB heap | writes keep the 10 ms timeout and the 1.5 s mute window |

### 6.3 The 28 KB between free and minimum-ever

**Found (2026-11-13, docs/STATE.md "RAM PASS"): the SD mount at boot with no card.** `SD.begin()` registers the FAT VFS (the volume and max_files file objects, 4 KB buffer each: ~18.5 KB here, ~27.7 KB in the default build) before it finds there is no card, then frees it; with the 8 KB mount task on top this set the minimum. The firmware now probes the card first (the SD library's own `disk_initialize`) and mounts only when one answers. Before that was found, the USB build measured it this way: `r` prints `[ram] min-ever free during boot: begin / storage / usb / menu+sd / ui / audio`, and every later drop is logged as `[ram] heap min-ever fell to N B at t=Ns`. Candidates read from the code: the transient `pg_sdmount` task (8 KB stack) at boot, overlapping the SD mount; FatFs at mount (with a card: about 25 KB in the default build, 16.7 KB in this one, permanent); the LittleFS file caches (512 B each) during save/load; `printf` with floats (small). Note: if the 66 / 38 KB were measured **without** an SD card, inserting one costs about 25 KB of heap in the default build (17 KB in this one).

### 6.4 Not done, for the owner to decide

| Option | Saves | Cost / risk |
|---|---|---|
| SD: 2 FatFs file objects instead of 3 | 4.1 KB heap with a card | an SD save during an SD transfer would fail cleanly (error, no data loss); review the open patterns first |
| Loop task stack 12 KB -> 8 KB (`getArduinoLoopTaskStackSize`) | 4 KB heap | needs the measured high-water mark (`[task] loopTask` line of `r`) after a session of saving, loading and transfers |
| Audio task stack 6 KB -> 4 KB | 2 KB heap | same: read `[task] pg_audio` while ACIDJAM plays with all FX |
| Keys task 4 KB -> 3 KB | 1 KB | same |
| Compile our own copy of `audio_device.c` (MIT) with EP OUT off and a 2048 B IN FIFO | 1 KB more static, and the 1 kHz ISR path could go to IRAM (+1 KB IRAM) | 1900 lines of vendored code; re-run the flow-control simulation with the new depth (jitter margin drops from about 8 to about 5 ms) |
| Display strips 2 x 1 row | 1.9 KB | 135 DMA transfers per full frame; measure the UI frame time first |
| Prebuilt IDF settings (esp_timer task 8 KB stack, 4 KB ISR stack, coredump stack) | several KB | needs `custom_sdkconfig`, i.e. rebuilding the IDF libraries (a large download and a long first build, docs section 9 of the old spike notes) |

The Project, the pattern count and the delay/reverb memory were not touched.

## 7. Serial port, flashing and recovery

### 7.1 The serial port

- It is a TinyUSB CDC port of the "Pocket Groove" device, not the chip's USB-Serial/JTAG. 115200 baud nominal (CDC ignores it). It comes up about 0.3 s after reset, once the firmware has read the sample rate (first boot after a format: after the format).
- Everything printed before a terminal connects (the boot log) is kept in RTC memory (3 KB, no heap) and printed again when a terminal first opens the port, and on `l`.
- Commands as before (`r`, `h`, `~PGX`), plus `l` boot log, `u` USB descriptor dump + self-check (feed it to `sh host/usbspike/run_checks.sh --dump log.txt`), `m` then `y` restart into the boot menu (the `m` of the boot window cannot be reached in time with this port), `B` then `y` download mode. Any other key cancels a pending `m` / `B`; it expires after 3 s.
- Opening the port never resets the board: the firmware turns off the core's DTR/RTS "reset to bootloader" sequence (`USBCDC::enableReboot(false)`), so terminals cannot trigger it whatever they do with the lines. Only an open at **1200 baud** restarts it (into the download mode), which is what `pio run -t upload` does on purpose; `tools/pg` always opens at 115200 and never touches DTR/RTS (`tools/test_pg.py` checks both).
- `tools/pg` finds either build's port: Espressif VID `303A`; when there are several, the one whose product is "Pocket Groove" wins. The receive queue is 256 B; the protocol's longest request line is 241 B and the host sends one at a time.
- Crash backtraces go to UART0 and the USB-Serial/JTAG console, neither of which reaches the computer in this build. Instead: the boot log shows the previous reset reason (as before, now visible through the replay), and when the core dump partition holds a dump, the boot log and `r` print `[crash] core dump in flash: task "...", PC 0x..., cause N, "reason"; backtrace 0x... 0x...` plus the `xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/cardputer-adv-usb/firmware.elf` command to decode it. The full dump: `esptool --chip esp32s3 read-flash 0x7f0000 0x10000 core.bin` in download mode, then the ESP-IDF tool `espcoredump.py info_corefile --core core.bin --core-format raw firmware.elf`.

### 7.2 Flashing in each direction

| From (running) | Command | What happens |
|---|---|---|
| `cardputer-adv` (USJ port) | `pio run -e cardputer-adv-usb -t upload` | the 1200-baud touch does nothing on the USJ port; PlatformIO waits up to 5 s for a new port, finds none and keeps the old one; esptool resets the chip into the ROM through USJ as always, flashes, hard-resets. The device restarts as "Pocket Groove" (new port name). |
| `cardputer-adv-usb` | `pio run -e cardputer-adv-usb -t upload` | PlatformIO opens the CDC port at 1200 baud; the firmware mutes the codec, shows DOWNLOAD MODE and restarts into the ROM, switching the PHY back to USJ (`usb_persist_restart(RESTART_BOOTLOADER)`); PlatformIO picks up the reappearing port (`wait_for_upload_port`), esptool flashes and hard-resets. |
| `cardputer-adv-usb` | going back: `pio run -e cardputer-adv-usb -t flashmode`, then `pio run -e cardputer-adv -t upload` | `flashmode` (`tools/pio_usb.py`) is the 1200-baud touch alone; the device waits in the ROM's download mode (USJ, `303A:1001`), where the default env's normal upload works. Without the first step the default upload fails: esptool sends its USB-Serial/JTAG reset sequence, which the TinyUSB port ignores. |
| `cardputer-adv-usb`, no computer command | serial `B` + `y`, or boot menu item `7 FLASH MODE` (pressed twice), then any upload | the same download mode, started on the device |
| anything, wedged | power off, hold G0, plug USB, release, then any upload | the ROM's download mode by the strap pin |

In download mode the screen shows DOWNLOAD MODE until esptool's final reset starts the new image. If the screen stays dark after an upload, the chip is still in download mode: unplug, switch off and on.

## 8. Status display

- **Diagnostics strip** (G0 double-tap): a fourth line `USB <state> 44.1K F<fifo>/<depth> U<underruns> O<overruns>`. State: `OFF` (stack failed), `NO HOST` (not enumerated), `SUSPENDED` (host asleep or cable out), `IDLE` (enumerated, nothing records), `OPENING` (a program opened the input, the host has not taken data yet), `STREAMING`. Colour: green while streaming cleanly, cyan idle/opening, amber without a host, red for 2 s after the underrun or overrun counter moved.
- **`r` report**: `[usb]` state, audio phase, rate (and what the host set), stream opens, serial port, descriptor self-check; packet counts by size, frames sent and pushed, FIFO level/depth/target, underruns (empty packets while streaming), overruns, start discards; `[crash]` core dump summary; `[ram]` minimum-ever by boot phase.
- **`[stats]`** (every 5 s): adds `| usb <state> fifo <bytes> und <n> ovr <n>`.
- No indicator in the normal UI.

## 9. Design notes (unchanged from the spike, see git history for the full study)

- UAC1 at full speed, one discrete rate per boot (`bSamFreqType = 1`, TinyUSB's flow control takes the rate from the descriptor), asynchronous isochronous IN endpoint (`bmAttributes 0x05`): the device sends what its I2S clock produced, 44 or 45 frames per 1 ms packet, the host adapts. No feedback endpoint (that is for asynchronous sinks), no Feature Unit (no host volume). Topology: Input Terminal (line connector 0x0603, 2 ch, L/R) -> Output Terminal (USB streaming) -> AS interface. `wMaxPacketSize` 184 B (44.1 kHz) / 132 B (32 kHz). The 91-byte block is built by `src/platform/uac1_desc.h`, the same code the desktop checker validates.
- The hand-off is TinyUSB's own IN FIFO (3104 B, single producer = the audio task, single consumer = the ISR, lock-free indices); `push()` never writes more than `tu_fifo_remaining()` because the driver configured it overwritable. With no stream open, `push()` is one atomic load.
- Drift: at 44.1 kHz the driver never sends 43 frames, so the I2S clock may run at most 2267 ppm slow against the host; measured +61 ppm.
- Flash writes (LittleFS saves) stall both cores, including the USB interrupt: a recording made across a save has a gap, as the jack does.
- A rate change in the boot menu restarts the device once more after the menu (the USB descriptor carries the rate); the host then sees the device leave and come back.

## 10. Hardware test checklist

Ordered; stop and report at the first step that fails. Needs a Mac with QuickTime and Audio MIDI Setup, `pio`, headphones. Write down the strip's lines (G0 double-tap) where asked.

1. **Default first.** With `cardputer-adv` running, play ACIDJAM with everything on (FX, ducker, DJ filter) for 1 min. Note the strip: `AUD ...%`, `HEAP ...K MIN ...K`, with and without an SD card in. (Baseline for steps 6-7.)
2. **Flash** `pio run -e cardputer-adv-usb -t upload`. Expected: about 5 s "Waiting for the new upload port", normal esptool output, the device restarts. `pio device list` shows a port described as "Pocket Groove".
3. **Serial.** `pio device monitor -p <that port>`: the boot log appears (replayed), with `[boot] USB: started, "Pocket Groove" stereo input 44100 Hz` and `[desc] self-check ... PASS`. Press `r`: check the `[usb]`, `[crash]` and `[ram] min-ever free during boot` lines; save the output. Press `u` and save that too (`sh host/usbspike/run_checks.sh --dump file`). Close the monitor.
4. **tools/pg.** `tools/pg ls`, `tools/pg pull session`: both work, the device keeps playing, nothing resets.
5. **Audio MIDI Setup**: "Pocket Groove", 2 inputs, 0 outputs, "2 ch 16-bit Integer 44.1 kHz". Strip USB line: `USB IDLE 44.1K` (cyan).
6. **Idle load.** Play ACIDJAM, everything on, nothing recording: strip `AUD` (expect about 1.2x step 1's value, about 36-40 % if step 1 showed 30 %), `HEAP`/`MIN` (expect about 11 KB less than step 1, MIN at least 24 K), `USB IDLE 44.1K F0/0 U0 O0`.
7. **Recording.** QuickTime, File > New Audio Recording, microphone "Pocket Groove", quality Maximum, record 2 min while ACIDJAM plays and you tweak (mutes, filter, a save to INT at the end). Strip while recording: `USB STREAMING 44.1K F1100..2000/3104 U0 O0` (green); `AUD` within 1-2 points of step 6; `MIN` not lower than step 6. After stopping QuickTime: `USB IDLE`, counters frozen. Listen to the file: stereo (pan a track to check), no clicks except at the save; the jack played throughout. `r` again: overruns 0, underruns 0 (or a few, only at the save), start discards some hundreds (normal).
8. **32 kHz.** Boot menu (hold a key at power-on, or serial `m` + `y`), `1 SAMPLE RATE` to 32 kHz, START: the device restarts once more by itself. Audio MIDI Setup shows 32 kHz. Repeat steps 6 and 7 (expect `AUD` about 0.73x the 44.1 kHz value, strip `USB ... 32K`).
9. **Unplug / sleep.** Unplug USB during a recording and plug back; let the Mac sleep and wake. The device must keep playing on the jack, the strip goes `SUSPENDED`/`NO HOST` and back to `IDLE`, the counters do not grow while nothing records.
10. **Download mode from the device.** Serial `B` then `y` (or boot menu item 7 twice): the screen shows DOWNLOAD MODE and `pio device list` shows the USJ port. Then `pio run -e cardputer-adv-usb -t upload` again works directly from the running firmware (1200-baud touch).
11. **Back to default.** `pio run -e cardputer-adv-usb -t flashmode`, then `pio run -e cardputer-adv -t upload`. The device runs the default firmware again (USJ port, `tools/pg ls` works).

Report: the strip values of steps 1, 6, 7 and 8 (a photo is fine), the `r` and `u` outputs, the recording (or its `tools/ears.py` summary), and anything that did not match.

## 11. Unverified until it runs on hardware

1. The instrument build boots, enumerates and records like the spike (same USB code, but the stack now starts from `setup()` on core 1 with `CDC_ON_BOOT=0`, and the CDC object is ours, `usb_cdc.h`).
2. The start-of-stream fix: OPENING ends at the first packet with data; the model of the host's late first poll (retry-then-fail every 2 ms) is from the driver source, not observed. `r` shows "start discards" (expected: some hundred frames per stream open) and overruns (expected 0).
3. The real CPU load in stereo (estimate 36-40 % at 44.1 kHz) and that the USB interrupt on core 1 does not disturb the UI.
4. The heap numbers (estimated: free about 55 KB, minimum-ever about 27 KB) and what takes the 28 KB at the low point (the `[ram]` lines will say).
5. The RTC-slow placement of the unused audio OUT FIFO and of the boot log (the linker and the ELF check confirm the addresses; that the driver never touches the OUT buffer is from its source).
6. The host-stack stubs: device-only operation unchanged (reasoned from `tusb.c`).
7. 3-row display strips: UI frame time (`[ui] ... panel push avg` in `r`), no tearing.
8. 1200-baud touch, `flashmode`, the boot-menu and serial download mode, PlatformIO re-detecting the port on macOS, and esptool's final reset leaving download mode.
9. The serial port: boot-log replay, `tools/pg` over TinyUSB CDC (256 B receive queue, 64 B TX FIFO with a 10 ms timeout), no reset when tools/pg or a monitor opens it.
10. The device name macOS shows (product string or interface string, both "Pocket Groove"), the PID (`0x1001` from the board variant's `pins_arduino.h`, the same number as the USB-Serial/JTAG; the spike notes said `0x0002`), and that macOS keeps the device's identity across reboots (serial number = MAC).
11. The core dump summary on a real crash.
12. Hard-panned tracks on the jack (-3 dB at the extremes) are acceptable to the ear.
