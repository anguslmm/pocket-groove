# Stereo USB audio input over the Cardputer-Adv's USB port

Researched and built 2026-11-11 against the installed toolchain (pioarduino 55.03.312-1: arduino-esp32 3.3.12, ESP-IDF 5.5.5, TinyUSB `88c7f0b81`). **Nothing here has run on hardware yet.** Section 9 lists what is unverified until it does.

## 1. Verdict

**Feasible, without rebuilding any library.** The prebuilt TinyUSB in arduino-esp32 3.3.12 has the audio class compiled in (`CFG_TUD_AUDIO = 1`). The spike registers its own UAC1 descriptor block with the Arduino core's composite-descriptor builder, the same way the core's `USBCDC` does. It supplies the TinyUSB audio callbacks, which the library declares weak. The result is one USB device with two functions: a CDC serial port and a stereo 16-bit UAC1 input called "Pocket Groove".

- **Class: UAC1** (USB Audio 1.0) at full speed, with an asynchronous isochronous IN endpoint, one fixed sample rate per boot (44.1 kHz, or 32 kHz), and no Feature Unit (section 4).
- **Drift:** the device sends what it has. TinyUSB's IN flow control picks 44 or 45 frames per 1 ms packet from the FIFO level, so the long-run rate is the I2S rate. That is valid for an asynchronous *source* in UAC1 and needs no feedback endpoint. Simulated at ±1000 ppm: no empty packets and no overflow (section 4.3).
- **Cost:** about **11.6 KB static RAM + about 7 KB heap**. Without the "linker diet" in `src/usbspike/usb_trim.c` it is 32.2 KB static. Flash cost is about 40 KB (section 6).
- **Workflow:** the USB port becomes a TinyUSB CDC port, not the chip's USB-Serial/JTAG. `pio run -t upload` still works from the spike's own env, through the 1200-baud reset that env enables. Going back to the normal firmware takes a 2 s G0 hold (or `b` on serial) first. Crash backtraces no longer appear on the serial port (section 5).
- **Spike:** `env:usb-audio-spike`, sources in `src/usbspike/`. The merged image is `build/firmware/pocket-groove-usbspike.bin`. The test procedure is in section 8.

## 2. What the toolchain has (evidence)

Paths are abbreviated: `LIBS` = `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3`, `CORE` = `~/.platformio/packages/framework-arduinoespressif32/cores/esp32`, `TUSB` = TinyUSB at `88c7f0b81` (the commit in `LIBS/../versions.txt`; its headers are byte-identical to the installed ones, and the sources were read at that commit on GitHub).

