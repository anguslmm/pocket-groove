# The macro lab

A workbench for designing the instrument's macros: a few designed controls per voice that move several real parameters at once, through curves that may be anything (non-linear, peaking, stepped) as long as they feel right. Nobody can guess the right macros up front, so the lab makes trying one take seconds:

**edit a `.pgm` file (or `git pull`) → hear it in the desktop app, already playing → mark what feels good or bad → paste `lab-notes.txt` back.**

It is a development tool, not a product feature: off unless the desktop app is started with `--lab`, a plain panel beside the device screen. Since stage A of the redesign (2026-11-13) **a macro set is an instrument**: every track slot holds one, its four macros are the SOUND page (docs/UX.md 10.3), and the sets in `macros/` are compiled into the firmware as the built-in instruments (see [Instruments](#instruments) below). The lab is where they are tuned: a file in the lab folder **replaces the built-in instrument of the same voice and name** while the app runs, and the lab's sliders move the track's own macros, the same ones the SOUND page shows. Tuning goes on continually, in parallel with development, and never blocks it.

## Start it

```bash
tools/pg-desktop --lab                 # sets from ./macros (any depth), marks to ./lab-notes.txt
tools/pg-desktop --lab ~/my-macros     # another folder
tools/pg-desktop --lab --lab-notes ~/Desktop/notes.txt
```

Run it from the repository root. With `--lab` the window is wider (the panel is 184 device pixels to the right of the screen and strip) and opens at scale 3 instead of 4 unless `--scale` is given. `--lab` also turns on **developer mode** (`--dev` alone does that without the lab): the SOUND view can then show the bare parameter pages as well, `l` flipping between them and the instrument page (a CLASSIC track shows the parameter pages first). Set authors need them; the device never shows them. Everything else is the normal desktop app ([DESKTOP.md](DESKTOP.md)).

## The panel

```
  MACRO LAB                  BASS     <- the selected track (SHIFT+pad selects silently)
  [<]         ACID  1/5         [>]   <- the track's instrument; < > step: none (CLASSIC), then the list
  FAMILY: INTERACTION.                <- its header line (the file's first comment), else its file
  RELOADED ACID.PGM                   <- what just happened, an error (red), a warning (amber),
                                         the keyboard macro's description, or the keys
  OPEN  [SHUT=|--------OPEN]  0.40    <- one slider per macro, its end labels inside; drag it.
  BITE  [|-------------------]  0.00     Amber = the keyboard's macro. A tick under a slider:
  WOBL  [|-------------------]  0.00     an enum parameter jumps there
  +---------+   CUT   57  RES   98    <- what the set drives, as the instrument plays it now (the
  |    .    |   ENV   68  DCY   44       mixer rule applied): amber = differs from what the set
  |         |   ...                      computes (a hand edit in dev mode, a mixer row the player
                                         moved); a CYAN macro name = notes carry own values
  +---------+   [ GOOD ]   [ BAD ]
  [X OPEN][Y BITE]  lab-notes.txt     <- the XY pad drives two macros; click X / Y to choose them
```

The **XY pad** stands in for tilting the device: click or drag in it and the X macro follows left-right, the Y macro bottom-top. With one macro, X and Y are the same and X wins. The axes' end labels are written along the pad's bottom edge (X) and up its left edge (Y).

## Keys and mouse

| | |
|---|---|
| drag a slider | that macro (a click jumps there) |
| click / drag the XY pad | the X and Y macros together |
| click `X ...` / `Y ...` | the next macro on that axis |
| `<` `>`, or **Cmd+Left / Cmd+Right** | previous / next instrument of the selected track (`none` = CLASSIC: the track keeps the values it has and plays them as stored) |
| **Cmd+1 .. Cmd+4** | which macro the keyboard moves (its name turns amber) |
| **Cmd+Up / Cmd+Down** | that macro +-5 % (with Shift: +-1 %); hold to repeat |
| **Cmd+G** / GOOD | mark "good" |
| **Cmd+D** / BAD | mark "bad" |
| after a mark | type a short note; **Return** saves the line with it, **Esc** without |

None of these reach the instrument (Command never does; while a note is typed the keyboard is the lab's). Select the track the normal way: a pad key, or SHIFT+pad to select without playing. They can be rebound with `--keymap` like the other app commands (`lab-good = Cmd+K`, names `lab-good lab-bad lab-prev lab-next lab-up lab-down lab-1 .. lab-4`).

## What the lab does to the instrument

There is one source of truth: the track's instrument and its macro positions, stored in the project (`TrackSettings::instrument`, `macro[4]`).

- **Choosing a set** in the panel chooses the track's instrument (`Ui::setInstrument`, the same path the SOUND page's `;` `.` use). Its macros start at their default positions, or where they were when the set was last left in this session. `none` is CLASSIC: the track keeps the sound parameters it has and plays them as stored.
- **The sliders and the XY pad** move the track's own macro positions (`Ui::setMacro`, 0..255), the ones the SOUND page shows; moving a macro on the device keys moves the slider and the other way round. The engine derives the sound parameters (see [Instruments](#instruments)); the lab sends no parameter values. A drag glides like a held key: each new position is a `SetMacro` with the default 72 ms ramp, and the voice hears fractional values while it moves ([What a moving parameter sounds like](#what-a-moving-parameter-sounds-like)).
- **A file whose voice and name match a built-in instrument replaces it** while the app runs: projects keep naming it by its id, so a project that uses ACID plays `macros/bass/acid.pgm` as it is on disk now. A file with no built-in counterpart is an **extra instrument** for this session: it is listed after the built-ins (in the panel and on the SOUND page) and can be chosen, saved in a project and reloaded; without the lab such a project plays the values it was saved with (an unknown id, see below). A file without a `name` line is named after the file (`bass/my_test.pgm` = `MY_TEST`). Two files with the same voice and name: the first in path order wins, the other is ignored (the terminal says so).
- **Motion**: a note can carry its own value of each macro, and the macro glides from note to note while the pattern plays ("Motion" below; docs/UX.md 10.3). A macro that has own values on some note of the current pattern has its name drawn in cyan, and the marks line puts a `~` after its position. While such a pattern plays, the lab's sliders and pad move the macro's base (stored, heard once the motion ends), except with record on: then the hand is live and recorded into the notes, exactly as with the keys.
- **Developer mode** (`--lab` or `--dev`) can edit the bare sound parameters of an instrument track on the parameter pages; such a value lasts until the instrument derives again (a macro moves, the file is reloaded). The readout shows it in amber.

## Hot reload

The folder is polled four times a second (plain `stat`, any depth, files ending `.pgm`; hidden files are ignored). A changed, new or deleted file takes effect at once, while the app plays; the panel says `RELOADED ACID.PGM` and the terminal prints what happened. On a reload the tracks playing that set keep their macro positions by macro name. The engine is handed the new version safely while it plays: the lab publishes the replacement in the overlay the engine reads (atomics), posts `RefreshInstruments`, and frees the old version only once the engine has counted that command (`Engine::instrumentRefreshes()`), so the audio thread never reads freed memory. A deleted file that replaced a built-in gives the built-in back; a deleted extra leaves its tracks playing the values they have.

A file that does not parse **never half-loads**: the last good version stays active, and the panel shows the error in red with file, line and reason (`acid.pgm:7: unknown parameter 'cutof' for bass`) until the file is fixed; the terminal prints it too. A file that never parsed is not offered. A **warning** (see the format) never stops a load: the terminal prints every warning of a file each time it loads, and while that set is active the status line shows its first one in amber (`23: BODY: plain set undoes line 17 (LO-HI); add 'override' if meant`, with the count in brackets when there are more).

## The marks file

Each mark appends one line to `lab-notes.txt` in the directory the app was started from (git-ignored; `--lab-notes` for another file). Paste the file, or the lines, into the conversation.

```
2026-11-13 14:02:11 GOOD bass ACID macros/bass/acid.pgm@ee20b2a7 OPEN=0.62 BITE=0.10 WOBL=0.00 | CUT=78 RES=110 ENV=64~ DRV=27 ... | squelchy, great
2026-11-13 14:03:40 BAD  bass ACID macros/bass/acid.pgm@ee20b2a7 OPEN=0.95 BITE=0.80 WOBL=0.00 | CUT=104 RES=107* ... | harsh up here
```

time, GOOD/BAD, track, set name, file `@` the version (FNV-1a of the file's text, so a mark names the exact curves it was made with; `builtin@00000000` for a built-in instrument with no file in the lab folder), every macro position (`~` after it: notes of the current pattern carry own values of that macro), `|`, every owned parameter as the instrument plays it (short name `=` raw value, enum values by name; mixer rows with the mixer rule applied), `|`, the note if one was typed. A `*` after a value: it differs from what the set computes (a hand edit in developer mode, or a mixer row the player moved). With no set on the track (CLASSIC) the line says `none -` and lists all its sound parameters. The format is unchanged since round 1.

## The .pgm format

One file per set, plain text. Keep them under `macros/<track>/` (the lab does not care where; the `voice` line decides the track). A set is an instrument: a character (the base, and named patches to morph toward) plus up to four controls. This is model v2 (2026-11-13); every v1 file loads unchanged and evaluates to the same values, bit for bit (`itest_macro_pin` pins the 24 round-1 files as they were, kept in `host/tests/macro_v1/`).

```
# ACID -- family: INTERACTION.     <- the first line, a comment: the set's header (the lab shows it)
voice bass                         # first statement: kick snare clap chh ohh perc bass lead
name ACID                          # optional, up to 12 characters (else the file name)
about "The 303 squelch. Open it, then bite."   # optional: the player's line (up to 38 characters)
base wave=0 sub=40 lfoshape=sine   # optional, any number of lines: fixed values under the macros

macro OPEN 0.4 "SHUT" "OPEN" "the 303 filter"   # up to 4 macros; name up to 6 characters; start
  cutoff   0:18 0.5:62 1:108 smooth           # a target: parameter, then points position:value
  reso     0:70 0.6:112 1:96                  # any shape: this one peaks at 60 % and backs off
  envmod   add 0:10 1:-20                     # add: an offset on top of what came before

macro BITE
  fold     0.5:0 1:70                         # flat before the first and after the last point
  reso     add 0:0 1:12 by OPEN               # by: the effect scales with OPEN's position
  sub      mul 0:1 1:0.6                      # mul: a factor on what came before
  trim     0:0 1:-3                           # trim: dB on the level, applied last
macro WOBL
  rate     0:1/4 0.4:1/8 0.75:1/8t 1:1/16     # enum parameters take names and step by default
  cutoff   add 0:0 1:-15 linear
```

A morph set (lead KEYS is the real one):

```
base  wave=orgn mix=0 sus=110 cutoff=100 shape=92 ...     # the organ
patch PLUCK mix=127 sus=0 cutoff=58 shape=36 ...          # a named patch: parameter=value pairs
patch BELL  wave=bell@0.3 mix=0 sus=12 cutoff=116 ...     # @0.3: this enum switches at amount 0.3
macro PLUCK "ORGAN" "PLUCK"
  morph PLUCK                       # 0 = the base, 1 = the patch, every parameter it lists
macro BELL "ORGAN" "BELL"
  morph BELL 0:0 0.3:0 1:1 switch 0.6     # optional: an amount curve, and the line's enum switch point
  cutoff   add 0:0 1:-10                  # targets still adjust after the morphs
```

**The model.** A set owns its base parameters, every parameter a target names, every parameter a used patch lists, and the level if it has trims. For each owned parameter:

1. **Start** from the base value (or, without one, the value the parameter had when the set was chosen).
2. **Morphs**, all morph lines together, wherever they are in the file. Each line has an amount `a` (the macro's position, or its curve at that position, 0..1) and a patch. If the amounts add up to more than 1 they are scaled down so they add up to 1. A continuous parameter becomes the start plus, for every line whose patch lists it, its scaled amount times (patch value - start). So the result is a mix of the base and the patches, the base weighted by whatever is left, and never beyond them. An enum parameter cannot mix: it takes the value of the patch whose line has the largest amount among the lines past their switch point (`a >= switch`; the value's own `@`, else the line's `switch`, else 0.5; a later line wins a tie), else it stays at the start value.
3. **Targets**, in file order: `set` replaces the value with the curve's value `c`, `add` adds `c`, `mul` multiplies by `c`, where `c` is the curve at that macro's position. With `by M`, the target's effect is scaled by macro M's position `s`: set moves `s` of the way to `c`, add adds `c*s`, mul multiplies by `1+(c-1)*s`.
4. **Trims**: the level is multiplied by 10^(dB/60), where dB is the sum of every trim line's curve at its macro's position (times its `by` macro's position). The mixer's level taper is cubic (100 = unity, gain = (level/100)^3), so that is dB on the sound. One raw level step is about 0.26 dB at 100, and the level stops at 127 (+2.1 dB over 100), so a trim cannot lift a quiet corner much: lower the loud ones instead.
5. **Clamp** to the parameter's range and round to an integer. Parameters the set does not own are left alone.

**The XY pad with two morphs** (the combination rule): with PLUCK on X and BELL on Y, the bottom left is the base, the right edge's end the pluck, the top edge's end the bell, and the top right an even mix of pluck and bell (the amounts 1 + 1 are scaled to 0.5 + 0.5). Below the diagonal from top left to bottom right each morph simply adds its share of the difference from the base; beyond it the base has no weight left and the pad moves between the patches.

**Lines.** One statement per line; indentation and spacing do not matter; words are case-insensitive; `#` starts a comment (except inside a string).

| line | |
|---|---|
| `# TEXT` | the first non-blank line, if it is a comment, is the set's **header** line (`MacroSet::about`, up to 72 characters, the rest is cut): the author's, shown by the lab and listed in the index. When it starts with the set's name and ` -- ` (the round-1 header `# ACID -- family: ...`), that part is dropped: the header line is then `family: ...` |
| `about "TEXT"` | optional, once: the **player's line** (`MacroSet::tagline`), one short sentence of 1 to 38 characters that fits the instrument page under the name (shown in capitals). Say what the instrument is and what to do with it, in a player's words ("The 303 squelch. Open it, then bite."), not the family or the idea being tested. Longer is an error, not cut |
| `voice TRACK` | required, first statement. `kick snare clap chh ohh perc bass lead` (also `snar`, `closedhat`, `openhat`) |
| `name NAME` | optional, one word, up to 12 characters, shown in capitals |
| `base p=v p=v ...` | optional, repeatable; no spaces around `=` |
| `patch NAME p=v p=v@SW ...` | a named patch, up to 4; NAME up to 8 characters (letters, digits, `-`, `_`); repeat the line to continue it (also after other patches). `@SW` (only on enum parameters): this value's switch point, above 0 and at most 1, 2 decimals |
| `macro NAME [START] ["LOW" "HIGH"] ["DESCRIPTION"]` | up to 4; NAME up to 6 characters (letters, digits, `-`, `_`); START 0..1. Strings in double quotes: two are the **end labels** (up to 8 characters each, e.g. `"DARK" "BRIGHT"`), one alone or a third is the **description** (up to 40) |
| `PARAM [set\|add\|mul] POS:VAL ... [linear\|smooth\|step] [by MACRO] [override]` | a target of the macro above; up to 32 per macro, 64 in the set; 1 to 16 points. `override`: this plain set is meant to replace what earlier lines did (silences the warning below) |
| `morph PATCH [POS:AMOUNT ...] [linear\|smooth\|step] [switch SW]` | a morph line of the macro above: blends from the base toward PATCH. Without points the amount is the macro's position; with them, a curve (amounts 0..1). `switch SW` is the enum switch point for this line (default 0.5). Up to 8 morph lines in the set; a macro morphs toward a patch at most once |
| `trim POS:DB ... [linear\|smooth\|step] [by MACRO]` | a level trim of the macro above, dB -24..+24, applied after everything else whatever its place in the file. Up to 8 in the set |

**Parameters** by short or long name from the list below, ignoring case, spaces and punctuation (`cut`, `cutoff`, `envmod` = `Env Mod`, `lfo_rate`), or by a unique start of the long name of at least 3 letters (`reso` = Resonance, `snappy` = Snappy). A start that fits two parameters is an error that says so.

**Values** are the raw stored numbers (the ranges below), not what the screen shows: `tune=24` is 0 semitones, `pan=64` is centre. Enum parameters also take their value names (`rate=1/8t`, `wave=supr`); a name wins over a number (`fmratio=2` is the ratio 2, raw 3). `add` and `mul` take plain numbers (`add` -255..255, `mul` 0..16). Numbers: optional sign, at most 3 decimals. Base and patch values are rounded to whole raw values.

**Curves.** `POS` 0..1, positions in order; two points at the same position make a jump. Between points: `linear` (default), `smooth` (an S-curve, flat at each point) or `step` (holds the left point's value; the default for enum parameters). Before the first point and after the last the curve is flat; one point is a constant.

**Enum jumps.** An enum parameter (wave, filter type, LFO rate, percussion model ...) can only jump. In a target the jump is where its step curve's points are; in a morph it is the switch point (0.5 unless `@` or `switch` says otherwise). The lab draws a tick under a slider wherever moving it makes an enum parameter jump (the other macros where they are), and `macro_audit` prints the positions per macro.

**`by MACRO`** names any macro of the set, also one declared further down; `morph PATCH` names any patch, also one declared further down.

**Warnings** do not stop the load; the lab shows the active set's in its status line (amber) and the terminal prints them all, and `macro_audit` lists them: a plain `set` (no `by`, no `override`) on a parameter that an earlier target, or a morph of another macro, already moves (that earlier work is thrown away: round 1 lost CRACK's body offset this way), and a patch no morph line uses.

**Errors** are reported with the line and stop the load (the last good version stays): an unknown or ambiguous parameter, a value out of range or not a value name, positions going backwards, a target, morph or trim before any `macro`, a missing `voice` line, a duplicate `voice` / `name` / macro / base value / patch value, a morph toward an unknown patch or twice toward one patch in a macro, a limit exceeded (the message says which), a name or string too long, a string without its closing quote, a string where none belongs, a line over 200 characters, a character that is not plain ASCII (outside comments).

### Limits, memory, cost

Limits live in `lib/pg/src/pg/macro/macro.h`:

| | per set | |
|---|---|---|
| macros | 4 | |
| targets | 64 (pool), at most 32 in one macro | 4 bytes each |
| curve points | 192 (pool), at most 16 on one curve | 4 bytes each |
| patches | 4 | 11 bytes each |
| patch values | 96 (pool) | 3 bytes each |
| morph lines / trim lines | 8 / 8 | 6 / 5 bytes each |
| strings | name 12, header 72, about 38, per macro: name 6, labels 8 + 8, description 40 | |

One loaded set is **1896 bytes** (`sizeof(pg::macro::MacroSet)`, including its 16-bit instrument id; v1 was 2420 with 4 x 8 targets of 8 float points). The 24 built-in sets live in flash (about 45 KB); the device holds no copy of them in RAM. Targets and points live in pools shared by the set's macros, so one long journey macro can use what the others do not. A curve point packs its position in thousandths (10 bits) and its value in thousandths, signed (22 bits): every number the format can write (3 decimals: `mul` 0.125, `add` -1.5, value 126.999) is stored exactly, and decoding gives the very float v1's parser produced. Switch points are hundredths. The parser needs about 1.5 KB of stack and no heap.

Evaluation of one set: per target a search for the segment (one position decode per point passed: an integer compare and one float division), two value decodes (integer divide and one float division each), one division and a few multiply-adds for the curve; per patch value of an active morph two multiply-adds; per trim a curve, and one 2^x from a 6-term polynomial for all trims (no `powf`, no libm); then a clamp and round per owned parameter. On this host it takes about 130 to 340 ns per set (KEYS and TRIO, all morph, are the cheapest, POLES with 22 targets and 64 points the dearest; `itest_macro_files` prints each), about 0.8 us for a set near the limits (60 targets, 188 points, two 22-value morphs, 4 trims; `test_macro_eval`). For POLES that is roughly 150 float divisions (point decoding and the curve segments), 200 other float operations and the integer work around them. On the ESP32-S3 (240 MHz, single-precision FPU) expect very roughly ten to thirty times the host time, around 3 to 10 us per set: eight sets at 300 Hz would be about 1 to 3 % of one core (an estimate from the host figures, not measured on the device).

## Instruments

Stage A of the redesign (2026-11-13). The model and the format above are unchanged; this is how a set becomes what a track plays.

**A track slot holds one instrument.** `TrackSettings` (lib/pg/src/pg/project.h) carries the instrument's 16-bit **id** and four **macro positions** (0..255, position = byte / 255) in what were reserved bytes, so the `Project` did not grow; `kProjectVersion` is 2. Id 0 is **CLASSIC**: no instrument, the track's stored sound parameters play as they are. A version-1 project loads with no instrument on any track and sounds exactly as before (`itest_golden` renders a frozen CLASSIC copy of every demo directly and through a version-1 file, bit for bit). A new project (CTRL+N) starts with the first instrument of every track at its default positions; the built-in demos use instruments on every track since stage G (docs/STATE.md).

**Ids.** `macro::setId(voice, name)`: FNV-1a of `voice:NAME` (`bass:ACID`), folded to 16 bits. A project names an instrument by its id, so adding or reordering sets never breaks a saved project. Changing a set's content changes how existing projects sound: intended, the sets are retuned continually. The generator and `itest_instrument` fail on two sets with one id and on id 0 (rename the set).

**Derivation.** With an instrument, the track's sound parameters 0..23 are derived: `evaluate(set, positions, under = the voice's default values)`, written into the project's parameters whenever the id or a position changes. So an instrument sounds the same whatever the track held before, the stored values are always what plays, and a project whose instrument the firmware does not know (an **unknown id**: a lab extra, a set removed later) simply plays the values it was saved with; the id is kept, the SOUND page says UNKNOWN and offers the list. The voices receive these 7-bit parameters, exactly as before stage B, whenever no macro moves; while one moves they hear fractions (see "What a moving parameter sounds like" below), and the stored values stay the 7-bit evaluation at the stored positions.

- *Where it runs*: on the audio thread, at a quantum edge, only on a change. Once per quantum the engine compares each track's id and positions with what it derived from (48 bytes, inside `quantum()`); `SetInstrument`, `SetMacro` and a Project edited by any route are followed that way. Where a motion holds the macros away from the stored positions, or they rest there after Stop, the 7-bit path is the evaluation where they stand, once per change. `Engine::sanitizeProject` also derives (so a loaded project already holds what will play and its CRC stays clean). The 7-bit derivation stays in flash, and so does the fractional evaluation of a moving macro (86 times a second per moving track; its per-tick part is in IRAM: "What a moving parameter sounds like").
- *Cost* of the 7-bit derivation: one evaluation is 130-340 ns on the host for the round-1 sets; on the ESP32-S3, running from flash, expect very roughly 3-10 us, once per `SetMacro` (a held key: one per 40 ms per macro) and once each time a motion comes to rest away from the stored positions. The cost of a macro in motion is in "What a moving parameter sounds like".

**The mixer rule.** Level, pan, the two sends and duck (parameters 24..28) are the player's (MIX view) and an instrument never overwrites them. A set may still drive the level (`lvl`, `trim`) and the sends (`dly`, `rev`); what plays is

- *level* = player × instrument ÷ default: the instrument's level is a gain trim relative to the parameter's default (through the cubic taper its dB add to the player's);
- *send* = player + (instrument − default): the instrument adds to (or takes from) the player's send;
- *pan* and *duck*: the player's only (an instrument's targets on them are ignored);

each rounded and clamped to 0..127 (`macro::combineMixer`). With the player's values at their defaults this is exactly what the set evaluated to, so the round-1 sets sound as they did in the lab and `macro_audit` (which now renders through the rule, the player at the defaults) passes all 24 unchanged.

**No locks on an instrument track; notes carry macro values.** Since project version 3 a step on an instrument track carries, instead of a parameter lock, its own value of each of the four macros (or none: it follows), and the macros glide from note to note ("Motion" below). There are no parameter locks on an instrument track: a version-2 file's macro lock becomes that macro's value on that note (rounded to the 63 levels), a lock of a sound parameter (already ignored) and a **mixer lock on an instrument track are dropped** when it loads. A CLASSIC track keeps its one parameter lock per step, exactly as before; a version-2 macro lock on a CLASSIC track (it did nothing) is dropped. Choosing an instrument for a CLASSIC track, or going back to CLASSIC, clears the track's step values in every pattern (a lock and macro values do not mean the same thing). Pasting or copying a track onto another track or another instrument drops its macro values (they only mean something for the instrument they were made on).

**Built-in instruments.** The `.pgm` files under `macros/` are the source. `host/tools/macro_builtin` compiles them into `lib/pg/src/pg/macro/builtin_data.cpp`, a checked-in C++ source of const, constant-initialised `MacroSet` tables (24 sets, 1896 bytes each, about 45 KB of flash). The firmware build runs no host tool; the engine and the UI use the tables in place, with no parser and no RAM copy on the device. The order and the selection come from `macros/builtin.txt` (one line per voice; the **first** file of a line is what a new project starts with: kick BOOM, snare CRACK, clap HYPE, CHH TICK, OHH BLAST, perc TAP, bass ACID, lead HANDS). Every `.pgm` under `macros/` must be listed; a listed file must parse without warnings and have a `name`. After changing any listed file or the list, regenerate and commit:

```bash
cmake --build build/host --target macro_builtin && build/host/macro_builtin
```

The ctest `check_builtin` (`macro_builtin --check`) fails while the checked-in table is stale, and `itest_instrument` compares every table entry with what parsing its file gives now, byte for byte.

**The library.** `macro::findInstrument(id)`, `instrumentCount(track)`, `instrumentAt(track, i)`: the built-ins, with the host's overlay laid over them (the lab, above; the device has none).

## The sets in macros/

[`macros/INDEX.md`](../macros/INDEX.md) lists every set by voice with its family, its idea and what to try first, and groups the sets by family ("What round 1 is testing"), so marks can be read back as evidence about a kind of macro. Every file starts with a comment header: `# NAME -- family: FAMILY.`, then the idea being tested and what the player should feel if it works, and has an `about "..."` line after its name: what the player reads on the instrument page (`itest_macro_files` checks the first line, which is also the set's header line, that every set has a player's line of at most 38 characters that the built-in table carries, that set names are unique per track, and that no set loads with a warning).

## Auditing a set (macro_audit)

Nobody writing a set can listen to it from inside the conversation, so `build/host/macro_audit` renders it instead: for a grid over the macro space it plays a short fixed phrase on the set's voice through the real engine and measures each render.

```bash
build/host/macro_audit macros/bass/acid.pgm      # one set, full report
build/host/macro_audit macros/lead               # every set in a folder (any depth)
build/host/macro_audit --points macros/kick/boom.pgm   # every grid point, not only lines and corners
build/host/macro_audit --wav build/audit macros/snare/crack.pgm   # also a WAV per point, to listen to
build/host/macro_audit --hard macros             # hard checks only, one line per set (the ctest)
```

**The grid.** Each macro alone at 0, 1/8 .. 1 with the others at their start positions (`--hard`: 0, 1/4 .. 1), every corner of the space, and a 5 x 5 grid of the first two macros (the XY pad), the others at their start.

**The phrase** (120 BPM, one bar plus a second of tail). Bass: C2, G2 accented and slid into C3, a long soft G1, an accented G3. Lead: C4, an A3 min7 chord, a long C5, C3 accented and slid into G3. Drums: hits at velocity 127 (accented), 100, 60, 100 seven semitones up, 100. The track plays alone with the ducker off; what is measured is the track's stem plus its own share of the delay and reverb returns (so send macros count), as the mono sum the device plays (so WIDTH and PAN moves are inaudible and show as dead).

**Columns** (one row per grid point):

| column | |
|---|---|
| `peak` | highest sample, dBFS |
| `loud` | loudness: the loudest 100 ms (RMS, dBFS). Compares a short pluck with a held note by how loud they sound, not by their energy; the level checks use it |
| `rms` | RMS over the whole render, dBFS (energy: long notes and tails count) |
| `centr` | spectral centroid, Hz, power weighted (dominated by the fundamentals on a bass) |
| `hf` | share of the power above 1.5 kHz, dB (a brightness measure that is not fooled by the sub) |
| `decay` | ms from the loudest 10 ms of the decay window until 20 dB below it (synths: the long note; drums: the first hit); `>` = it never fell that far inside the window |
| `d` | spectrogram distance to the next point on the line, dB: the mean level difference over the time-frequency cells (2048-point FFT, 96 log bands 40 Hz..16 kHz) within 60 dB of the top, each weighted by the square root of its amplitude. 0 = the same sound; one CUT value at a time on a resonant bass is about 0.6; a stepped flavour change is 5 to 25 |

Each macro's line is headed with its end labels, and with the positions where an enum parameter jumps along it (`enum jumps: WAVE@0.125 FLT@0.440 ...`). Per set it then prints the loudness range across the whole space, the highest peak, the centroid range, the **travel** of each macro (the sum of the `d` values along its line: how much happens over its whole throw; under about 5 is a macro that barely does anything) and the problems:

- **HARD** (exit 1, fails the ctest): the file does not parse, a point is **silent** (peak below -45 dBFS), a point **clips** (stem peak above 0 dBFS), **NaN / Inf** in the output, engine faults.
- **warn** (reported, exit 0): the parser's **warnings** (a plain set that undoes an earlier line, an unused patch), **dead travel** (neighbours on a macro's line closer than `--dead` dB, default 0.5; merged into stretches, and counted separately for the rest of the pad), a **level jump** between neighbours (loudness differs by more than `--jump` dB, default 6), **hot** points (peak above -3 dBFS), a **loudness range** over 18 dB across the space, **subnormal** samples (with where: the track stem, the delay or the reverb return). Today the engine's delay and reverb produce subnormals on their default settings (their one-pole states decay into the subnormal range when the input goes quiet), so almost every set with a send warns; that is an engine matter, not the set's.

**Level suggestions.** For every macro whose line strays more than 2 dB in loudness from its loudness at its start position (the others at their start), the audit prints a ready-to-paste trim line that would hold it there: that macro's own trims plus the measured difference, at 0, 1/4 .. 1, rounded to 0.5 dB, for example

```
  suggest, macro GRIT (strays up to 4.5 dB from its start's -19.7 dB); paste under it:
  trim 0:0 0.25:-1 0.5:-2 0.75:-3.5 1:-4.5
```

Paste it in place of the macro's own trim lines (not its `lvl` targets, which it measured with). Flat is not always right: a "more" macro may be meant to get louder, and a flattened macro can turn out to do little else (VOYAGE's GRIT, flattened, is dead travel at the flute station: it is mostly a loudness control there). Boosts over +2 dB are flagged: the level stops at 127.

A set whose dead stretch or jump is the idea (stepped flavours, a macro that only works combined with another) says so in a comment line, `# audit: allow-dead NAME` or `# audit: allow-jump NAME` (`*` for every macro), and the warning is not raised for that macro.

The ctest `macro_audit` runs `--hard` over all of `macros/` (about 40 s); `macro_audit_catches_*` check that it really fails the broken sets in `host/tests/macro_bad/` (silent, clipping, a typo) for the right reason.

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

## What a moving parameter sounds like

Stage B of the redesign (2026-11-13): macro motion. Parameters are still stored and shown as 7-bit values (one bass CUT value moves the cutoff about 73 cents, the lead's about 83), but a macro that moves no longer walks the voices up a staircase of them. The contract is in `lib/pg/src/pg/engine.h` ("MACRO MOTION"), the per-voice choices in the voice headers.

**Trajectories.** Per track and macro the engine keeps a current position (a float in byte units) that moves to each `SetMacro` target in a straight line over the command's ramp time: by default **72 ms** (`kMacroRampMs`), longer than the longest gap between two commands from a held key on the device (the UI repeats every 40 ms but runs in 33 ms frames, so commands arrive 33 or 66 ms apart). A new target starts from wherever the macro is, so a held key, the lab's sliders and XY pad, and later tilt join into one continuous movement instead of stop-and-go. `setMacroCommand(track, macro, position, rampQuanta)` takes an explicit ramp. Motion (below) uses the same trajectories, aimed by the engine itself at the quantum the next note fires (ramps of any length: 128 steps at 60 BPM is 32 s). The Project byte, and the stored parameters derived from it, are the target at once; only what the voice hears follows the trajectory.

**Jumps, never ramps:** an instrument change (`SetInstrument`, a loaded project, the lab replacing a set), a ramp of 1 (`kMacroJump`), positions changed by any other route, and Play from the top (to the motion's start values; the voices start fresh anyway). **Enums and semitone parameters** (waves, filter types, LFO rates, the perc model, TUNE and OSC2 PITCH) take the whole value the set gives, so they switch where the set says, to within one evaluation interval (11.6 ms).

**Fractional parameters.** While any macro of a track moves, the engine evaluates the instrument without rounding (`macro::evaluateFine`: the same steps as `evaluate` without the last rounding, points decoded by multiplication, within an ulp) once per **11.6 ms** (`kFineEvalQuanta` = 32 quanta), where the macros will be at the end of that interval (the ramps are known, so this adds no lag; a new target changes the speed from the next interval). In between, the values run in a straight line, and the voice gets them every **1.45 ms** (`kFineQuanta` = 4 quanta; 2 ms at 32 kHz) through `Voice::setParamsFine`; the drums every 11.6 ms. The mixer gets a fractional level and sends through the same rule as the bytes (`macro::combineMixerFine`). Entering the fractional path the values run for 50 ms from the bytes the voice was playing; when the movement has ended they run for 50 ms (`kMacroSettleMs`) into the stored bytes, and the track is back on the 7-bit path, bit for bit what a static project plays (`itest_motion`). A track whose macros stand still, and every CLASSIC track, never leaves the 7-bit path: `itest_golden` is unchanged and `macro_audit` prints the same report for all 24 sets as before.

**Per voice** (what follows fractions; the rest takes whole values as before):

| voice | continuous while a macro moves | stepped / latched |
|---|---|---|
| bass | every parameter but the enums and TUNE: cutoff, resonance, env mod and decay, accent, drive, LFO depth, wave, sub, FM mix and depth, fold, timbre env, slide, amp decay, release, attack, key track, LFO>timbre. Its mapping has no libm, so it simply maps the floats; the cutoff CV (~1.2 ms) and the level-type params (~1.6 ms) keep their smoothing on top | LFO rate and shape, FM ratio, sub octave, TUNE |
| lead | cutoff, resonance, env amount, shape, detune, width, sustain, key track, drive, osc mix, LFO rate, vibrato, LFO>shape, LFO>cutoff (cheap mappings: polynomial exp2 / sin / cos); the 5 ms smoothing stays on cutoff, resonance, shape, width, drive and mix | waves, filter type, OSC2 PITCH, the envelope times and glide: rounded, through the 7-bit mapping as before (a 7-bit step of a time is not heard as a step) |
| kick | TUNE, PUNCH, DRIVE, HARM, TONE, also inside a ringing hit | DECAY, SWEEP, HOLD, CLICK, CLICK TONE: at the next hit |
| snare | TUNE, DETUNE, TONE, BEND, SNAPPY, RING, DRIVE | DECAY, BODY DECAY: at the next hit |
| clap | TONE, RESONANCE, AIR, TAIL, DRIVE | DECAY, SPREAD, BURSTS: at the next hit |
| hats | TUNE, SPREAD, TONE, COLOUR, METAL, SNAP | DECAY: at the next hit |
| perc | (none) | everything at the next hit (one change recomputes the whole model) |

A drum computes its continuous coefficients with polynomials instead of `powf` / `expf` / `tanf` / `sinf` and applies a latched parameter with the exact libm mapping once, at the start of the next hit. So a sweep over a roll recomputes no transcendental per control tick and never changes an envelope or a model inside a hit. For whole numbers every voice's fractional path gives the 7-bit path's result (the bass exactly, the others within 1e-6 of the libm values), and on the way back to bytes everything the fractions touched is re-mapped exactly.

**Measured** (`build/host/zipper_probe --engine`, ctest `zipper_motion`): a real set's macro swept through the engine's command path at the device's key-repeat cadence (+2 units per 40 ms repeat in 33 ms UI frames), rendered with every `SetMacro` a jump (the 7-bit path: how stage A sounded), with the default glide, and as one ideal continuous ramp. "path" is the voice's own value of the key parameter (the bass's cutoff after its lag, the lead's smoothed cutoff, the kick's body pitch): the largest change in one quantum during the sweep. "step" is the largest change of the bass's harmonics 1-12 across each 7-bit step instant (median), "flux" the 99th percentile of their change per 5.8 ms hop.

| case | | 7-bit steps | glide | ideal |
|---|---|---|---|---|
| bass ACID OPEN 0.40 -> 0.65, held A2 | path (cents per quantum) | 19.0 | 0.90 | 0.85 |
| | step (dB) | 5.1 | 3.0 | 3.0 |
| | flux (dB) | 2.9 | 1.7 | 1.8 |
| lead HANDS OPEN 0.30 -> 0.55 (PUSH, HOLD up), held A3 | path | 5.8 | 0.92 | 0.77 |
| kick BOOM SIZE 0.25 -> 0.75, 1/16 roll | path | 17.5 | 2.7 | 2.5 |

The glide is as smooth as the ideal everywhere; the ctest fails when it is not, or when the 7-bit render stops showing its staircase. On the lead the audio measures are printed but not checked: its two detuned saws beat, which moves the spectrum more than a 7-bit step does (step 2.6 / 2.4 / 2.4 dB). No case clicks (the kick's largest sample step between hits is the same in all three renders). To listen: `build/zipper/motion_bass_acid_open_steps.wav` against `..._glide.wav` and `..._ideal.wav`, the same for `lead_hands_open` and `kick_boom_size`. The original voice-level probe (`zipper_probe` without `--engine`) still renders the CUT 40 -> 64 staircase it measured on 2026-11-13 (`bass_steps_4s.wav` against the LFO-smooth `bass_smooth_4s.wav`).

**Cost.** With nothing moving the only addition is a compare per quantum and per voice; on the CLASSIC busy pattern callgrind counts 5 x86 instructions per frame more than stage A (2105 against 2100 stereo, 1756 against 1755 mono: about 0.1 % of a core at most). With two macros of each of two tracks sweeping all the time (`build/host/bench_motion`: the busy pattern, bass on ACID, lead on HANDS, OPEN + BITE and OPEN + PUSH moving as triangles, a new target every 33 ms UI frame): 2138 -> 2250 instructions per frame stereo (ratio 1.052), 1787 -> 1888 mono (1.056); as an ESP32-S3 estimate (cycles = instructions x 1.3, range 1.0..1.6, as `bench_instrument`) **2.7 % of one core stereo (2.1..3.3), 2.4 % mono (1.8..3.0)** for the two tracks together. Where it goes, per moving track: the evaluation, once per 11.6 ms (ACID, HANDS: about 4000 instructions each, ~22 us on the device), about 0.2 % of a core; the voice's mapping every 1.45 ms (bass: its whole mapping, ~1300 instructions with 14 exp2 calls, ~0.5 %; lead: only what changed, ~850, ~0.35 %); the interpolation of 27 values every 1.45 ms (~0.15 %); and the mixer's gain ramps while the level moves (~0.4 %). A moving drum costs the evaluation plus a few polynomials every 11.6 ms and its latched mappings once per hit, about 0.3 %. The per-tick part (the engine's `fineTick` / `fineEval`, the bass's `mapParams`, the lead's `setParamsFine` / `applyFine`) runs from IRAM (`tools/pio_audio_ram.py`); the evaluation (86 Hz per moving track) and the drums' mappings run from flash, so on the device they also pay instruction-cache refills the host cannot see. `env:profile-ram`'s MOTION lines measure all of it on the device.

## Motion

Stage C of the redesign (2026-11-13; decided with the owner 2026-11-12). The contract is `lib/pg/src/pg/engine.h` "MOTION"; the player's side is docs/UX.md 10.3 "Motion" and docs/MANUAL.md "Motion".

**Notes carry macro values.** On an instrument track a note (a step that is on) can carry its own value of each of the four macros, at 63 levels from 0 to 255 (a 6-bit code per macro, 0 = no own value: follow), packed into the step's existing 8 bytes (`lib/pg/src/pg/project.h` Step; the RAM wall in docs/STATE.md left no room to grow the pattern data). They are glide targets; the glide itself is continuous (the trajectories above).

**The rule.** Per track and macro the keyframes are the notes with a value of that macro, at their grid time with swing (polymeter: the track wraps on its own length), whether or not they fire (probability, mute and a recorded note's suppression do not move the motion). No keyframe in the playing pattern: the macro sits at its base (the instrument page's position). One: constant. Several: in the quantum a keyframe fires the macro is exactly at its value, then it moves in a straight line to the next keyframe's value, arriving in the quantum that one fires; through silence and notes without a value of their own, across the wrap. After the last keyframe of a pattern it glides toward the first keyframe of whatever plays next (the same pattern, a queued one, the next song slot), or to the base, arriving at the boundary, when that pattern has none. A pattern queued late, an edit, a tempo or swing change: the glide re-aims from where it is, so it still arrives on the note. A value cannot rise and fall within one note. Known limit (accepted): a note followed by a long gap can only glide slowly.

**Start and stop.** Play from the top starts each macro where the looping pattern has it at step 1 (a keyframe there, or the line from the pattern's last keyframe to its first at that time), so the first pass sounds as the later ones. At Stop the macros rest where the motion had them; the next move of a macro glides from there to its base (and Play in a pattern without motion takes it back to the base).

**The base while a motion plays.** A `SetMacro` (a key, the lab's slider or pad, later tilt) of a macro with motion in the playing pattern stores a new base but is not heard until the motion ends (the base is what a pattern without values plays, and where a glide to the base goes). With record on and the transport running the hand is **live** instead: from its first move until it has been still for one bar (or record goes off, or Stop) the macro moves as the hand moves it, and every note of the track that starts meanwhile takes its value. A recording pass is one undo unit with the notes recorded in it.

**Cost.** Nothing runs per quantum but two compares (is something to re-aim, is something to publish). At each step of a track whose pattern has motion a few compares; at a keyframe, a pattern boundary or a change, a scan of at most two patterns' steps of that track to find the next keyframe. A glide is a moving macro: the fractional path's cost above, for as long as it moves (between two notes with different values, all the time). A macro held by a single keyframe stands still on the 7-bit path at that value.

**Proof** (host): `itest_motion_notes` (exact values at the keyframes, straight lines with and without swing, silence, wrap, queued pattern and song slot with and without keyframes, late queue, tempo change, polymeter, first pass, base moves, clearing, stop, the published positions, live recording); ctest `zipper_motion`'s keyframe case: two bass notes a bar apart, ACID OPEN 102 -> 166: the voice's cutoff path moves at most 4.75 cents per quantum, the same as one ideal ramp over that bar, and stays within 0.9 cents of it (WAVs `build/zipper/motion_bass_acid_keyframes_{motion,ideal}.wav`).
