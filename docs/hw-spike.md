# Hardware spike: tilt and sampling (`env:hw-spike`)

A test firmware that answers two questions before the playful redesign (docs/STATE.md, "Design direction") is built around them:

1. **Tilt.** Can the BMI270 IMU be read fast enough, quietly enough and without disturbing the keyboard and codec on the same I2C bus to be a playable control? How does "hold a key and tilt" feel?
2. **Sampling.** What does the built-in microphone sound like through the ES8311 ADC, does recording straight to flash work without dropouts, and what does playback from memory-mapped flash cost in CPU compared with RAM?

Sources: `src/hwspike/` (own full-duplex codec driver, BMI270 reader, flash sample store, the two pages), on top of `src/platform` system / display / keys and the smoke test's text canvas (`src/smoke/gfx.*`). It does not link lib/pg, uses the plain USB-Serial/JTAG serial port like the smoke test, and **never mounts LittleFS**. Portable parts (tilt maths, resampler, filter, level statistics, report format) are tested on the host: `ctest -R test_hwspike`.

## 1. Flashing, and going back

You are probably running the USB audio instrument (`cardputer-adv-usb`). Its port ignores esptool's reset, so first put it into download mode (docs/usb-audio.md 7.2):

```sh
pio run -e cardputer-adv-usb -t flashmode     # device reboots into the ROM download mode
pio run -e hw-spike -t upload
pio device monitor -e hw-spike               # 115200; keep it open, the reports go here
```

(From `cardputer-adv` or the smoke test, `pio run -e hw-spike -t upload` works directly.)

**Back to the instrument**, from the running spike (its port is the normal USB-Serial/JTAG one):

```sh
pio run -e cardputer-adv-usb -t upload        # or: pio run -e cardputer-adv -t upload
```

Your projects are there as before (section 2). If anything is wedged: power switch off, hold G0, plug USB, release, then the upload.

**Never** run `pio run -t erase`, `esptool erase-flash` or `-t uploadfs` for this: those wipe or overwrite the LittleFS partition with your projects. None of the steps above does.

## 2. What flashing this does to the device

| Flash range | What | Written by |
|---|---|---|
| 0x0000-0x7FFF | bootloader | every upload (same bootloader as the instrument) |
| 0x8000 | partition table | every upload; **byte-identical** to the instrument's (below) |
| 0x9000 + 20K | nvs | never |
| 0xE000 + 8K | otadata | every upload writes `boot_app0.bin` here (selects app0), as with every env |
| 0x10000 + 3.19M | app0: the running firmware | the upload (the spike replaces the instrument until you flash it back) |
| **0x340000 + 3.19M** | **app1, the unused second OTA slot** | **the spike at run time**: only 0x340000-0x50FFFF (headers, recording, 1 MB test sample; the rest is only read) |
| **0x670000 + 1.5M** | **"spiffs" = LittleFS with your projects** | **never** (not mounted, not erased, not written) |
| 0x7F0000 + 64K | coredump | never |

**Partition decision.** No custom table. The samples go into `app1` (ota_1) of the stock `default_8MB.csv`, which nothing in this project uses: there is no OTA code anywhere in `src/` or `lib/`, and every upload rewrites otadata so that app0 boots. An app partition can be erased, written and memory-mapped through the normal `esp_partition_*` API (that is how OTA itself works), so a data partition is not needed for the spike. This was chosen over the custom-table route because it changes **nothing** in the table: flashing the spike and flashing the instrument afterwards cannot move or resize anything.

Proof (this session, offline):

- `.pio/build/hw-spike/partitions.bin` has MD5 `801ba71678a964614657a6d8fbc6baca`, identical to `.pio/build/cardputer-adv/partitions.bin`, `.pio/build/cardputer-adv-usb/partitions.bin` and to `gen_esp32part.py default_8MB.csv` run on the framework's CSV.
- `gen_esp32part.py .pio/build/hw-spike/partitions.bin` prints: nvs 0x9000 20K, otadata 0xe000 8K, app0 0x10000 3264K, app1 0x340000 3264K, spiffs 0x670000 1536K, coredump 0x7f0000 64K.
- At run time `store::begin()` (src/hwspike/sample_store.cpp) refuses to touch flash unless ota_1 is exactly 0x340000 + 0x330000, the spiffs partition is exactly 0x670000 + 0x180000, the two do not overlap, the running app is not ota_1, and ota_1 is not read-only or encrypted. Every erase/write is checked again against the spike's own region (end 0x1D0000 inside app1), and the IDF partition API bounds-checks it once more. The SAMPLER page shows `store REFUSED <reason>` if any check fails, and nothing is written.
- After going back, app1 holds sample data instead of nothing. It is never booted: otadata (rewritten at every upload) selects app0.

