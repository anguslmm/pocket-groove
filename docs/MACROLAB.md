# The macro lab

A workbench for designing the instrument's macros: a few designed controls per voice that move several real parameters at once, through curves that may be anything (non-linear, peaking, stepped) as long as they feel right. Nobody can guess the right macros up front, so the lab makes trying one take seconds:

**edit a `.pgm` file (or `git pull`) → hear it in the desktop app, already playing → mark what feels good or bad → paste `lab-notes.txt` back.**

It is a development tool, not a product feature: off unless the desktop app is started with `--lab`, a plain panel beside the device screen, nothing stored in projects. The macro model and the file parser are portable core code (`lib/pg/src/pg/macro/`, no heap, float only), so the device can load the same files later.

## Start it

```bash
tools/pg-desktop --lab                 # sets from ./macros (any depth), marks to ./lab-notes.txt
tools/pg-desktop --lab ~/my-macros     # another folder
tools/pg-desktop --lab --lab-notes ~/Desktop/notes.txt
```

Run it from the repository root. With `--lab` the window is wider (the panel is 184 device pixels to the right of the screen and strip) and opens at scale 3 instead of 4 unless `--scale` is given. Everything else is the normal desktop app ([DESKTOP.md](DESKTOP.md)).

## The panel

```
  MACRO LAB                  BASS     <- the selected track (SHIFT+pad selects silently)
  [<]         ACID  1/2         [>]   <- the set on this track; < > step: none, then each set
  macros/bass/acid.pgm                <- its file
  RELOADED ACID.PGM                   <- what just happened, or an error (red), or the keys
  OPEN  [=====|--------------]  0.40  <- one slider per macro; drag it. Amber = the keyboard's macro
  BITE  [|-------------------]  0.00
  WOBL  [|-------------------]  0.00
  +---------+   CUT   57  RES   98    <- what the set drives, as the instrument has it now:
  |    .    |   ENV   68  DCY   44       amber value = edited by hand since (differs from the set)
  |         |   ...                      cyan name = p-locked on steps of this pattern
  +---------+   [ GOOD ]   [ BAD ]
  [X OPEN][Y BITE]  lab-notes.txt     <- the XY pad drives two macros; click X / Y to choose them
```

The **XY pad** stands in for tilting the device: click or drag in it and the X macro follows left-right, the Y macro bottom-top. With one macro, X and Y are the same and X wins.

## Keys and mouse

| | |
|---|---|
| drag a slider | that macro (a click jumps there) |
| click / drag the XY pad | the X and Y macros together |
| click `X ...` / `Y ...` | the next macro on that axis |
| `<` `>`, or **Cmd+Left / Cmd+Right** | previous / next set for the selected track (`none` = the bare parameters as before) |
| **Cmd+1 .. Cmd+4** | which macro the keyboard moves (its name turns amber) |
| **Cmd+Up / Cmd+Down** | that macro +-5 % (with Shift: +-1 %); hold to repeat |
| **Cmd+G** / GOOD | mark "good" |
| **Cmd+D** / BAD | mark "bad" |
| after a mark | type a short note; **Return** saves the line with it, **Esc** without |