| Fact | Evidence |
|---|---|
| The audio class is enabled in every esp32s3 variant | `LIBS/qio_qspi/include/sdkconfig.h:516` `CONFIG_TINYUSB_AUDIO_ENABLED 1` (same in dio_qspi, opi variants) |
| It maps to `CFG_TUD_AUDIO` | `LIBS/include/arduino_tinyusb/include/tusb_config.h:143`; audio config `:176-194`: EP IN and OUT on, 2 ch, 4 B/sample max, 96 kHz max, `EP_IN_SZ_MAX` = 776 B, IN FIFO = 4 × 776 = 3104 B |
| The driver is actually in the archive | `LIBS/lib/libarduino_tinyusb.a(audio_device.c.obj)` defines `audiod_open`, `audiod_xfer_isr`, `tud_audio_n_write`, `tud_audio_n_get_ep_in_ff`, ...; `tud_audio_*_cb` and `tud_audio_tx_done_isr` are **W**eak (nm). `.bss`: `ep_in_sw_buf` 3104, `ep_out_sw_buf` 3104, `_audiod_fct` 88 |
| No interrupt or feedback EP on the S3 | `audio_device.h:133-140` defaults (`tusb_config.h` sets them only for the P4); `audio_device.c:827-829` rejects an AC interface with an endpoint when the interrupt EP is not compiled |
| IN flow control is on | `audio_device.h:128` `CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 1`; `audio_device.c:1776-1880` |
| A UAC1 data EP must not use sync type NONE | `audio_device.c:898,1156`: for UAC1 the driver calls an EP with sync type NONE a *feedback* EP and never streams it |
| The IN data path runs in the USB ISR | `usbd.c:1476` calls `driver->xfer_isr`, and `audio_device.c:511-540` picks the packet size and calls `tud_audio_tx_done_isr` |
| Arduino builds the configuration from registered blocks | `CORE/esp32-hal-tinyusb.c:876` `tinyusb_enable_interface2`, `:750` `tinyusb_load_enabled_interfaces` (enum order: MSC, DFU, HID, VENDOR, CDC, CDC2, MIDI, **AUDIO**, CUSTOM), `:1030` `tinyusb_get_free_in_endpoint` |
| CDC uses EP 0x85 (notify), 0x03 / 0x84 (data) and interfaces 0-1 | `CORE/USBCDC.cpp:33,108` |
| The core even ships a UAC class | `.../libraries/USB/src/USBAudioCard.{h,cpp}` (UAC1 at FS, `TUD_AUDIO10_MICROPHONE_DESCRIPTOR`). It is not used: see 3 |
| The app class-driver hook exists | `TUSB/src/device/usbd.c:438` `TU_ATTR_WEAK usbd_app_driver_get_cb` (W in `usbd.c.obj`). Not needed |
| The USB task and ISR | `CORE/esp32-hal-tinyusb.c:946` `xTaskCreate(usb_device_task, "usbd", 4096, NULL, configMAX_PRIORITIES - 1 = 24, NULL)` (no core affinity); `dwc2_esp32.h` `esp_intr_alloc(..., ESP_INTR_FLAG_LOWMED, ...)` on the core that runs `tusb_init` = `app_main` = **core 0** (`sdkconfig.h:915` `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`) |
| The DWC2 FIFO budget | `TUSB/src/portable/synopsys/dwc2/dwc2_esp32.h`: 7 EPs, 5 IN EPs, 256 words of FIFO RAM; `dcd_dwc2.c:215-260` `dfifo_alloc` |
| Today's port is the USB-Serial/JTAG | the board `m5stack-stamps3.json` sets `-DARDUINO_USB_MODE=1`, hwids `303A:1001`; `platformio.ini` adds `ARDUINO_USB_CDC_ON_BOOT=1` → `Serial` = `HWCDCSerial` (`CORE/HardwareSerial.h:444`) |

## 3. Approach and why

The brief's two fallbacks were for a library without the audio class. Neither is needed:

1. **App class driver via `usbd_app_driver_get_cb`, or our own copy of `audio_device.c`:** possible, since the hook is weak in the prebuilt `usbd.c.obj` and the core accepts any descriptor block through `tinyusb_enable_interface`. It is not needed because the prebuilt driver already has everything this needs (still to be confirmed on hardware). Compiling our own `audio_device.c` from `88c7f0b81` against a private config is still the **best RAM lever for the real feature**. The archive member is then never linked, the unused 3.1 KB OUT FIFO goes away and the IN FIFO can shrink (section 6.2).
2. **`custom_sdkconfig` / hybrid compile:** not attempted. Setting `custom_sdkconfig` makes pioarduino add the ESP-IDF framework (`platform.json`: `framework-espidf` from `github.com/pioarduino/esp-idf/releases/.../esp-idf-v5.5.5.tar.xz`) and rebuild the IDF and Arduino libraries in the project (`builder/frameworks/arduino.py:265-300, 341+`). That costs a large download (an IDF source tree, several hundred MB, not measured), a first build of tens of minutes (not measured), and the IDF component manager. The component manager fetches from `components.espressif.com`, which is probably blocked in this sandbox (not tried). It also changes the user's Mac build in the same way. Its only advantage here would be switching off unused classes (RAM). `usb_trim.c` gets most of that for free.