For the real instrument a dedicated data partition (converting app1, keeping every other offset) would be the clean version, if OTA is never wanted; it needs no decision now.

## 3. The pages and keys

**Tab** (or G0) switches page. **Enter** prints the current page's report block to serial. **-** / **=** change the volume (2 dB steps, starts at -31.5 dB). Serial: `T` tilt report, `S` sampler report (plus the mic matrix block once a run finished), `A` both, `>` next page, `M` run the mic matrix, `Q` print its block.

### TILT

Lines: IMU rate (set, polls/s, new samples/s, duplicate polls, I2C time per 18-byte read); raw accelerometer (g) and gyro (dps); tilt angles A and B from the accelerometer and from the gyro-complementary filter; noise (std of the quietest 2 s window, degrees); gyro calibration / drift test; the held relative angles with bars; filter cutoff and resonance; preset; latency estimate; shake counter and lost samples; key INT-to-read latency with the IMU off and on.

A = rotation that moves gravity along the chip's x axis, B = along its y axis. Which way the chip sits in the case is not known yet; the report's `rest:` line (gravity vector with the device flat on the table) tells us.

| Key | Action |
|---|---|
| **Space (hold)** | engage tilt: A -> filter cutoff, B -> resonance, relative to the pose at the moment you pressed (works at any holding angle). Release: glides back. |
| 1 2 3 4 | presets: 1 `acc-fast` (accelerometer, 20 Hz smoothing, 0.5 deg dead zone, +-30 deg), 2 `acc-smooth` (6 Hz, 1.5 deg, +-35), 3 `fused` (gyro-complementary tau 0.4 s, 25 Hz, 1 deg, +-30; default), 4 `fused-wide` (tau 0.8 s, 12 Hz, 3 deg, +-60) |
| p / n | sound on/off / source (saw chord, noise, sine) |
| y | swap: A -> resonance, B -> cutoff |
| 5 6 7 | IMU rate 100 / 200 / 400 Hz |
| 8 | poll at 2x the rate (fewer lost samples, twice the bus traffic) |
| i | IMU polling off/on (for the key-impact comparison) |
| c | gyro bias calibration (2 s, keep still; also runs at boot) |
| z | drift test: 60 s, device still on the table |
| g | flip the gyro sign in the fusion (diagnostic only) |

### SAMPLER

Lines: store state; input level of both I2S slots (RMS with DC removed, peak held 1.5 s, meters -72..0 dBFS, `*` = the slot being recorded, `STUCK` if every sample is identical); gain preset; DC and clips; record state; last recording (length, peak, I2S overflows, ring drops, flash write count x chunk size, average / longest write, longest gap between input blocks); silence measurement; playback (voices per trigger, active voices, cycles per sample); benchmark progress / summary.

| Key | Action |
|---|---|
| 1 2 3 4 | mic gain: 1 PGA 0 dB (M5Unified's setting), 2 PGA 15 dB, 3 PGA 30 dB (default; Rust BSP / espp maximum), 4 PGA 30 dB + reg 0x16 = 0x24 (Espressif driver's digital gain) |
| l | record the left or the right I2S slot |
| n | measure 3 s of silence at the current gain (noise floor, peak, DC) |
| r | erase (about 1-2 s, audio glitches meanwhile), then record; r again stops; stops by itself at 5.9 s |
| w | flash write chunk 256 / 1024 / 4096 bytes (affects the stall per write) |
| a s d f g h j k | play the recording at -12 -7 -5 0 +3 +5 +7 +12 semitones |
| v | voices per trigger 1 / 2 / 4 / 8 (stacked at 0, +12, +7, +19, -12, +5, +24, +3) |
| p | the raw recording straight back, 1:1 |
| x | stop all voices |
| b | playback-cost benchmark, about 15 s (the first run writes a 1 MB test sample to flash first, a few seconds) |
| **m** | **mic matrix**, about 30 s, hands off: tries 24 microphone setups with a 1 kHz test tone on the speaker, tests the DIN pin electrically, prints the `MICMATRIX` block and, if a setup works, switches the page to it (section 4b). Serial: `M` runs it, `Q` prints its block again |

