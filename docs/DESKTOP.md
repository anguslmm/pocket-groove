# Pocket Groove on the desktop

`pocket-groove` is the instrument in a window on your Mac (or Linux PC): the same engine, sequencer, UI and file format as the Cardputer firmware, compiled from the same `lib/pg` code. Program sounds and patterns here, then take the project to the device with `tools/pg push`; or review every instrument and setting without flashing firmware.

It is not a separate desktop UI. The window shows the device's 240x135 screen, the Mac keyboard stands in for the Cardputer keyboard key for key, and what you hear is what the device's engine renders (in stereo, or in the device's mono with `--mono`).

## Install (macOS)

```bash
brew install sdl2 cmake
```

You also need the Xcode command-line tools (`xcode-select --install`) for the compiler. Nothing else: no PlatformIO, no Python.

Recent Homebrew versions ship `sdl2` as *sdl2-compat* (the SDL2 interface running on SDL3). That is fine; the app prints the SDL version it runs on at start. If something misbehaves and you want a plain SDL2 instead, build it from source with the app: `cmake -S host -B build/host -DPG_FETCH_SDL2=ON` (downloads SDL 2.32 from GitHub, takes a few minutes once).

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
  F1 = G0 (side button)     F2 = audio stats in the title bar     F9 = record on / off

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
| G0 (side button) | F1 |

On a MacBook or Apple keyboard the top row is brightness, volume and media keys: hold the keyboard's `fn` (globe) key with F1 / F2 / F9, or switch on *Use F1, F2, etc. keys as standard function keys* in System Settings > Keyboard.

Command (⌘) does nothing in Pocket Groove: macOS uses it (Cmd+Q quits, Cmd+Tab switches apps, Cmd+H hides, Cmd+M minimises). Pressing Command releases every key the app holds, so Cmd+Tab in the middle of a chord leaves nothing stuck. When the window loses focus, every held key is released too (no stuck notes, no stuck modifiers). Holding a key does not auto-repeat in the OS sense: the instrument does its own repeat on `-` / `=` as on the device.

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
```

Cardputer keys by legend as in the manual (`` ` `` `1` ... `del` `tab` `q` ... `fn` `sft` `a` ... `ent` `ctl` `opt` `alt` `z` ... `spc` `G0`, or `shift`, `space`, `enter`, `backspace`, ...), plus `record`, `stats` and `none`. Scancode names are SDL's (`A`, `1`, `Left Shift`, `Right Alt`, `Backspace`, `F1`, `Keypad +`, ...; SDL calls Option "Alt" and Command "GUI"). `tools/pg-desktop --print-keymap` prints the map in force.

## Options

| Option | |
|---|---|
| `--dir PATH` | projects directory (default `./projects`) |
| `--sd PATH` | also mount `PATH` as the SD card (default: no card, as a Cardputer without one) |
| `--scale N` | window size: N x 240 by N x 135 points (default 4). The window can be resized; the picture stays sharp at whole multiples |
| `--buffer N` | audio buffer in frames (default 256 = 5.8 ms at 44.1 kHz). Lower for less latency, higher if you hear clicks |
| `--sr 32000` | run the engine at the device's fallback rate (default 44100) |
| `--mono` | the device's mono mode: what its headphone jack plays (one bus, pan ignored, mono delay and reverb) |
| `--device-keys` | model the Cardputer's key matrix: phantom keys appear and the phantom filter removes them, as on the device (see below) |
| `--keymap FILE` | key map overrides (above); `--print-keymap` prints the map |
| `--rec-dir PATH` | where F9 recordings go (default `./bounces/live`) |
| `--record` | start recording at launch |
| `--script FILE`, `--out DIR` | play a key script (as `host/tools/ui_shot` scripts, see `host/app/script.h`) and write its `shot`s to DIR |

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

F9 starts recording what you hear to `bounces/live/YYYYMMDD-HHMMSS.wav` (16-bit stereo at the engine's rate; in `--mono` both channels are the same); F9 again stops. The title bar shows `[REC m:ss]` while it runs. Quitting stops and closes the file. The recording is the engine's output sample for sample, live moves included, before the Mac's volume. (`tools/pg bounce` renders a saved song to stems instead; it does not capture live moves.)

## Differences from the device

- **Stereo** by default (the device has one DAC channel: `--mono` to hear exactly that).
- **No phantom-key filter.** The Cardputer's keyboard has no diodes, so some three-key combinations make it report a fourth key, and the UI filters those (`docs/ghosting-audit.md`). A Mac keyboard has no such matrix, so the filter is off and every combination works. `--device-keys` turns on a model of the Cardputer matrix and the filter: use it to check that a gesture you rely on works on the real device.
- **No boot menu, diagnostics strip, codec volume or backlight.** Use the Mac's volume. The status-bar battery icon is empty (unknown) and the low-battery save never fires.
- **Timing.** Keys reach the instrument the moment they arrive and the screen updates at the device's ~30 fps, as on the device. The audio latency is the buffer (`--buffer`) plus macOS's output path; the device's is about 15 ms.
- **Sample rate conversion.** The engine runs at 44.1 kHz (or 32 kHz); if the Mac's output device is set to 48 kHz, SDL converts.
- **CPU.** The `cpu` figure the UI gets is the desktop's load (F2 shows it in the title bar), not the device's; for the device's budget see `bench_instrument` and the device's diagnostics strip.

## Troubleshooting

- *No keys reach the window* after starting from Terminal: click into the window once (macOS sometimes leaves keyboard focus with Terminal for a binary that is not an app bundle).
- *Clicks or dropouts*: `--buffer 512`. F2 shows the audio callback's load and counts late callbacks, the sample rate, the buffer the output device actually gave, and the pixel factor of the picture (8x on a Retina screen at the default size).
- *F9 changes the volume or skips a track*: see the note on function keys above (fn + F9).
- *Control+Space switches the input source*, or *Control+arrow / Control+number switches Spaces*: macOS keyboard shortcuts take those before any app; turn them off in System Settings > Keyboard > Keyboard Shortcuts if you need the combination.
- *`SDL2 not found`*: `brew install sdl2`, then run `tools/pg-desktop` again (it re-runs CMake).

## How it is built

`host/app/` (target `pocket-groove`, built by the host CMake when SDL2 is found; without SDL2 the rest of `host/` builds as before):

- `main.cpp`: boot as the firmware's `setup()` (instrument, UI, session or demo 0, audio last), the main loop, quit.
- `audio.cpp`: SDL's audio callback is the audio thread and the only caller of `Instrument::process` (16-frame quanta, any buffer size), as the device's audio task. It also copies the output into a lock-free ring that the main thread drains for F9 recording; no file I/O, locks or allocation in the callback.
- `keymap.cpp`, `input.cpp`: the scancode table, overrides, and the held-key logic (repeat, Command, focus loss). `EventQueue` is where a second input source (MIDI) will post.
- `dirfs.h`: `pg::FileSystem` over the projects directory.
- `display.cpp`: integer nearest-neighbour scaling, HiDPI aware; uploads only the rows the UI changed.
- `script.cpp`: `--script`, which pushes SDL keyboard events so a script takes the same path as real keys.

Tests: `test_desktop_keymap` (map and key logic), `test_desktop_files` (desktop files == device files, session round trip, ghost filter switch) and `test_desktop_app` (`host/app/tests/desktop_check.py`: the app headless under SDL's dummy drivers; its screenshots must be pixel-identical to `ui_shot` for the same keys, the F9 recording bit-identical to the offline engine, the session saved at quit and resumed, a saved file through the device's transfer protocol).