**Why not the core's `USBAudioCard`:** it would work for a stereo mic (`USBAudioCard(44100, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_STEREO)`). But it hardcodes the interface name "TinyUSB UAC1", declares a non-predefined channel layout instead of L/R (`wChannelConfig 0`), and leaves the producer side to us anyway (`write()` is `tud_audio_write`). Our own 91-byte descriptor (`src/usbspike/uac1_desc.h`) is portable, so the desktop checker builds and validates the very same bytes. The USB *library* is not linked at all. Nothing includes its headers, so its strong callbacks cannot collide with ours.

## 4. Design for the real feature

### 4.1 UAC1, not UAC2

- The S3 is full speed only. UAC1 is the natural full-speed class, and macOS supports it without a driver (AppleUSBAudio). It is also what Espressif's own template uses on this stack (`USBAudioCardDescriptors.h:560-599`).
- **One discrete rate in the Type I format descriptor** (`bSamFreqType = 1`). TinyUSB's flow control then takes the rate straight from `tSamFreq` (`audio_device.c:1788`), before any control request. UAC2 needs a Clock Source entity with RANGE and CUR requests, and its flow control only learns the rate from a clock SET_CUR or a GET_CUR answer passing through `tud_audio_buffer_and_schedule_control_xfer` (`:1691-1700`). That is workable, but there are more moving parts and nothing to gain at full speed.
- The rate follows the instrument: 44.1 kHz, or 32 kHz (the boot-menu fallback). It is chosen at boot, before the descriptor is built. The spike keeps the choice in RTC memory across `esp_restart()` (`usb_audio.cpp`). The host cannot change it: the endpoint answers SET_CUR and GET_CUR sampling-frequency requests for that one rate.
- No Feature Unit: the host gets no volume or mute, so there are no entity requests to answer. The topology is Input Terminal (line connector 0x0603, 2 ch, L/R front 0x0003) → Output Terminal (USB streaming) → AS interface. The AC interface has no endpoints, as the prebuilt driver requires.
- Bandwidth: 45 frames × 4 B = 180 B per frame. `wMaxPacketSize` = (ceil(rate/1000) + 1) × 4 = 184 B (132 B at 32 kHz). That is at least TinyUSB's flow-control maximum, (floor(rate/1000) + 1) frames, which the driver asserts (`audio_device.c:1823`).

### 4.2 Clock drift: asynchronous IN, the device sends what it has

The audio clock is the I2S clock: PLL_F160M / (N + b/a) / 256. The S3's 9-bit fractional divider hits 44100 and 32000 Hz exactly in theory (14 + 76/441 and 19 + 17/32). What is left is the crystal-versus-Mac-clock error, which should be tens of ppm (UNVERIFIED; the spike measures it).

- The endpoint is `bmAttributes 0x05` (iso, **asynchronous**). USB 2.0 §5.12.4.1 says an asynchronous *source* carries its rate implicitly in the number of samples per frame, and the host adapts. A feedback endpoint exists only for asynchronous *sinks*. That holds the same way in UAC1 and UAC2.
- TinyUSB's flow control (`audiod_tx_packet_size`, `:1842`) sends N = floor(rate/1000) frames normally and N + 1 when the FIFO is above `threshold + one slot`. At 32 kHz it also sends N - 1 when the FIFO is low. The threshold defaults to depth / 2 = 1552 B (`:1174`), about 8.8 ms. The long-run frames per ms therefore equal the I2S rate measured in the host's clock. At 44.1 kHz with ideal clocks the cadence is 9 × 44 + 1 × 45 per 10 ms; the simulation gives 9.996 % of 45-frame packets.
- Limit: at 44.1 kHz the driver never sends 43 frames, so the I2S clock may run at most 2267 ppm slow relative to the host (44.0 frames/ms) before the FIFO drains. That is far more than a crystal error. At 32 kHz the range is ±31 000 ppm.
- **Start of stream:** the FIFO starts empty. At 44.1 kHz it would climb to the target only through the 0.1 frame/ms surplus, about 4 s with less than one block of margin, and the simulation shows an empty packet right after opening. So the producer **prefills** the FIFO with silence up to the threshold when a stream opens (`s_prefillPending` in `usb_audio.cpp`). That costs no RAM and no latency beyond the steady state.