The recording stays in flash across reboots (and is played again by the next spike flash).

The line above the message line shows the matrix progress, then in green **`MIC WORKS (config N)`** or in red **`MIC DEAD IN ALL N CONFIGS`**.

## 4. What to do and send back

Headphones (the speaker is fine for the tilt test, but the mic will hear it). About 20 minutes.

1. **Boot.** Flash (section 1), open the monitor. The serial log shows `[tilt] BMI270 @0x69 ...` and `[sampler] store: OK ...`. If the store line says `REFUSED`, stop and send the log.
2. **TILT at rest.** Put the device flat on the table, untouched, for 10 s (the noise numbers want a quiet window; the boot calibration needs 2 s of stillness, press `c` if the line shows `nocal`/`CAL?`). Then press `z` and leave it for the full 60 s.
3. **Rates.** Press `7` (400 Hz), wait 5 s, then `8` (2x polling), wait 5 s, back to `8` and `6` (200 Hz). Note the `gaps` and the I2C time at each (or just send the report at each setting: Enter).
4. **Key impact.** With the IMU on, type on the keyboard for 20 s (lots of fast presses, some chords). Press `i` (IMU off) and do the same for 20 s, then `i` again.
5. **Play.** Hold Space and tilt; try presets 1-4, the sources, `y`. Decide which preset feels best and whether the range and dead zone are right. Shake it a few times (count should go up once per shake, not on normal tilting).
6. **Tilt report:** Enter. Copy the block from `===== PG-HWSPIKE TILT BEGIN` to `END`.
7. **SAMPLER, silence.** Tab. In a quiet room, for each gain 1-4: press the number, then `n`, keep quiet 3 s.
8. **Clap.** At gain 3: `r`, wait for REC, clap twice about 30 cm away, say a word, `r`. Listen: `p` (raw), then the pitch keys. If it clipped (red peak marker, `clips` > 0), repeat at gain 2; if it is very quiet, try gain 4. Also try `w` (chunk 4096) and record again: does the drop counter change?
9. **Voices.** Press `v` until x8 and hit a few pitch keys quickly; note the `play ... cyc/smp` line.
10. **Benchmark:** `b`, keep hands off for about 20 s.
11. **Sampler report:** Enter. Copy the block from `===== PG-HWSPIKE SAMPLER BEGIN` to `END`.
12. Flash the instrument back (section 1) and check a project loads.

### 4b. Mic matrix (the microphone returned a constant -1; this settles it in one flash)

Background: docs/hardware-notes.md 11.6. About 1 minute.

1. Flash this build (section 1), open the monitor (`pio device monitor -e hw-spike`).
2. **Unplug the headphones** (the speaker must play the test tone; with headphones in, the tone would be loud in your ears and the mic cannot hear it). Put the device on the table, screen up, and keep the room reasonably quiet.
3. Tab to SAMPLER and press **`m`** (or send `M` on serial). Do not touch it for about 30 s: it beeps at 1 kHz in bursts and clicks between setups. The line above the message line shows `matrix N/24 <name>`.
4. When it ends, that line shows **`MIC WORKS (config N)`** (green) or **`MIC DEAD IN ALL N CONFIGS`** (red), and the block `===== PG-HWSPIKE MICMATRIX BEGIN` ... `END` is printed on serial (send `Q` to print it again).
5. If it says MIC WORKS, the page now runs that setup: press `r`, clap / speak, `r`, then `p` and the pitch keys to listen. Also do the silence measurement (`n`) at gain 1-4 (step 7 of section 4) and one clap recording at gain 3 (step 8).
6. **Send:** the whole `MICMATRIX` block (copy from `BEGIN` to `END`, about 90 lines), then the SAMPLER block (Enter), and say how the recording sounds.