None of these reach the instrument (Command never does; while a note is typed the keyboard is the lab's). Select the track the normal way: a pad key, or SHIFT+pad to select without playing. They can be rebound with `--keymap` like the other app commands (`lab-good = Cmd+K`, names `lab-good lab-bad lab-prev lab-next lab-up lab-down lab-1 .. lab-4`).

## What a set does to the instrument

- While a set is active on a track, the parameters it **owns** (its base patch and every parameter a macro names) follow the macros. The lab evaluates once per UI frame and sends only values that changed, through the UI's own value path (`Ui::setTrackParam`, the same command the value keys post), so the SOUND and MIX pages show the values moving.
- **Editing an owned parameter by hand**: your value stays until the set computes a different value for that parameter (you move a macro that drives it, or its file is reloaded). Then the macro wins. The readout shows a hand-edited value in amber.
- **P-locks win**, as always: a step that locks an owned parameter plays its lock. The readout shows those parameter names in cyan (the factory demos lock some bass parameters, e.g. ACIDJAM's ENV on pattern 1). Use a pattern without locks to hear the macro alone.
- **Choosing `none` or another set** puts back the values the track had when the set was chosen. Positions and the chosen set are remembered per track and file for the session (not across restarts, not in projects).
- The resulting values are ordinary parameter values: they are in the project like any edit (the title's `*`), and in the session saved at quit. The macro state itself is never stored in a project.
- Loading another project while a set is active: the set keeps driving its parameters on its next move; "none" then puts back the values from the old project. Choose `none` before loading if that matters.

## Hot reload

The folder is polled four times a second (plain `stat`, any depth, files ending `.pgm`; hidden files are ignored). A changed, new or deleted file takes effect at once, while the app plays; the panel says `RELOADED ACID.PGM` and the terminal prints what happened. On a reload the macro positions are kept by macro name and every owned value is sent once. A deleted active set puts the track back to the values from before it.

A file that does not parse **never half-loads**: the last good version stays active, and the panel shows the error in red with file, line and reason (`acid.pgm:7: unknown parameter 'cutof' for bass`) until the file is fixed; the terminal prints it too. A file that never parsed is not offered.

## The marks file

Each mark appends one line to `lab-notes.txt` in the directory the app was started from (git-ignored; `--lab-notes` for another file). Paste the file, or the lines, into the conversation.

```
2026-11-13 14:02:11 GOOD bass ACID macros/bass/acid.pgm@ee20b2a7 OPEN=0.62 BITE=0.10 WOBL=0.00 | CUT=78 RES=110 ENV=64~ DRV=27 ... | squelchy, great
2026-11-13 14:03:40 BAD  bass ACID macros/bass/acid.pgm@ee20b2a7 OPEN=0.95 BITE=0.80 WOBL=0.00 | CUT=104 RES=107* ... | harsh up here
```

time, GOOD/BAD, track, set name, file `@` the version (FNV-1a of the file's text, so a mark names the exact curves it was made with), every macro position, `|`, every owned parameter as the instrument has it (short name `=` raw value, enum values by name), `|`, the note if one was typed. A `*` after a value: edited by hand, the set computes something else. A `~`: p-locked on steps of the current pattern (those steps play their lock). With no set on the track the line says `none -` and lists all its sound parameters.

## The .pgm format

One file per set, plain text. Keep them under `macros/<track>/` (the lab does not care where; the `voice` line decides the track).

```
# ACID: a 303 line. Everything after # is a comment.
voice bass                         # first line: kick snare clap chh ohh perc bass lead
name ACID                          # optional, up to 12 characters (else the file name)
base wave=0 sub=40 lfoshape=sine   # optional, any number of lines: fixed values under the macros

macro OPEN 0.4                     # up to 4 macros; name up to 6 characters; start position (default 0)
  cutoff   0:18 0.5:62 1:108 smooth        # a target: parameter, then points position:value
  reso     0:70 0.6:112 1:96               # any shape: this one peaks at 60 % and backs off
  envmod   add 0:10 1:-20                  # add: an offset on top of what came before

macro BITE
  fold     0.5:0 1:70                      # flat before the first and after the last point
  reso     add 0:0 1:12 by OPEN            # by: the effect scales with OPEN's position
  sub      mul 0:1 1:0.6                   # mul: a factor on what came before
macro WOBL
  rate     0:1/4 0.4:1/8 0.75:1/8t 1:1/16  # enum parameters take names and step by default
  cutoff   add 0:0 1:-15 linear
```

**The model.** A set owns its base parameters and every parameter a target names. For each owned parameter, start from the base value (or, without one, the value the parameter had when the set was chosen), then apply every target on that parameter in file order: `set` replaces the value with the curve's value `c`, `add` adds `c`, `mul` multiplies by `c`, where `c` is the curve at that macro's position. With `by M`, the target's effect is scaled by macro M's position `s`: set moves `s` of the way to `c`, add adds `c*s`, mul multiplies by `1+(c-1)*s`. At the end clamp to the parameter's range and round to an integer. Parameters the set does not own are left alone.

**Lines.** One statement per line; indentation and spacing do not matter; words are case-insensitive; `#` starts a comment.

| line | |
|---|---|
| `voice TRACK` | required, first statement. `kick snare clap chh ohh perc bass lead` (also `snar`, `closedhat`, `openhat`) |
| `name NAME` | optional, one word, up to 12 characters, shown in capitals |
| `base p=v p=v ...` | optional, repeatable; no spaces around `=` |
| `macro NAME [START]` | up to 4; NAME up to 6 characters (letters, digits, `-`, `_`); START 0..1 |
| `PARAM [set\|add\|mul] POS:VAL ... [linear\|smooth\|step] [by MACRO]` | a target of the macro above; up to 8 per macro; 1 to 8 points |

**Parameters** by short or long name from the list below, ignoring case, spaces and punctuation (`cut`, `cutoff`, `envmod` = `Env Mod`, `lfo_rate`), or by a unique start of the long name of at least 3 letters (`reso` = Resonance, `snappy` = Snappy). A start that fits two parameters is an error that says so.

**Values** are the raw stored numbers (the ranges below), not what the screen shows: `tune=24` is 0 semitones, `pan=64` is centre. Enum parameters also take their value names (`rate=1/8t`, `wave=supr`); a name wins over a number (`fmratio=2` is the ratio 2, raw 3). `add` and `mul` take plain numbers (`add` -255..255, `mul` 0..16). Numbers: optional sign, at most 3 decimals.

**Curves.** `POS` 0..1, positions in order; two points at the same position make a jump. Between points: `linear` (default), `smooth` (an S-curve, flat at each point) or `step` (holds the left point's value; the default for enum parameters). Before the first point and after the last the curve is flat; one point is a constant.

**`by MACRO`** names any macro of the set, also one declared further down.

**Errors** are reported with the line and stop the load (the last good version stays): an unknown or ambiguous parameter, a value out of range or not a value name, positions going backwards, a target before any `macro`, a missing `voice` line, a duplicate `voice` / `name` / macro / base value, too many macros (4), targets (8 per macro), points (8), a name too long, a line over 200 characters, a character that is not plain ASCII.

**Limits** live in `lib/pg/src/pg/macro/macro.h` (`kMaxMacros`, `kMaxTargets`, `kMaxPoints`, name lengths). One set is a 2.4 KB table (`sizeof(pg::macro::MacroSet)`); evaluation is a few hundred float operations.

## Every parameter a file can name

Generated from the instrument's parameter tables by `build/host/param_reference --macrolab docs/MACROLAB.md` (ctest `check_macrolab` fails when it is stale). Short name, long name, raw range (what the screen shows instead, if different) or the enum's value names, `[default]`. What each one does: MANUAL.md chapter "Parameter reference" and the comments in each voice's header (`lib/pg/src/pg/bass/bass.h`, `lead/lead.h`, `drums/drums.h`).

<!-- BEGIN MACRO PARAMS -->
```
voice kick
  tune  Tune          0..127  [60]
  dcy   Decay         0..127  [46]
  pnch  Punch         0..127  [76]
  swp   Sweep Time    0..127  [58]
  clk   Click         0..127  [72]
  drv   Drive         0..127  [28]
  tone  Tone          0..127  [100]
  hold  Hold          0..127  [30]
  harm  Harmonics     0..127  [36]
  cton  Click Tone    0..127  [56]
voice snare
  tune  Tune          0..127  [60]
  dcy   Decay         0..127  [56]
  snap  Snappy        0..127  [80]
  tone  Tone          0..127  [70]
  body  Body Decay    0..127  [52]
  bend  Pitch Bend    0..127  [60]
  ring  Ring          0..127  [64]
  dtun  Detune        0..127  [50]
  drv   Drive         0..127  [26]
voice clap
  tone  Tone          0..127  [57]
  dcy   Decay         0..127  [56]
  sprd  Spread        0..127  [64]
  tail  Tail          0..127  [70]
  brst  Bursts        0..4  [3]
  res   Resonance     0..127  [50]
  air   Air           0..127  [48]
  drv   Drive         0..127  [30]
voice chh
  tune  Tune          0..127  [64]
  dcy   Decay         0..127  [58]
  tone  Tone          0..127  [80]
  mtl   Metal         0..127  [84]
  colr  Colour        0..127  [56]
  snap  Snap          0..127  [40]
  sprd  Spread        0..127  [64]
voice ohh
  tune  Tune          0..127  [64]
  dcy   Decay         0..127  [52]
  tone  Tone          0..127  [72]
  mtl   Metal         0..127  [84]
  colr  Colour        0..127  [56]
  snap  Snap          0..127  [32]
  sprd  Spread        0..127  [64]
voice perc
  modl  Model         tom rim cowb clav shkr zap  [rim]
  tune  Tune          0..127  [64]
  dcy   Decay         0..127  [64]
  tone  Tone          0..127  [64]
  bend  Bend          0..127  [64]
  harm  Harmonic      0..127  [64]
  nois  Noise         0..127  [64]
  drv   Drive         0..127  [32]
voice bass
  cut   Cutoff        0..127  [46]
  res   Resonance     0..127  [78]
  env   Env Mod       0..127  [70]
  dcy   Decay         0..127  [44]
  acc   Accent        0..127  [90]
  drv   Drive         0..127  [30]
  lfo   LFO Depth     0..127  [0]
  rate  LFO Rate      2bar 1bar 1/2 1/4. 1/4 1/8. 1/4t 1/8 1/8t 1/16 1/16t 1/32  [1/8]
  wave  Waveform      0..127  [0]
  sub   Sub Level     0..127  [56]
  fmix  FM Osc Mix    0..127  [0]
  fm    FM Depth      0..127  [64]
  rati  FM Ratio      0.5 1 1.5 2 3 3.5 4 5 7  [2]
  fold  Fold          0..127  [0]
  tenv  Timbre Env    0..127  [90]
  lshp  LFO Shape     tri sine down up sqr s&h  [tri]
  slid  Slide Time    0..127  [65]
  adcy  Amp Decay     0..127  [96]
  rel   Release       0..127  [32]
  atk   Attack        0..127  [18]
  ktrk  Key Track     0..127  [40]
  ltmb  LFO>Timbre    0..127  [0]
  subo  Sub Octave    uni -1oct  [uni]
  tune  Osc Tune      0..48 (screen -24..+24)  [24]
voice lead
  cut   Cutoff        0..127  [72]
  res   Resonance     0..127  [30]
  env   Env Amount    0..127  [48]
  fdcy  Env Decay     0..127  [60]
  wave  Osc1 Wave     saw puls tri supr fm1 fm2 bell orgn  [saw]
  shp   Shape         0..127  [0]
  det   Detune        0..127  [56]
  dcy   Decay         0..127  [70]
  atk   Attack        0..127  [0]
  wid   Width         0..127 (screen 0%..100%)  [96]
  sus   Sustain       0..127  [80]
  rel   Release       0..127  [44]
  fatk  Env Attack    0..127  [0]
  flt   Filter Type   lp bp hp  [lp]
  ktrk  Key Track     0..127 (screen 0%..100%)  [64]
  drv   Drive         0..127  [0]
  osc2  Osc2 Wave     saw puls tri sine  [saw]
  intv  Osc2 Pitch    0..48 (screen -24..+24)  [24]
  mix   Osc Mix       0..127 (screen -64..+63)  [64]
  lfo   LFO Rate      0..127  [94]
  vib   Vibrato       0..127  [0]
  lshp  LFO>Shape     0..127  [0]
  lcut  LFO>Cutoff    0..127  [0]
  glid  Glide         0..127  [64]
mixer rows, on every voice (defaults differ per track: the bass's)
  lvl   Level         0..127  [98]
  pan   Pan           0..127 (screen -64..+63)  [64]
  dly   Delay Send    0..127 (screen 0%..100%)  [0]
  rev   Reverb Send   0..127 (screen 0%..100%)  [0]
  duck  Duck Amount   0..127 (screen 0%..100%)  [110]
```
<!-- END MACRO PARAMS -->

## What a moving parameter sounds like (facts for the redesign)

Measured on 2026-11-13 (`build/host/zipper_probe`, which also writes WAVs to `build/zipper/` to listen to):

- **Resolution.** Parameters are 7-bit. One CUT value moves the bass cutoff about 73 cents (40 Hz..8.6 kHz over 127 values), the lead's about 83 cents. A macro mapped to a narrow range has few values: CUT 40..64 is 24 steps.
- **Smoothing inside the voices.** The engine hands every voice its parameters once per 16-sample quantum (0.36 ms) and drains commands at quantum edges. Bass: the cutoff CV (knob + envelope + LFO + accent) runs through a ~1.2 ms one-pole lag, and the level-type parameters (resonance, drive, wave / pulse mix, FM mix and index, fold, sub) through ~1.6 ms one-poles; everything else (envelope times, LFO rate and depth, slide, key track, tune) applies on the next quantum unsmoothed. Lead: ~5 ms one-poles on cutoff, resonance, shape, width, drive and the oscillator mix; the rest unsmoothed; a note starting from silence snaps to the current values. Drums: no smoothing at all; a change recomputes coefficients (with `powf` / `expf`) on the next quantum, also in the middle of a ringing hit. Mixer rows (level, pan, sends): ~4 ms smoothing in the engine.
- **Zipper noise: no clicks, but a staircase.** Bass, saw A2 at RES 110, CUT 40 -> 64 one value at a time over 4 s (a step every 167 ms) and over 1 s, against the same sweep done smoothly by the bass's own LFO: the click energy in the 3 ms after each step equals the smooth sweep's (median 0 dB, worst +1.7 dB), so the smoothing de-clicks every step. But the motion is stepped: the stepped render differs from the smooth one by -15 dB (4 s) / -18 dB (1 s) of the signal, and across each step the harmonics near the resonance change by about 11 dB at once (median; worst 19 dB), where the smooth sweep spreads a change of about half that over the same time. On a slow sweep across a narrow range this is very likely heard as discrete steps in the resonance (I could not listen; compare `bass_steps_4s.wav` with `bass_smooth_4s.wav`). Lead, same sweep: no click either (median +0.7 dB against the signal between steps).
- **What this means for the redesign** (not done here): macros want either a finer internal parameter (e.g. 16-bit or float targets for the cutoff-type parameters, with the 7-bit value as the stored/displayed one) or a longer slew (tens of ms) on the parameters a macro sweeps; and the drums need cheap per-quantum parameter updates before anything moves them continuously.