### 4.3 Hand-off from the audio thread (never blocks; free while unplugged)

```
audio task (core 0, prio 20, every 128 frames = 2.9 ms)
  render stereo -> usbaudio::push(LR, 128) -> mono sum (L+R)/2 -> I2S -> codec (left slot)
                       |  one atomic load if no stream is open
                       v
  TinyUSB IN FIFO (3104 B, static, already in the library)   <- the ring buffer: single producer / single consumer
                       |  tu_fifo indices are volatile, no mutex configured for this FIFO
                       v
  USB ISR every 1 ms: audiod_tx_xfer_isr takes 44/45 frames -> DWC2 TX FIFO -> host
```

- **No extra ring buffer:** the driver's own FIFO is the SPSC ring, and it is allocated anyway. `push()` writes at most `tu_fifo_remaining()` bytes and counts the rest as overrun frames. That rule matters: the driver configured the FIFO "overwritable", and a write past full would move the read index under the ISR.
- **Gate:** `s_on` = streaming alternate selected && mounted && not suspended. The flag is set from `tud_audio_set_itf_cb` / `..._close_ep_cb` (tud task) and from the Arduino USB events (unmount, suspend, resume). When nobody records (macOS opens alt 1 only while a client uses the input), when the cable is out (no VBUS sensing, so it looks like suspend) or when the host is asleep, `push()` is one atomic load.
- **Known benign race:** the driver clears the FIFO in `set_interface` (`:1081`) while the audio task may be mid-write. Worst case, one block of stale samples at a stream stop or start. The simulation and the code agree that nothing else touches the FIFO from both sides.
- **Measured offline** (`host/usbspike/check_usb_descriptors.cpp`, a model of the driver's exact flow-control code with this FIFO and producer): 120 s at 44.1 kHz with 0 / ±100 / ±1000 ppm and 0-2 ms producer jitter, and at 32 kHz with 0 / ±250 ppm. Results: no empty packet, no overflow, only N-1/N/N+1 sizes, delivered rate within 1.4 ppm of the I2S rate. The steady FIFO level is 900-1950 B (5-11 ms). A block may arrive up to about 8 ms late before an empty packet results (12 ms gives 9 per minute).

### 4.4 CPU

- **Per 1 ms in flash, on core 0** (the USB interrupt is allocated by `app_main`, which is pinned to core 0, the audio core): `dwc2_int_handler_wrap` → `dcd_int_handler` (1564 B) → `handle_txfifo_empty` / `epin_write_tx_fifo` / `edpt_schedule_packets` → `dcd_event_handler` (767 B) → `audiod_xfer_isr` (247 B) → `usbd_edpt_xfer_fifo` → `tu_fifo_*` / `tu_hwfifo_write`. That is about **4 KB of flash-resident code** run 1000 times a second (sizes from `nm -S` of the spike ELF). It is prebuilt, so it cannot be moved to IRAM short of rebuilding the library or vendoring the driver (and `dcd_dwc2.c` is not vendorable cheaply). Expect some cache pressure on core 0. The spike prints the audio task's CPU% (`plat::audio` stats) to make it measurable, with and without a stream open. UNVERIFIED how much it costs.
- Ours is in IRAM: `usbaudio::push` (354 B) and `tud_audio_tx_done_isr` (187 B, counters only), checked at 0x4037xxxx in the ELF. `push()` calls `tu_fifo_write_n_access_mode` (flash, 479 B) once per block.
- Two prebuilt priority-24 tasks with no core affinity, `usbd` (TinyUSB's `tud_task`) and `arduino_usb_events`, can preempt the audio task (priority 20) on core 0 for short bursts: control requests, CDC traffic, events. The audio data path never wakes them.
- Option for the real feature: move the USB interrupt to core 1 by building with `ARDUINO_USB_CDC_ON_BOOT=0` and calling `USB.begin()` from `setup()` (core 1). Then `Serial` becomes UART0, whose TX is GPIO43 = the I2S word clock (hardware-notes §9.12), so every `Serial` user in src/platform would have to move to an explicit `USBSerial`. Not done in the spike.
- Flash writes (LittleFS saves stall both cores for 250-300 ms, hardware-notes §11.1) also stop the USB interrupt (it is not IRAM-safe), so a recording made while saving will have a gap. Same as the jack.

## 5. Consequences for flashing and the serial console

The S3 has one USB PHY. With `ARDUINO_USB_MODE=0`, `tinyusb_init` gives it to the USB-OTG controller (TinyUSB), and the USB-Serial/JTAG controller (USJ) that all other envs use is disconnected from the pins.

| | Normal firmware (USJ, `303A:1001`) | Spike (TinyUSB CDC, `303A:0002`, "Pocket Groove") |
|---|---|---|
| `pio run -e cardputer-adv -t upload` | auto-reset through USJ, as today | **not reliable**: esptool's first resets on macOS (`UnixTightReset`, DTR and RTS set together) do not match the core's DTR/RTS state machine (`USBCDC.cpp:216-262`, which wants RTS, then both, then DTR, then none, one line at a time); its `ClassicReset` attempt does, and reboots into the ROM, but the port then disappears and comes back as USJ, and esptool does not follow it (it only special-cases USJ, `esptool/loader.py:837`). Do a 2 s G0 hold or send `b` first (below) |
| `pio run -e usb-audio-spike -t upload` | works; waits 5 s for a new port first, then flashes over USJ | works: `board_upload.use_1200bps_touch` opens the CDC port at 1200 baud, then `USBCDC::_onLineCoding` (`:283`) calls `usb_persist_restart(RESTART_BOOTLOADER)`, which switches the PHY back to USJ (`esp32-hal-tinyusb.c:590`) and forces download boot (`:679`). `wait_for_upload_port` then picks up the re-enumerated USJ port (`pioupload.py` `WaitForNewSerialPort`; checked with a dry run: "Forcing reset using 1200bps open/close on port ...") |
| Download mode by hand | power off, hold G0 while plugging USB, release (hardware-notes §8) | same, or **hold G0 for 2 s** while the spike runs, or **send `b`**: the LCD shows DOWNLOAD MODE and the ROM's USJ port appears |
| `pio device monitor` | `HWCDC` | works on the new port (`pio device list` shows "Pocket Groove"). It reconnects by itself after the spike restarts (rate switch). Output is dropped while no terminal holds DTR, never blocking (`USBCDC::write` checks `tud_cdc_n_connected`). Boot lines printed before the Mac opens the port are lost; press `s` for a status line |
| Panic / crash backtrace | printed on USJ (secondary console, `sdkconfig.h:919`) | **not visible**: the IDF console is UART0 (pins are the I2S word clock) plus USJ, which is unplugged from the PHY. The core dump still goes to flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, partition `coredump`): read it with `esptool read-flash 0x7f0000 0x10000 core.bin` in download mode |
| After esptool's final hard reset | runs the new image | runs the new image (UNVERIFIED: if the screen stays dark after an upload, the chip is still in download mode; switch the power off and on with USB unplugged) |

The 1200-baud touch is harmless against the normal firmware (the USJ ignores baud rates). It is set only in `env:usb-audio-spike`, so the existing envs are unchanged.

## 6. RAM and flash cost

### 6.1 Measured (link maps)

`env:usb-audio-spike` against `env:usb-audio-spike-nousb`: the same sources, the same flags, with Serial on USJ and `usbaudio::` as stubs. Reproduce with `python3 host/usbspike/ram_diff.py .pio/build/usb-audio-spike-nousb/firmware.elf .pio/build/usb-audio-spike/firmware.elf`, which uses the toolchain's own nm, not the host's.

| | Static DRAM (.data + .bss) | Flash text | IRAM |
|---|---|---|---|
| Baseline (no USB stack) | 28 124 B | | |
| Spike, prebuilt TinyUSB as is (`-DUSBSPIKE_TRIM=0`) | +32 219 B | +57.3 KB | +230 B |
| **Spike as committed (trim on)** | **+11 577 B** | **+38.4 KB** | +230 B (ours +541, newlib atomics +98, core +41; HWCDC's ISR helpers -446) |

Where the 32 KB came from: every class driver in `usbd.c`'s table is linked with its buffers, and so is the whole host stack (`tusb.c` references `tuh_inited`). NCM 6488 B, audio 6300 B, MSC 4160 B, DFU 4106 B, HID host 2983 B, usbh 1809 B, core descriptors and strings 1221 B, CDC 776 B, MSC host 700 B, and so on. `src/usbspike/usb_trim.c` defines the table entries of the unused drivers (MSC, DFU, DFU-RT, NCM, HID, MIDI, vendor, video; HID/CDC/MSC/MIDI host). The linker then never extracts those archive members. If anything ever needs another symbol from one of them, the link fails with "multiple definition": a build error, not a silent change. The stubs behave like a driver that finds no interface of its class (`open()` returns 0). The compiler checks their signatures against the installed headers.

What is left (11.6 KB): the audio IN FIFO 3104 B, the **unused** audio OUT FIFO 3104 B, usbh + hcd + hub (about 2.4 KB, still pulled in by `tusb.c`), CDC 776 B, the core's descriptor and string buffers about 1.2 KB, usbd about 540 B, dcd 244 B.

**Heap** (estimated from the sources; the spike prints `heap free / min-ever` at boot, so compare the two builds on the device): the `usbd` task stack 4096 B + TCB, the `arduino_usb_events` task 2048 B + TCB + queue, the CDC RX queue 256 B, mutexes, and the 166 B configuration descriptor (malloc in `tinyusb_load_enabled_interfaces`). Minus the USJ console's buffers. That comes to **about 7 KB**.

**Total for the instrument: about 19 KB** out of 66 KB free / 38 KB minimum-ever, so about 47 / 19 KB would be left. That is tight but workable. Without the trim it would be about 39 KB, which is not workable.

### 6.2 Further savings, if needed

- Vendor `audio_device.c` from `88c7f0b81` (MIT, about 1900 lines) with `CFG_TUD_AUDIO_ENABLE_EP_OUT 0` and an IN FIFO of 2048 B. That saves about 4.2 KB, and the per-ms driver code could be placed in IRAM. The threshold would then be 1024 B, which must stay above one block (512 B) plus jitter margin. Re-run the simulation with the new depth first.
- Stub the host stack's core (`tuh_*` used by `tusb.c`): about 2.4 KB. Riskier, because it touches what `tusb_init` calls.
- The 4 KB `usbd` stack and the 2 KB event task are fixed by the core (`esp32-hal-tinyusb.c:946`, `USB.h` defaults).

## 7. The spike (`env:usb-audio-spike`)

Sources: `src/usbspike/` plus `src/platform/audio_out.cpp` and `display.cpp` (unchanged). No lib/pg, no src/app, no `plat::sys` (its `HWCDC` calls do not compile in TinyUSB mode).

| File | What |
|---|---|
| `uac1_desc.h` | portable builder of the 91-byte UAC1 block (shared with the desktop checker) |
| `usb_desc_check.h` | portable byte-level validator (USB 2.0 framing, UAC1, TinyUSB driver rules, DWC2 budget); runs on the device at boot and on `d` |
| `usb_audio.{h,cpp}` | registration (static constructor → `tinyusb_enable_interface`), strong TinyUSB audio callbacks, `push()`, prefill, ISR counters, rate in RTC memory, download-mode reboot. `-DUSBSPIKE_NO_USB=1` turns it into stubs (baseline) |
| `usb_trim.c` | the linker diet (6.1) |
| `spike_main.cpp` | test signal, audio callback, LCD + serial status, commands, G0 |

What it does: the device enumerates as CDC + "Pocket Groove" stereo input. The `plat::audio` task (core 0, priority 20, 128-frame blocks) renders left 440 Hz and right 660 Hz at -12 dBFS, plus a 2 ms click on both channels every `rate` frames (one second of I2S time). It pushes the stereo frames to USB, then writes the mono sum to the codec, so the jack plays it. Once a second the LCD and the serial port show:

- enumerated / suspended / streaming, and stream opens;
- the rate and the rate the host set;
- packets per second and their sizes (×44 / ×45 / ×43);
- the device rate measured in the host's clock (frames per packet × 1000) and in ppm;
- FIFO level / depth / target;
- underruns (empty packets after data) and overruns (frames dropped because the FIFO was full);
- the I2S rate against the ESP crystal and I2S underruns;
- audio CPU %, heap free and minimum, the `usbd` stack, and the descriptor self-check result.

Serial commands: `h` help, `s` status, `d` dump plus validate the descriptors the host receives, `r` switch 44.1 ↔ 32 kHz (restart), `b` download mode. G0: tap switches the rate, hold 2 s enters download mode.

Offline verification done here:

- Both envs build with **0 warnings** (`-Wall -Wextra -Wdouble-promotion` on our sources).
- `sh host/usbspike/run_checks.sh` passes. It rebuilds the 166-byte configuration exactly as the core assembles it (TinyUSB's real `TUD_CDC_DESCRIPTOR` with `USBCDC.cpp`'s arguments, plus our block with the endpoint the core's allocator picks, 0x83), decodes it field by field, and validates it. 19 mutations per rate, each one caught, show the validator is not vacuous. It also runs the flow-control simulation of 4.3.
- `host/usbspike/analyze_recording.py` was tested on synthetic WAVs of the test signal: clean passes; a 44-frame dropout is caught as a -44 sample click interval and a glitch.

The image: `build/firmware/pocket-groove-usbspike.bin`, merged for offset 0 (bootloader + partitions + boot_app0 + app), in the main checkout. The same file is at `.pio/build/usb-audio-spike/firmware.factory.bin`.

## 8. Test procedure (macOS)

1. **Flash.** In VS Code / PlatformIO: `pio run -e usb-audio-spike -t upload`. Coming from the normal firmware, it waits about 5 s for a new port, then flashes. Without PlatformIO: `esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0x0 build/firmware/pocket-groove-usbspike.bin`. If nothing connects: power off, hold G0, plug USB, release, and repeat.
2. **Look at the device.** The LCD shows "USB enumerated" (green), "Audio idle (no app)", "Descriptor self-check: PASS", and a tone with a tick once a second on headphones (mono sum, quiet: -31.5 dB codec volume).
3. **Serial (optional).** Run `pio device list`, find the port described as "Pocket Groove", then `pio device monitor -p /dev/cu.usbmodemXXXX`. You get one `[usb] ...` line per second. Press `d` once and save the output to a file: `sh host/usbspike/run_checks.sh --dump that-file.txt` checks the real descriptor and compares it byte by byte with the offline one.
4. **Audio MIDI Setup** (Applications/Utilities): the device list shows **Pocket Groove** with 2 inputs and 0 outputs. Format: "2 ch 16-bit Integer 44.1 kHz" (32 kHz after a G0 tap). System Information → USB shows one device with both functions.
5. **Record 60 s in QuickTime:** File → New Audio Recording. In the menu next to the record button choose **Pocket Groove** as microphone and Quality **Maximum**. The LCD switches to "STREAMING" as soon as QuickTime opens the input, sometimes only once you press record. Record 60 s and save. During the recording the LCD should show: Pkts about 1000/s, sizes ×44 ≈ 900/s and ×45 ≈ 100/s, host-clock rate 44100 ± a few Hz (tens of ppm), FIFO around 1000-1900 / 3104, **Underruns 0, Overruns 0**.
6. **Check the file.** Listen: left is a 440 Hz tone, right a 660 Hz tone (a fifth higher), a tick every second, no crackle. Then convert and analyse: `afconvert -f WAVE -d LEI16 Recording.m4a rec.wav` (QuickTime may save `.mov`; same command), then `python3 host/usbspike/analyze_recording.py rec.wav` (needs numpy). It expects 440 / 660 Hz at about -15 dBFS RMS, click intervals all within 3 samples of each other, and no glitches. The mean click spacing in ppm shows whether QuickTime resampled the device clock (an offset is fine; a varying spacing is not). Repeat at 32 kHz (G0 tap, then reselect the device in QuickTime).
7. **Also try:** unplug and replug during a recording, quit QuickTime, sleep the Mac. The device must recover, and the counters must stop growing while nothing records. Also check that the jack keeps playing throughout.
8. **What failure looks like:**
   - No "Pocket Groove" in Audio MIDI Setup: enumeration or descriptor problem. Is the LCD "NOT enumerated" or the self-check FAIL? Send `d` and run the checker; check Console.app for `AppleUSBAudio` messages. Fallback build: `PLATFORMIO_BUILD_FLAGS=-DUSBSPIKE_TRIM=0 pio run -e usb-audio-spike -t upload` (no linker diet).
   - Device present but the recording is silent: the LCD never shows STREAMING (the host never selected alt 1), or it does but Pkts sizes are 0. Report both.
   - Crackles: the Underruns or Overruns counters grow (send the serial log), or the analyser lists irregular click intervals.
   - Wrong pitch: rate mismatch between descriptor and stream (the analyser's frequencies tell which).
   - One channel silent or the channels swapped.
9. **Back to the normal firmware.** Hold G0 for 2 s (or send `b`) until the LCD says DOWNLOAD MODE, then run `pio run -e cardputer-adv -t upload` (it finds the ROM's `303A:1001` port as usual). If the screen stays dark afterwards, unplug USB, switch off, switch on, plug in. If the spike is wedged: power off, hold G0, plug USB, release, upload.

Please report: steps 2, 4 and 5 (a photo of the LCD while recording is ideal), the analyser output at both rates, the serial lines, and the boot heap line of both `usb-audio-spike` and `usb-audio-spike-nousb`.

## 9. Unverified until it runs on hardware

1. macOS enumerates the composite device and AppleUSBAudio accepts this exact UAC1 descriptor: no IAD for the audio function, line-connector terminal type, no Feature Unit, one discrete rate. If not, try terminal type 0x0201 (microphone) first. That is the only field Espressif's tested template has differently, apart from the channel config.
2. The device name shown is "Pocket Groove". It comes from `USB_PRODUCT` (checked in `USB.cpp.o`) and the AC interface string; which one macOS shows is not verified.
3. The TinyUSB UAC1 IN path with flow control on the S3's DWC2 in slave mode (ISO IN odd/even frame handling, `dcd_dwc2.c`). The UAC1 branches of this driver (`tud_audio_n_version(i) == 1`) are newer than its UAC2 paths; how much they have run on an S3 is not known.
4. The real I2S-versus-host drift (expected tens of ppm), the delivered cadence, and zero under/overruns over 60 s, all from the LCD counters.
5. The upload paths of section 5: the 1200-baud touch, the port re-enumeration on macOS, esptool's hard reset leaving download mode, `b` and G0-hold reaching download mode, and `pio device monitor` reconnecting.
6. The CPU cost of the flash-resident USB interrupt on core 0, and its effect on the instrument's audio load (measure with the real engine; the spike's render is trivial).
7. The heap numbers of 6.1 (estimated from sources; the spike prints them).
8. `-DUSBSPIKE_TRIM=1` changes nothing functionally. Reasoned from `usbd.c` (each driver's `open()` is only asked to claim interfaces of its class), not observed.
9. macOS sleep/wake and unplug behaviour (no VBUS sensing on this board as far as known; unplug looks like suspend).