### Send (whole session)

**Send:** the two report blocks (or `A` on serial, which prints both), plus impressions: which tilt preset felt best, whether the dead zone / range / smoothing should change, latency felt OK or not, whether shake detection fired correctly, how the mic sounds (noisy, thin, boomy, distorted?) and which gain was usable.

## 5. Report fields

**TILT:** `imu` registers read back after configuration (ACC_CONF / GYR_CONF / ranges / power; `err` = ERR_REG, should be 0); `rate` (`new_hz` = fresh samples/s, `dup_pct` = polls that found no new sample, `gaps` = samples whose sensortime step exceeded 1.5 periods, i.e. lost samples; `odr_sensortime_hz` assumes 39.0625 us per sensortime tick, `ticks_per_sample` is the raw value); `i2c` (one 18-byte burst read: average / max, and a 1-byte read); `cpu` (IMU task time per poll including the bus wait, the maths in cycles, % of core 1; the audio task's time); `noise` (std of the quietest 2 s window: accelerometer-only and fused angles, gyro); `rest` (gravity vector when still, bias); `drift` (gyro-only integration vs the accelerometer after `secs`, and the fused estimate's error); `fusion` (`sign_check` +1 = gyro and accelerometer agree on rotation direction, -1 = the gyro sign is inverted relative to the maths, 0 = no motion seen); `latency_ms` (estimate: half a poll period + the smoothing's group delay per preset + one audio block + the TX DMA ring; the BMI270's internal filter delay is not included); `keys_imu_off` / `keys_imu_on` (keyboard INT to event read, per state); `keys` (drops, FIFO overflows, I2C errors, downs vs ups); `play` (preset, shakes).

**SAMPLER:** `store` (partition, LittleFS partition, running app, regions); `codec` (ES8311 registers read back); `live_L` / `live_R` (last full second per slot); `gain1..4` (silence: RMS with DC removed, peak, DC; last recording at that gain: peak, RMS, DC, clips, length); `rec` (samples, chunk, write count, average/max write time, erase time); `rec_drops` (`i2s_rx_overflows` = input DMA blocks lost while recording, `ring_drop_samples` = the flash writer fell behind, `max_rx_gap_us` vs `block_period_us`); `play` (live voice rendering); `bench_row: src voices poll_kb cyc_per_sample cyc_per_voice_sample max_block_cyc core_pct` and `miss_cyc_per_line`.

**MICMATRIX** (one block per run, also appended to the SAMPLER block once a run finished):

- `result:` the verdict, the winning config's name and slot, `applied=1` if the page now runs it, run time.
- `pin_boot:` GPIO46 (I2S data in) as `M5.begin()` left it, read before our first I2S call: `oe` output enable, `out` output level, `out_sel` GPIO-matrix output signal (256 = plain GPIO), `ie` input enable, `wpu` / `wpd` pulls, `mcu_sel` IO_MUX function, `level` pad level, `in_sel_i2s1_sd` the GPIO-matrix input select of the I2S1 data-in signal (46 = from GPIO46, plus 128 if routed through the matrix). **`oe=1 out=1` here is the root cause of hardware-notes 11.6.** `pin_i2s:` the same right after `i2s_channel_init_std_mode`.
- `pin_test:` fraction of 20,000 pad reads that were 1, with the codec running and its clocks on: `as_found` (pin as at boot), then output driver off with pull-up `pu` / pull-down `pd`, then the codec's serial output muted (reg 0x0A bit 6) with pull-up / pull-down. `verdict`: `codec_drives_line` (the line does not follow the pulls, or reads 0 under the pull-up while muted), `line_floats_codec_silent` (follows the pulls), prefixed `esp_drove_din_itself+` if the ESP's own output was on.
- `chip:` ES8311 regs 0xFD-0xFF. `regs_before_XX:` / `regs_after_XX:` all registers 0x00-0x45 before the run and after the winner was applied (16 per line, from register XX).
- `cfgNN:` one setup: `path` (our full-duplex driver, or the M5.Mic control), `din` (as-found / oe-off = the fix / forced-high / oe-off+pd), slot width, rate, whether the tone played, `loopR` (reg 0x44 = 0x58 sends the DAC to the R slot: an electrical proof that the codec drives ASDOUT; judged on L only), the better slot, its score and five registers read back after the capture. `cfgNN_L:` / `cfgNN_R:` 0.5 s capture per slot (a whole number of tone periods): `stuck` (every sample equal), `min` / `max`, `dc` (LSB), `rms` (dBFS, DC removed), `pk`, `tone` (level of the 1 kHz component, dBFS), `snr` (tone vs everything else, dB), `alive` (not stuck, >= 8 LSB span, RMS > -87 dBFS), `tone_ok` (alive, tone > -90 dBFS and >= 10 dB above the rest).
- `rank:` config numbers, best first: alive first, then tone heard, then tone SNR; equal scores keep the table order.

