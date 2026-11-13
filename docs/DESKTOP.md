# Pocket Groove on the desktop

`pocket-groove` is the instrument in a window on your Mac (or Linux PC): the same engine, sequencer, UI and file format as the Cardputer firmware, compiled from the same `lib/pg` code. Program sounds and patterns here, then take the project to the device with `tools/pg push`; or review every instrument and setting without flashing firmware.

It is not a separate desktop UI. The window shows the device's 240x135 screen, the Mac keyboard stands in for the Cardputer keyboard key for key, and what you hear is what the device's engine renders (in stereo, or in the device's mono with `--mono`).

Under the screen is a thin strip with the only three things the desktop adds (see [The strip](#the-strip)): a legend that lights the Cardputer modifiers while you hold them, **Listen** (Cmd+L: hear a connected Cardputer through the Mac) and **Stems** (Cmd+B: pick a project on the device or on disk and bounce its stems).

## Install (macOS)

```bash
brew install sdl2 cmake
```

You also need the Xcode command-line tools (`xcode-select --install`) for the compiler. Nothing else: no PlatformIO, no Python.

What `brew install sdl2` installs: Homebrew has no separate SDL2 formula any more; `sdl2` is an alias of **sdl2-compat** (2.32.x, the SDL2 interface implemented on top of SDL3, which it pulls in as `sdl3`). It installs the SDL2 headers, `sdl2-config`, `sdl2.pc` and a CMake package (`lib/cmake/SDL2/SDL2Config.cmake`, target `SDL2::SDL2`), which is what the build looks for; CMake finds it under `/opt/homebrew` (Apple silicon) or `/usr/local` (Intel) without any setting. The app prints the SDL version it runs on at start (2.32.x for sdl2-compat). If something in the window, keyboard or audio misbehaves and you suspect the compatibility layer, build a plain SDL2 from source with the app instead: `cmake -S host -B build/host -DPG_FETCH_SDL2=ON` (downloads SDL 2.32.10 from GitHub, a few minutes once; `tools/pg-desktop` keeps that setting).

On Debian/Ubuntu: `apt install libsdl2-dev cmake g++`.

## Run

From the repository root:

```bash
tools/pg-desktop
```

The first run builds the app (about a minute); later runs rebuild only what changed, so after pulling new code the same command gives you the new version. Options go after it, e.g. `tools/pg-desktop --mono --scale 3`.

Quit by closing the window or with Cmd+Q (or Ctrl+C in the terminal). The session is saved on the way out, exactly as the device saves it, and the next launch resumes it.

The first launch behaves like the device's first boot: the factory song ACIDJAM is loaded, stopped, with the `SPACE PLAY` banner. All three factory songs are in the PROJECT view (`\`) under ROM.

## The keyboard

Keys are mapped by **position**, not by the letter printed on them, so the layout of your Mac (US, German, French, ...) does not matter: the key where a US keyboard has `Q` is always the Cardputer's `q`. Letters, digits and punctuation are the same keys as on the Cardputer. The Cardputer's modifiers sit where the Mac's are:

```
  G0 (side button) = F1 or esc     record = F9 or cmd+R     stats = F2 or cmd+I     listen = cmd+L     stems = cmd+B

  ┌─────┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬──────────┐
  │  `  │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │ 0 │ - │ = │  delete  │
  │     │   │   │   │   │   │   │   │   │   │   │   │   │   DEL    │
  ├─────┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬────────┤
  │  tab  │ q │ w │ e │ r │ t │ y │ u │ i │ o │ p │ [ │ ] │   \    │
  │  TAB  │   │   │   │   │   │   │   │   │   │   │   │   │        │
  ├───────┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴────────┤
  │  caps  │ a │ s │ d │ f │ g │ h │ j │ k │ l │ ; │ ' │  return   │
  │   --   │   │   │   │   │   │   │   │   │   │   │   │   ENT     │
  ├────────┴──┬┴──┬┴──┬┴──┬┴──┬┴──┬┴──┬┴──┬┴──┬┴──┬┴──┬┴───────────┤
  │   shift   │ z │ x │ c │ v │ b │ n │ m │ , │ . │ / │   shift    │
  │   SHIFT   │   │   │   │   │   │   │   │   │   │   │    FN      │
  ├─────┬─────┼───┴─┬─┴───┴───┴───┴───┴───┴───┴─┬─┴───┼───────┬────┘
  │ fn  │ctrl │ opt │cmd│        space        │cmd│  opt  │
  │ --  │CTRL │ OPT │-- │        SPACE        │-- │  ALT  │
  └─────┴─────┴─────┴───┴─────────────────────┴───┴───────┘
        upper line: the Mac key     lower line: the Cardputer key (blank = the same key, -- = nothing)
```

| Cardputer | Mac |
|---|---|
| `shift` (`Aa`) | left Shift |
| `fn` | **right** Shift |
| `ctrl` | Control (either side) |
| `opt` | **left** Option |
| `alt` | **right** Option |
| `tab` | Tab |
| `del` | Delete (the backspace key) |
| `ent` | Return (and keypad Enter / fn+Return) |
| `space` | Space |
| G0 (side button) | F1 or Escape |

| App command | Keys | Strip button |
|---|---|---|
| record on / off | F9 or Cmd+R | REC |
| audio stats (in the strip) | F2 or Cmd+I | STATS |
| Listen to the connected device | Cmd+L | LISTEN |
| the Stems list | Cmd+B | STEMS |
| macro lab (with `--lab` only): mark good / bad, previous / next set, macro +- , macro 1..4 | Cmd+G / Cmd+D, Cmd+Left / Right, Cmd+Up / Down, Cmd+1..4 | the lab panel ([MACROLAB.md](MACROLAB.md)) |

On a MacBook or Apple keyboard the top row is brightness, volume and media keys: there, F1 / F2 / F9 need the keyboard's `fn` (globe) key held (or *Use F1, F2, etc. keys as standard function keys* in System Settings > Keyboard). Escape, Cmd+R and Cmd+I work without it.

Command (⌘) never reaches the instrument as a key. Cmd+R, Cmd+I, Cmd+L and Cmd+B are the app's shortcuts (none of those letters is played, on the press or the release); everything else with Command is macOS's (Cmd+Q quits, Cmd+Tab switches apps, Cmd+H hides, Cmd+M minimises, Cmd+W closes) or nothing. Pressing Command releases every key the app holds, so Cmd+Tab in the middle of a chord leaves nothing stuck. When the window loses focus, every held key is released too (no stuck notes, no stuck modifiers). Holding a key does not auto-repeat in the OS sense: the instrument does its own repeat on `-` / `=` as on the device.

On ISO keyboards (with a key left of `Z`), that key and the key left of `1` both act as `` ` ``.

G0 does what it does on the device's UI: nothing. The firmware's G0 gestures (volume, diagnostics strip) are not part of the desktop build; G0 + `-`/`=` is swallowed as on the device so it cannot change a value by accident.

The Cardputer's top-row legends in the manual (`9` SOUND, `0` MIX, `o` FX, `p` SONG, `\` PROJECT, `` ` `` home, `/` help) work unchanged. Everything in [MANUAL.md](MANUAL.md) chapter 4 applies key for key.

### Changing the map

`tools/pg-desktop --keymap my.keys` reads overrides, one per line:

```
# <Cardputer key> = <SDL scancode name>
fn = Right Alt         # FN on right Option instead ...
alt = Right Shift      # ... and ALT on right Shift
= = Keypad +           # a second key for =
record = F10           # F10 records as well
none = F2              # F2 does nothing
stats = Cmd+S          # a Command shortcut (record and stats only)
none = Cmd+I           # remove one
```

Cardputer keys by legend as in the manual (`` ` `` `1` ... `del` `tab` `q` ... `fn` `sft` `a` ... `ent` `ctl` `opt` `alt` `z` ... `spc` `G0`, or `shift`, `space`, `enter`, `backspace`, ...), plus the app commands `record`, `stats`, `listen`, `stems` and `none`. `Cmd+<name>` makes a Command shortcut; those can only be app commands, never a Cardputer key. Scancode names are SDL's (`A`, `1`, `Left Shift`, `Right Alt`, `Backspace`, `F1`, `Keypad +`, ...; SDL calls Option "Alt" and Command "GUI"). `tools/pg-desktop --print-keymap` prints the map in force.

## Options

| Option | |
|---|---|
| `--dir PATH` | projects directory (default `./projects`) |
| `--samples PATH` | the sample pool's folder (default: `samples/` next to the projects directory; below) |
| `--sd PATH` | also mount `PATH` as the SD card (default: no card, as a Cardputer without one) |
| `--scale N` | window size: N x 240 by N x 135 points (default 4). The window can be resized; the picture stays sharp at whole multiples |
| `--buffer N` | audio buffer in frames (default 256 = 5.8 ms at 44.1 kHz). Lower for less latency, higher if you hear clicks |
| `--sr 32000` | run the engine at the device's fallback rate (default 44100) |
| `--mono` | the device's mono mode: what its headphone jack plays (one bus, pan ignored, mono delay and reverb) |
| `--device-keys` | model the Cardputer's key matrix: phantom keys appear and the phantom filter removes them, as on the device (see below) |
| `--keymap FILE` | key map overrides (above); `--print-keymap` prints the map |
| `--rec-dir PATH` | where F9 recordings go (default `./bounces/live`) |
| `--record` | start recording at launch |
| `--lab [DIR]` | the macro lab, the workbench where instruments are tuned: a panel right of the screen, `.pgm` files from `DIR` (default `./macros`) hot-reloaded and replacing the built-in instruments of the same voice and name, marks to `./lab-notes.txt` (`--lab-notes FILE`); opens at scale 3; implies `--dev`. See [MACROLAB.md](MACROLAB.md) |
| `--dev` | developer mode: the SOUND view can also show the bare parameter pages (CLASSIC tracks show them; `l` flips between them and the instrument page). Never on the device |
| `--script FILE`, `--out DIR` | play a key script (as `host/tools/ui_shot` scripts, see `host/app/script.h`) and write its `shot`s to DIR |
| `--pg-tool PATH` | the `tools/pg` the Stems list runs (default: the one in this repository; or `$PG_TOOL`) |
| `--pg-port PORT` | the device's serial port for the Stems list (default: found automatically; or `$PG_PORT`) |
| `--listen-dump FILE.wav` | Listen also writes the device's audio exactly as SDL delivers it (before any conversion; the format, channels and rate SDL gave are in the WAV header). For troubleshooting, below; a re-plug starts `FILE-2.wav` |
| `--listen-device NAME` | Listen to another input: its exact name or a part of it (the terminal lists the inputs when Listen starts) |
| `--listen-sim PPM` | for tests: Listen plays a simulated device (a 1 kHz tone) whose clock runs PPM fast; `--listen-sim-spec FMT:CH:RATE:FRAMES` makes it deliver another format than asked for (e.g. `f32:1:48000:1024`), `--listen-sim-burst N` N callbacks back to back, `--listen-sim-vary` varying callback sizes |

## Projects and the device

The projects directory (`./projects`) is the device's internal flash as far as the instrument is concerned: SAVE AS in the PROJECT view writes `projects/NAME.pgv`, the list shows every valid `NAME.pgv` there, and the autosaved session is `projects/_last.pgv` (with the UI settings in `projects/_ui.cfg`). The files are byte for byte what the device writes (the same `pg::io` code and format version).

It is also the directory `tools/pg` uses, so moving a project is one command each way (device plugged in, from the repository root):

```bash
tools/pg pull MYSONG            # device -> projects/MYSONG.pgv: open it in the PROJECT view
tools/pg push projects/MYSONG.pgv   # desktop -> device's internal flash
tools/pg pull session           # what the device was playing -> projects/SESSION.pgv
```

`tools/pg push` refuses while the device is playing (stop it, or `--force`). The desktop app does not need to be closed for either; after a pull, press `\` twice (or leave and re-enter PROJECT) to refresh the list.

There is no SD card unless you start with `--sd PATH` (any directory; it plays the card's role, `NAME.pgv` files directly in it). Without it the PROJECT view behaves as on a Cardputer without a card.

## Recording

F9 (or Cmd+R, or the strip's REC) starts recording what you hear to `bounces/live/YYYYMMDD-HHMMSS.wav` (16-bit stereo at the engine's rate; in `--mono` both channels are the same); the same key again stops. The REC button shows the time while it runs. Quitting stops and closes the file. The recording is the engine's output sample for sample, live moves included, before the Mac's volume; while Listen is on, the device is in it too (it records what you hear). (`tools/pg bounce` and the Stems list render a saved song to stems instead; they do not capture live moves.)

## The strip

```
  LSHFT RSHFT CTRL  LOPT  ROPT  TAB   ESC        <- the Mac key
  SHIFT  FN   CTRL  OPT   ALT   TAB   G0         <- the Cardputer modifier: lit while held
  [LISTEN ⌘L] [STEMS ⌘B] [REC ⌘R] [STATS ⌘I]     <- click, or the shortcut
  DEVICE  LISTENING 15 MS +61 PPM                <- device status, Listen (or the stats)
```

- **Modifier legend.** Each Cardputer modifier with the Mac key it is on. A chip lights while the app's key mapper holds that Cardputer key down, which is exactly what the instrument receives, so it doubles as a check that a modifier registers (useful for right Shift / right Option, and after `--keymap` changes).
- **Controls.** LISTEN, STEMS, REC and STATS can be clicked or used with their shortcuts. A click never reaches the instrument; clicks on the device screen do nothing.
- **Status.** `DEVICE` (green) when a Cardputer's USB audio input ("Pocket Groove") is connected, `NO DEVICE` otherwise; this is the audio device, so a Cardputer with the jack-only firmware shows `NO DEVICE` here even though the Stems list can reach it over USB serial. Then Listen's state: `LISTEN OFF`, `LISTENING 15 MS +61 PPM` (the latency Listen adds and the measured clock difference), or a hint (below). With STATS on, this line shows the audio stats instead: callback load (smoothed / peak %), late callbacks, gaps, the buffer the output device gave, the pixel factor (8X on a Retina screen at the default size).

The window title shows the project name (with `*` when it has unsaved changes).

## Listen: hear the device through the Mac

With the USB audio firmware (default since 2026-11-12, docs/usb-audio.md) the Cardputer is a stereo USB microphone called **Pocket Groove**. Cmd+L (or LISTEN) plays that input through the Mac's output, mixed with the app's own engine, which keeps running (stop the app's sequencer, or not, as you like). Cmd+L again stops it. No QuickTime or monitoring app needed.

- It finds the input by name in the Mac's list of audio inputs, checks every two seconds and when macOS reports a device change. Unplugging while listening shows `UNPLUGGED: WAITING` and Listen picks the device up again when it comes back; nothing to restart.
- **Latency**: what the Mac's audio needs, measured: Listen watches how the input and the output callbacks really arrive (macOS hands audio over in bursts of several buffers) for half a second, then keeps the largest gap between two input deliveries plus the largest output burst plus 3 ms in hand. Typically 20 to 60 ms on top of the app's output; the strip shows it. If a bigger burst comes later it adds the difference, gliding the read rate by at most 0.1 % for a few seconds instead of dropping out.
- **Clocks**: the Cardputer's audio clock and the Mac's are independent crystals (the device measured +61 ppm), which over minutes drift apart by whole buffers. Listen continuously adjusts how fast it reads the device's audio (by at most 0.2 %, inaudible) to keep its buffer at the target fill: no dropouts and no growing delay, however long it runs. If the Mac stalls long enough for the buffer to pile up, it skips back to the target once.
- **Formats**: it asks for 16-bit stereo at the app's rate and converts whatever it gets (float or integer, any channel count, 32 / 44.1 / 48 / 96 kHz, any callback size) itself; the terminal says what was asked, what came and the conversion.
- **Level**: the device is added to the app's engine at full level. If both are loud, the sum clips (it saturates, never wraps around); the counters below count clipped samples. Stop the app's sequencer while listening to the device alone.
- **Microphone permission.** macOS treats any audio input as a microphone. The first Cmd+L makes macOS ask whether **Terminal** (or iTerm, or whichever app started `tools/pg-desktop`: the prompt names that app, not Pocket Groove) may use the microphone. Allow it. If it was refused, macOS delivers only silence: the strip then says `SILENT: STOPPED? MIC ALLOWED?` after three seconds without signal (which is also what a stopped device sends: press play on the device to tell the two apart). To change it: System Settings > Privacy & Security > Microphone, switch on your terminal app, then quit and restart the terminal and `tools/pg-desktop`.
- Other hints: `NO POCKET GROOVE INPUT` (no such input: the device is off, on a charge-only cable, or running the jack-only firmware), `WON'T OPEN: MIC PERMISSION?` (macOS refused to open the input; the reason is printed in the terminal).
- The input is opened at the app's rate (`--sr`); if the device runs at the other rate (32 kHz from its boot menu), SDL or Listen converts.
- **What it prints.** When Listen starts, the terminal gets one diagnostics block: the SDL version and whether it is sdl2-compat (Homebrew's `sdl2` is the SDL2 API on SDL3), the audio driver, every audio input's name, what was asked of the capture device and what it gave, the same for the output, and the conversion chosen. 1.5 s later the callbacks as they really come (sizes, bursts, gaps) and the target that follows; then every 5 s (and at once when STATS is switched on) the counters: fill, target, read rate, clock difference, underruns, overflows, resyncs, the device's peak level before and after mixing, clipped samples. With STATS on, the strip alternates the audio stats with Listen's (`U` underruns, `O` overflows, `R` resyncs, `CLIP`, latency, clock).

## Stems: bounce a project from the list

Cmd+B (or STEMS) opens a list over the screen (framed in cyan: it is the app's, not the device's). While it is open the keyboard belongs to the list and no instrument key stays held.

- **ON THE DEVICE**: the projects on the connected Cardputer, its autosaved session first (what it was playing at power-off), with tempo and FLASH / SD. **IN PROJECTS/**: the `.pgv` files in the projects directory (`--dir`), the desktop's own session first.
- Up / Down choose, Return bounces the chosen project, Esc closes (a running bounce carries on; Cmd+B shows it again).
- A device project is pulled first (into `bounces/NAME/NAME.pgv`, next to its stems; `./projects` is not touched), then rendered with the bounce's defaults (the song chain, dry stems + returns + mix, 32-bit float, 44.1 kHz: MANUAL.md chapter 15) into `bounces/NAME/` (the device's session: `bounces/SESSION/`, the desktop's: `bounces/DESKTOP-SESSION/`). Progress shows under the list; when it is done the folder opens in Finder and the list says where the files are. For other bounce options use `tools/pg bounce` in a terminal.
- It runs `tools/pg --json` in the background (`ls`, `bounce`), so it needs what `tools/pg` needs: Python 3 with pyserial (PlatformIO's Python has it; MANUAL.md chapter 15) and, the first time, a minute to build the renderer. The window and the sound never wait for it.
- Errors are shown in the list in plain words: device not found, port busy (close the serial monitor), a project that does not load, pyserial missing. The device is found over USB serial, so this works with the jack-only firmware too.

## The macro lab

`tools/pg-desktop --lab` adds a development workbench beside the screen for tuning the instruments: `.pgm` files under `macros/` are reloaded while the app plays and replace the built-in instruments of the same voice and name, the selected track's instrument gets a slider per macro and an XY pad (they move the track's own macros, the ones the SOUND page shows), and Cmd+G / Cmd+D write "good" / "bad" lines with every position and value to `lab-notes.txt`. Off unless asked for. Everything about it, the file format and the loop: [MACROLAB.md](MACROLAB.md).

## Differences from the device

- **Stereo** by default (the device has one DAC channel: `--mono` to hear exactly that).
- **No phantom-key filter.** The Cardputer's keyboard has no diodes, so some three-key combinations make it report a fourth key, and the UI filters those (`docs/ghosting-audit.md`). A Mac keyboard has no such matrix, so the filter is off and every combination works. `--device-keys` turns on a model of the Cardputer matrix and the filter: use it to check that a gesture you rely on works on the real device.
- **No boot menu, diagnostics strip, codec volume or backlight.** Use the Mac's volume. The status-bar battery icon is empty (unknown) and the low-battery save never fires.
- **Timing.** Keys reach the instrument the moment they arrive and the screen updates at the device's ~30 fps, as on the device. The audio latency is the buffer (`--buffer`) plus macOS's output path; the device's is about 15 ms.
- **Sample rate conversion.** The engine runs at 44.1 kHz (or 32 kHz); if the Mac's output device is set to 48 kHz, SDL converts.
- **CPU.** The `cpu` figure the UI gets is the desktop's load (F2 shows it in the title bar), not the device's; for the device's budget see `bench_instrument` and the device's diagnostics strip.

## Troubleshooting

- *No keys reach the window* after starting from Terminal: click into the window once (macOS sometimes leaves keyboard focus with Terminal for a binary that is not an app bundle).
- *Clicks or dropouts*: `--buffer 512`. F2 / Cmd+I shows the audio callback's load and counts late callbacks, the buffer the output device actually gave, and the pixel factor of the picture (8x on a Retina screen at the default size), in the strip.
- *F9 changes the volume or skips a track*: use Cmd+R, or fn + F9 (see the note on function keys above).
- *Listen is silent*: see the microphone permission above (System Settings > Privacy & Security > Microphone, your terminal app).
- *Listen sounds wrong* (distorted, crackling, wrong pitch), or a Cmd+R recording made while listening does: stop the app's sequencer (so only the device is heard), play something steady on the device, and run

  ```bash
  tools/pg-desktop --listen-dump ~/Desktop/listen-dump.wav 2>&1 | tee ~/Desktop/listen-log.txt
  ```

  then Cmd+L, Cmd+R, 10 seconds, Cmd+R, Cmd+L, quit. Send: `listen-log.txt` (the diagnostics block, the callback sizes and the counters), `listen-dump.wav` (what macOS really delivered, untouched) and the recording from `bounces/live/`. The dump answers whether the device's audio reaches the app clean; the recording, whether it stays clean through the app; the log, why not. `build/host/listen_analyze FILE.wav --from 1 --seconds 5` fits a test tone in either file (frequency, level, THD+N).
- *The Stems list says pyserial is missing*: see MANUAL.md chapter 15 (`tools/pg` needs it; PlatformIO's Python has it, or set `PG_PYTHON`).
- *Control+Space switches the input source*, or *Control+arrow / Control+number switches Spaces*: macOS keyboard shortcuts take those before any app; turn them off in System Settings > Keyboard > Keyboard Shortcuts if you need the combination.
- *`SDL2 not found`*: `brew install sdl2`, then run `tools/pg-desktop` again (it re-runs CMake).

## Samples

The sampler instruments on PERC (CHOP, TAPE, STUTTER; MANUAL.md chapter 20) play one of twelve sample slots. On the desktop the slots are the folder **`samples/`** next to the projects directory (`./samples` with the default `--dir projects`; `--samples PATH` for another), one audio file per slot, named by the slot number:

```
samples/01-ohyeah.wav     slot 1
samples/03-hey there.wav  slot 3 (01..12, then '-', '_' or '.', then any name)
```

- They are loaded when the app starts and the folder is checked every 0.4 s: a new, changed or removed file reloads its slot (a file changed less than 0.7 s ago is left until it has settled, so a copy in progress is not read half). The terminal says what each slot holds (`[samples] slot 3: HEYTHERE 1.42 s`).
- Any WAV: 8/16/24/32-bit PCM or 32/64-bit float, mono or stereo (or more channels: mixed down), any sample rate. It is converted on load to the slot's format: mono, 22,050 Hz (a windowed-sinc resampler), silence trimmed from both ends (below -48 dB of the peak, keeping 5 ms before and 20 ms after), cut to 6.2 s, levelled to a -1 dBFS peak. The name the screen shows is the file name's, upper case, letters and digits, up to 8 characters.
- A slot with no file, or a file that does not convert (the terminal says why), plays its **built-in sound**, as an empty slot on the device does.
- **Drop a file on the window**: it goes into the selected track's slot (the slot SHIFT+`;` / `.` chose on the instrument page). A WAV is copied into `samples/` as `NN-name.wav` (the slot's other files there are removed); on a Mac any other audio file is converted to WAV with `afconvert` first. A toast says `SLOT 3: HEYTHERE`, or what went wrong.
- The desktop's slots and the device's are separate: `tools/pg sample put 3 samples/03-hey.wav` copies one to the device (MANUAL.md chapter 20). The C++ converter here (`host/common/sample_convert.h`) and the Python one `tools/pg` uses (`tools/pg_sample.py`) are the same algorithm; ctest `test_sample_convert` checks that they give the same samples.

## How it is built

`host/app/` (target `pocket-groove`, built by the host CMake when SDL2 is found; without SDL2 the rest of `host/` builds as before):

- `main.cpp`: boot as the firmware's `setup()` (instrument, UI, session or demo 0, audio last), the main loop, quit.
- `audio.cpp`: SDL's audio callback is the audio thread and the only caller of `Instrument::process` (16-frame quanta, any buffer size), as the device's audio task. With Listen on it adds the device's input (below); then it copies the output into a lock-free ring that the main thread drains for F9 recording; no file I/O, locks or allocation in the callback.
- `capture_convert.h`: Listen's capture side. SDL's capture device is opened allowing any change, and `CaptureSink` converts from the spec it really obtained: any SDL format (u8 to f32, either byte order) to int16 saturating, mono to both sides, more channels to the first two, callbacks of any length (also a frame split across two). Also the callback statistics and the `--listen-dump` tap (a lock-free byte ring; a thread writes the file).
- `listen_buffer.h`: Listen's rate match. The capture callback writes the device's frames into a lock-free ring; the output callback reads them with 4-point Hermite interpolation at a step (capture rate / output rate) x (1 + u), where u comes from a PI controller (critically damped, 0.1 rad/s narrowing to 0.02 rad/s once locked, |u| <= 2000 ppm) that holds a time-consistent fill (both sides' bursts smoothed out by their timestamps) at the target. The target is measured from how the callbacks really arrive, not from the sizes SDL reports (which sdl2-compat only echoes). It watches, primes to the target and fades in, counts underruns (then primes again), overflows, resyncs and clipped samples. No lock, allocation or system call on either side.
- `listen.cpp`: `CaptureSource` (SDL's capture device named "Pocket Groove"; `SimCapture` for the tests: a tone on the output clock x (1 + ppm), in any obtained spec and burst pattern), the dump writer, the diagnostics, and the main-thread Listen logic (find, open, unplug, hints).
- `strip.cpp`: the strip, drawn with `pg::ui::Gfx` (the instrument's font and palette, read-only) into a spare framebuffer; `display.cpp` scales it with the screen.
- `stems.cpp`: the Stems list: a worker thread runs `tools/pg --json` (one JSON object per line on stdout: `ls`, progress, done, error) and the list is drawn into its own framebuffer. `json_lite.h` reads the lines.
- `keymap.cpp`, `input.cpp`: the scancode table, overrides, and the held-key logic (repeat, Command, focus loss). `EventQueue` is where a second input source (MIDI) will post.
- `dirfs.h`: `pg::FileSystem` over the projects directory.
- `display.cpp`: integer nearest-neighbour scaling, HiDPI aware; uploads only the rows the UI changed.
- `script.cpp`: `--script`, which pushes SDL keyboard events so a script takes the same path as real keys.
- `samples.cpp`: the `samples/` folder (above): polls it, converts with `host/common/sample_convert.h` into one full-size buffer per slot, hands them to the instrument as a `pg::sampler::MemoryPool`, and takes dropped files (`SDL_DROPFILE`; script command `app drop PATH`, `app print-samples`).
- `lab.cpp`: the macro lab (`--lab`): polls the folder, parses with `pg::macro`, is the instrument library's overlay (`pg::macro::LibraryOverlay`), moves the track's instrument and macros through `Ui::setInstrument` / `Ui::setMacro`, draws its panel, writes the marks.

Tests: `test_desktop_samples` (the samples folder: load, a second file of a slot ignored, a broken file, reload on change, removal, drop and its copy), `test_desktop_keymap` (map and key logic), `test_desktop_files` (desktop files == device files, session round trip, ghost filter switch), `test_desktop_listen` (the capture conversion for every format, channel count and callback cut; the rate match against two simulated clocks, an hour at +500 and -500 ppm with callback jitter, variable blocks, 32 kHz, the Mac's burst patterns behind sdl2-compat, f32 mono 48 kHz, s32 32 kHz in odd sizes, 6-channel 96 kHz, a pattern that grows while playing, unplug and an output stall: no underrun, overflow or glitch, latency bounded, and a fit of the output: the device's frequency, its level, THD+N below -60 dB) and `test_desktop_app` (`host/app/tests/desktop_check.py`: the app headless under SDL's dummy drivers; its screenshots must be pixel-identical to `ui_shot` for the same keys, also with the strip's controls and the Stems list used in between; the modifier lights; the F9 recording bit-identical to the offline engine; the session saved at quit and resumed; a saved file through the device's transfer protocol; Listen against a simulated device, recording and unplug included, the recording fitted by `listen_analyze` (frequency, level, THD+N) for several obtained specs and burst patterns, the `--listen-dump` file checked against what was delivered, and SDL's real capture path driven by SDL's disk driver reading a sine from a file; the Stems list end to end against `xfer_sim` and its error messages; the macro lab: macros moved by mouse reach the engine, a file rewritten while running is reloaded, a broken one reported and ignored, the marks written, `none` and a deleted file give the values back).