The 24 configs: 0 the spike as it was; 1 + DIN output driver off (the fix); 2 DIN forced high (repro); 3 output off + pull-down; 4 / 5 M5Unified's speaker + mic sequence with / without the fix; 6 esp_codec_dev's open + set_fs + start; 7 the same with the DAC reference in the R slot; 8 esp-bsp's es8311_init + microphone_config; 9 spike + DAC loopback (0x44 = 0x58); 10 / 11 32-bit slots (64 fs) with x8 / x4; 12 16-bit slots with x4 (wrong ratio, control); 13 DMIC on (control); 14 MIC2 input; 15 serial output muted (control: should read 0); 16 24-bit word; 17 Espressif's bias / HPF / ramp / 0x44 / 0x45 registers; 18 digital gain 0x16 = 0x24, 0x17 = 0xC8; 19 full codec reset first; 20 codec written before the I2S clocks start; 21 slow power-up with 100 ms settles; 22 / 23 M5Unified's own `M5.Mic` (port 0, 16 kHz, right slot, its exact callback; our driver shut down) as is / with the fix. Spike-based setups use PGA 18 dB; the reference sequences keep their own gain. During the run the DAC is at -8 dB and the tone at -12 dBFS; afterwards your volume is restored.

The benchmark: one block = 128 frames at 44.1 kHz; voices loop over their source at +7 semitones (ratio 1.498, so the interpolation is exercised), started evenly spread over it. Sources: `ram48k` = 48 KB copy of the test sample in internal RAM; `flash48k` = the same 48 KB read through the mapping; `flash1m` = the whole 1 MB mapped test sample, voices up to 128 KB apart (sequential with 1 voice, cache-hostile with 8). `poll_kb` = before each block, that many KB of other mapped flash are read one byte per 32-byte cache line, marching through 256 KB (eight times the 32 KB data cache), so the voices' lines are evicted as they would be by an engine that reads flash tables in between; its time is not counted, but it gives `miss_cyc_per_line`, the cost of one data-cache line fill from flash. The voice loop runs from IRAM, as the instrument's hot path does, so the numbers are about data, not code.

## 6. How it works (short)

- **Audio:** one I2S port in full duplex (TX + RX share BCLK 41 / WS 43; DOUT 42, DIN 46), 16-bit stereo slots, 44.1 kHz, 6 DMA buffers of 128 frames each way (17.4 ms, so a ~9 ms flash stall cannot overflow the input). Audio task on core 0, priority 20: read input block, render, write output block. ES8311: the instrument's DAC sequence plus the ADC sequence of the device-tested Rust BSP (0x01 = 0xBF, 0x0A = 0x0C, 0x0E = 0x02, 0x14 = 0x10 | PGA, 0x17 = 0xBF, 0x1C = 0x6A); reg 0x16 is left at its power-on value except in gain 4.
- **IMU:** M5.begin() uploads the BMI270 config file (M5Unified); the spike then writes ACC_CONF / GYR_CONF in performance mode at the chosen rate, +-4 g, +-1000 dps (values from the Rust BSP), and an esp_timer wakes a task on core 1 (priority 9, below the keyboard task) that reads 18 bytes from 0x0C at 400 kHz (accel, gyro, sensortime, data-ready flags). The maths runs there; the audio task reads two atomics.
- **Recording:** `r` stops playback, erases the header sector and 512 KB, unmaps the region, then the audio task pushes the selected slot into a 16384-sample RAM ring while a writer task (core 1, priority 5) streams chunks of 256 / 1024 / 4096 bytes into the pre-erased flash and finally writes a header (length, gain, peak, CRC) and maps the region again.
- **Playback:** linear-interpolating integer voices (32.32 position) read straight from the mapped flash pointer; nothing is copied.
- **Mic matrix** (src/hwspike/mic_matrix.cpp, tone maths in tone.h): a job task on core 1 restarts the duplex driver once per setup (`duplex::restart` with the setup's slot width, DIN mode, codec sequence and order), lets it settle 250-600 ms while the audio task plays the tone, then the audio task correlates 22,050 frames per slot with an exact 441-sample (10-period) 1 kHz table: tone amplitude, residual and SNR with no leakage. The M5.Mic controls shut our driver down and use `M5.Mic.begin()` / `record()` / `end()`. Ranking and the report are portable and host-tested; the winner is restarted with your volume and gain preset. It writes no flash.

## 7. Verified offline / unverified

Verified here (mic matrix, 2026-11-12): `pio run -e hw-spike -e cardputer-adv-usb` builds with no warnings in the spike's sources, partitions.bin still has MD5 `801ba716...`; `test_hwspike` covers the tone table at 44.1 / 32 / 16 kHz, the generator against `sin`, the detector (exact amplitude and DC of a pure tone, SNR of a tone 20 dB under white noise, a quiet tone 16 dB above noise found, other frequencies rejected, constant -1 and 1-LSB toggling judged dead), the ranking / verdict text and the MICMATRIX block (fixed markers, no truncation at worst-case values).

Verified here (first version): `pio run -e hw-spike` builds with no warnings in its sources; `cardputer-adv`, `cardputer-adv-usb` and `smoke` still build; partition table byte-identical (section 2); `test_hwspike` (ctest) covers the relative-tilt frame at arbitrary holding angles, the complementary filter (tracks a 1 rad/s turn, halves accelerometer noise, a flipped gyro sign diverges), the mapping (dead zone, range, clamp, glide back), shake detection (shake yes, slow tilt and single bump no), the resampler (bit-exact at 1.0, interpolation, pitch, 32.32 position accuracy, loop/one-shot end), the SVF at every extreme, the level statistics and both report formats (markers, fixed line counts, no truncation).

Not verified (needs the device):

1. **The microphone.** Measured 2026-11-12: constant -1 in both slots at every gain. Root cause found in the sources (hardware-notes 11.6): `M5.begin()` leaves GPIO46 an output driven high, and ESP-IDF 5.5's I2S driver no longer switches that driver off (5.4 did), so the receiver reads the ESP's own 1s; this is espressif/esp-idf#18621. Not yet confirmed on the device: the mic matrix (section 4b) does that, with the fix as config 1.
2. Which I2S slot carries the ADC (the Rust BSP takes the left one; both are measured) and whether reg 0x16's power-on value and the 0x24 preset mean what Espressif's driver says.
3. That full duplex on one port with this codec is glitch-free (espp and the Rust BSP do it; not this code).
4. The BMI270 register values beyond what the Rust BSP and espp state (ERR_REG is printed to catch a bad combination); the sensortime tick of 39.0625 us is from memory of the datasheet, so `odr_sensortime_hz` is only as good as that (the raw ticks are printed).
5. That the gyro sign convention matches the maths (`sign_check` answers it), and the chip's orientation in the case.
6. I2C sharing: M5GFX's I2C layer locks per transaction and busy-polls the bus with `taskYIELD`; the key-latency numbers will show what the IMU costs the keyboard.
7. That reading memory-mapped app1 through the data cache works as on any partition (IDF documents mmap for any partition; app1 being an app slot is not special to the MMU), and the mapped size (1.8 MB, 29 MMU pages) fits next to the app's own mappings. A failure shows as `store ... mmap failed`.
8. Flash write stall per chunk size and whether 6 x 128-frame input buffers ride it out (that is what `rec_drops` measures).
9. The latency figure is an estimate from its parts, not a motion-to-sound measurement.
