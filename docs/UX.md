# Pocket Groove: UX specification (v1)

Status: complete for v1 implementation. Mockups are in `docs/ux/` (contact sheet: `docs/ux/overview.png`) and are drawn by `tools/ux_mock/render.py` at true 240x135 with the palette, font and coordinates given here. Where this text and the mockup code disagree on a coordinate, **the mockup code wins** (`tools/ux_mock/screens.py` names every region). Anything the design needs that the contract headers don't provide is listed in section 17, "Contract change requests". The spec doesn't assume those changes; each one has a fallback.

---

## 0. The decisions in one page

1. **Modes.** There is one home view, **GROOVE**: an 8-track overview above 16 step keys. There are five more views, each on its own key that never changes: **SOUND** `9`, **MIX** `0`, **FX** `o`, **SONG** `p` and **PROJECT** `\`. `` ` `` always goes home. Everything else is a temporary layer: a step held (STEP EDIT), a modifier held (FN mute, OPT patterns, ALT roll, TAB tempo, CTRL edit), a pop-up or a dialog. These never trap you, because releasing the key, or pressing `` ` ``, ends them. The keys that never change meaning anywhere (except while typing a file name) are SPACE (play/stop), `` ` `` (home), the five view keys, `[ ]` (DJ filter, with SHIFT for bass cutoff), TAB (tempo), `/` (help), the pads `a`-`k` (play and select a track), FN+pad (mute), OPT+step (queue a pattern) and the CTRL combos.
2. **Step grid.** Steps 1-8 are on `1`-`8` and steps 9-16 on `q`-`i`, as two rows of 8 that sit exactly under the on-screen 2x8 pads. The row `a`-`k` holds the 8 track pads (KICK SNAR CLAP CHH OHH PERC BASS LEAD). The row `z`-`,` holds the 8 note keys. `'` pages through 16-step pages (up to 4). SHIFT+step sets the pattern length. CTRL+SHIFT+step sets the length of the selected track only (polymeter).
3. **Instruments (stage A, 2026-11-13).** Every track slot holds an **instrument**: a macro set (docs/MACROLAB.md) with up to four macros. **SOUND is the instrument page**: the instrument's name and its macros as four large controls, each moved by its own key pair (`z`/`x`, `c`/`v`, `b`/`n`, `m`/`,`), so two macros move at once with two fingers; `;` `.` (and `9` again) step through the track's instruments. The bare sound parameters are not shown to the player any more (only in a developer mode on the desktop). A track with no instrument is CLASSIC (demos, older projects): it plays its stored sound, and its page invites a choice (10.3).
4. **Value editing.** There's exactly one "focused value", shown in the status-bar chip. `-` and `=` change it from every view. A tap moves it by 1. Holding sweeps it with acceleration. SHIFT moves it 8x. Pressing both keys resets it to the default. You choose what is focused in SOUND, MIX or FX, or by selecting a track, and it stays on `-`/`=` after you go back to GROOVE. Two performance sweeps are always live and ignore the focus: `[`/`]` move the **master DJ filter** (both keys together = off) and **SHIFT+`[`/`]` move the bass's macro 1** (on a CLASSIC bass: its cutoff). Neither needs any selection first.
5. **Step detail and p-locks.** Hold a step key. The pad row becomes 8 step fields (VEL, GATE/PITCH, PROB, RATCH, ACC, SLIDE, CHORD, LOCK) and `-`/`=` edit the chosen field. The note row sets the step's note, and on drum tracks it sets velocity instead. Tapping another step while one is held copies the held step there. To p-lock a step, hold it while you're in SOUND or MIX and press `-`/`=` (in SOUND: or a macro key): that locks the focused macro (or MIX parameter) on that step. Every gesture uses at most two non-modifier keys.
6. **Note entry.** v1 ships three methods. The first is a **scale-locked note row** (`z`-`,` = 8 consecutive degrees of the project scale from the track's octave; `;`/`.` change octave). The second is **303-style step-write**: REC while stopped, then each note key writes and advances, SHIFT adds an accent, holding the previous note gives a slide, DEL inserts a rest. The third is **live record**: REC while playing. There is no piano layout in v1, because a scale-locked row can't hit a wrong note. Lead chords come from **AUTO** mode by default: each note key plays the diatonic triad on that degree of the scale. `l` cycles AUTO, AUTO7 and the fixed chord types.
7. **Performance.** FN+pad mutes and FN+note-key solos. OPT+step queues a pattern for the next boundary, and OPT+SHIFT+step copies the current pattern there first and then queues it (that's how variations get built). ALT+pad rolls a track while held. `[`/`]` sweep the DJ filter. Holding DEL while playing erases the selected track as the playhead passes. All of these need at most one modifier plus one key.
8. **Feedback.** There's a status bar on every screen showing transport, rec, pattern/queue/song slot, BPM with a beat blink, the focus chip, a mono level meter, a ducker gain-reduction bar and battery. GROOVE shows all 8 tracks' 16 steps plus a trigger lamp per track. The selected track's 16 steps are drawn as large pads with note names, velocity bars and accent, slide, ratchet, probability, lock and chord markers. Value changes pop up a name, value and bar for 1.2 s. Holding `/` shows the current view's key map, and tapping `/` turns on "explain" mode.
9. **Destructive actions.** CTRL+X/C/V cut, copy and paste the selected track, and CTRL+SHIFT+X/C do the same for the whole pattern. Every destructive action stores a one-level undo snapshot of the affected pattern (4.2 KB), and so does a change of instrument (the track's instrument, positions and sound). CTRL+Z undoes, and pressing it again redoes. Files are `/int/pg/NAME.pgv` and `/sd/pg/NAME.pgv`, with 8-character names. CTRL+S saves. Load asks before discarding unsaved work.
10. **Power-on.** The device resumes the last session, or loads the factory demo ACIDJAM (A minor, 124 BPM, ducker on, an instrument on every track) the very first time. A banner says "SPACE PLAY". One key press gives a pumping acid-house groove whose bass moves by itself.

---

## 1. Principles

1. **Sound in one key press, beat in ten seconds.** Nothing has to be configured before it plays. Every default sounds good, and the demo is loaded.
2. **One home, no dead ends.** `` ` `` always goes home. View keys go straight to their view from anywhere. Overlays exist only while their key is held. Dialogs always accept `` ` `` to cancel.
3. **Keys never lie.** A physical key does the same thing in every performance view. When a view reuses a key, it's for the local "column / row / page" selection, the screen shows it (the column labels print the key letter), and the help overlay shows it too.
4. **Two keys at most.** No gesture needs more than two non-modifier keys held at once. FN, SHIFT, CTRL, OPT and ALT may be added on top. The matrix has no diodes, so three held keys can produce a phantom fourth; the UI filters phantoms (16.1) and no gesture relies on a key combination that is electrically ambiguous (docs/ghosting-audit.md).
5. **The selected track is the cursor.** The pads choose the track. Steps, notes, SOUND, MIX lane and the step strip all follow it.
6. **What you touched last is on `-`/`=`.** The focus chip in the status bar always tells you what those keys will change.
7. **Looks like an instrument.** Track colours carry meaning, there are big pads, there's very little text, and all text is upper case at 7 px or larger.
8. **Mono out.** Pan and stereo width are never prominent. Pan is the last MIX row, drawn dimmed, and it says so.

---

## 2. Core state (what the UI remembers)

All of this is UI-thread state except where it says Project. Nothing here is saved, except that `octave`, `chordMode` and `rollRate` (plus the open project's name, mount and dirty flag) go into a small UI settings file (section 13.2).

| State | Values | Default | Notes |
|---|---|---|---|
| `view` | GROOVE, SOUND, MIX, FX, SONG, PROJECT | GROOVE | |
| `selTrack` | 0..7 | 6 (BASS) at power-on with the demo; 0 on a new project | Changed by a pad press. |
| `focus` | `{track t, param p}`, `{track t, macro m}` or `{global g}` | `{6, CUT}` with the demo; a new project: the track's macro 1 | The thing `-`/`=` edits. See 5.1. A macro is index 32+m. |
| `trackFocus[8]` | param or macro index per track | 0 for every track, except PERC = 1 (TUNE) | Remembered per track. A track with an instrument focuses a macro (its last one, else macro 1). |
| `soundPage[8]` | 0..2 | 0 | Developer mode only (the parameter pages). |
| instrument, macros | per track, in the Project (`TrackSettings`) | a new project: each track's first instrument at its default positions | Session memory: the positions each instrument had when the track left it (16 entries). |
| `mixRow` | LEVEL, DUCK, DELAY, REVERB, PAN | LEVEL | |
| `fxTab` | DUCK, SPACE, MASTER | DUCK | `fxFocus[3]` per tab: DDEP, DFB, FILT. |
| `shownPage` | 0..3 | 0 | The 16-step page shown in GROOVE/SOUND/MIX. It doesn't follow the playhead (see 4.2). |
| `octave[8]` | bass 0..4, lead 1..6 | bass 1, lead 3 | Drums: none (their note row is velocity). |
| `chordMode` | NONE, AUTO, AUTO7, OCT, 5TH, MAJ, MIN, MAJ7, MIN7, DOM7, SUS4, MIN9 | AUTO | Lead only. |
| `rollRate` | 1/8, 1/16, 1/32, 1/16T | 1/16 | |
| `heldStep` | none or 0..63 | none | |
| `stepCursor` | 0..63 | 0 | Step-write. |
| `solo` | none or track, plus saved mute mask | none | See 8.2. |
| `undo` | one Pattern snapshot plus its index, plus `kind` | empty | 4168 B static. |
| `clip` | empty, Track(type), Pattern | empty | TrackPattern 520 B / Pattern 4168 B static. |
| `dirty` | bool | false | Set when `status.revision` changes after a load or save. |

**Edit target.** Edits always go to the **pattern that is playing**, `status.pattern`. When stopped, that's the pattern last queued or played. There is no "edit a pattern that isn't playing" mode. To edit another pattern, queue it (OPT+step). While stopped, queuing switches immediately.

---

## 3. Mode and state diagram

```
                    `  (always: back to GROOVE, closes overlays / cancels dialogs)
      +---------------------------------------------------------------------+
      |                                                                     |
      v           9            0            o            p           \      |
  +--------+  +-------+   +-------+    +-------+    +-------+   +---------+ |
  | GROOVE |->| SOUND |   |  MIX  |    |  FX   |    | SONG  |   | PROJECT | |
  +--------+  +-------+   +-------+    +-------+    +-------+   +---------+ |
   any view key goes directly to its view from any other view  ------------+
   pressing a view key while in that view = next page / row / tab

  Layers inside GROOVE / SOUND / MIX / FX (exist only while a key is held):
    step key held >= 250 ms ...... STEP EDIT panel (GROOVE) / LOCK mode (SOUND, MIX)
    FN held ...................... MUTE/SOLO strip       OPT held ... PATTERN pads
    ALT held ..................... ROLL strip            TAB held ... TEMPO box
    / held ....................... KEY MAP overlay       CTRL ....... no overlay (combos only)

  Sticky sub-modes:
    REC (enter): stopped -> STEP-WRITE (cursor)   |   playing -> LIVE REC
                 SPACE moves STEP-WRITE -> LIVE REC;  stop moves LIVE REC -> STEP-WRITE
                 enter again or ` -> REC off
    / tapped -> EXPLAIN: the next keys only describe themselves; / or ` exits

  Modal (only these swallow the global keys):
    CONFIRM dialog  (enter = yes, ` = no, SPACE still plays/stops)
    NAME ENTRY      (all character keys type, including space; enter = OK, ` = cancel)
```

---

## 4. Layout of the keyboard

```
 row0  `     1  2  3  4  5  6  7  8    9     0     -   =   del
       HOME  [ steps 1-8 ............]  SOUND MIX  VAL-  VAL+ ERASE
 row1  tab   q  w  e  r  t  y  u  i    o     p     [   ]   \
       TEMPO [ steps 9-16 ...........]  FX    SONG  DJ<  DJ> PROJ
 row2  fn    shift a  s  d  f  g  h  j  k    l     ;   '   enter
       MUTE  SHIFT [ pads KICK..LEAD ...]   CHORD OCT+ PAGE REC
 row3  ctrl  opt  alt  z  x  c  v  b  n  m  ,   .   /    space
       EDIT  PATT ROLL [ notes 1-8 .......]  OCT- HELP PLAY
```

### 4.1 Step keys

* Step key `1`..`8`, `q`..`i` = step `shownPage*16 + n` of the selected track.
* **Tap** (released within 250 ms with no other key pressed meanwhile):
  * If the step is off, it turns on at **key-down**, for responsiveness, using `kDefaultStep`. Synths get note = degree 1 of the scale at the track's octave. Lead gets chord = the chord that `chordMode` gives for that degree.
  * If the step is on, it turns off at **key-up**, but only on a clean tap.
* **Hold** (250 ms or more, or any other key pressed while it's held) opens STEP EDIT (section 7). A step that was off and gets edited stays on.
* Steps at or beyond the track length are drawn as empty outlines. Tapping one does nothing and shows the toast "BEYOND LENGTH - SHIFT+STEP SETS LENGTH".

### 4.2 Pages and length

* `'` sets `shownPage` to the next page within `ceil(len(selTrack)/16)`, wrapping round. The footer page indicator shows pages in use (filled), the shown page (white) and the page currently playing (a green dot).
* Pages **don't follow the playhead**. Edits must never land on a page that just flipped under your thumb. The green dot shows where play is.
* SHIFT+`'` = **add page**: if the pattern is shorter than 64, every track's length grows by 16 (capped at 64), the new steps are copied from that track's previous 16, and the view jumps to the new page. It also sets `SetPatternLength`. It stores an undo snapshot.
* SHIFT+step *n* sets the **pattern length** to `shownPage*16 + n`. That's `SetPatternLength` plus `SetTrackLength` for all 8 tracks. Steps beyond the new length are kept but hidden. It stores an undo snapshot. Toast: "LENGTH 24".
* CTRL+SHIFT+step *n* sets **the selected track's length only** (polymeter). Pattern length is unchanged. Toast: "BASS LENGTH 12".

### 4.3 Pads (`a`..`k`)

* **Press = select that track and play it**: `NoteOn` at vel 100. Synths play degree 1 of the scale at the track's octave, and the lead plays the `chordMode` chord. Release sends `NoteOff`. If REC is on and the transport is playing, the hit is recorded by the engine.
* SHIFT+pad = select **without sound**, which is the safe way to switch tracks mid-performance.
* In the FX view on the DUCK tab, a pad also moves the focus to that track's DUCK amount (section 10.6).
* FN+pad = mute toggle. ALT+pad = roll (section 8).

### 4.4 Note row (`z` `x` `c` `v` `b` `n` `m` `,`)

* **In SOUND** the row is the instrument's **macro keys** (10.3): `z`/`x` macro 1 down/up, `c`/`v` macro 2, `b`/`n` macro 3, `m`/`,` macro 4. Everywhere else it plays notes as below.
* **Synth track selected:** key *k* (0..7) plays scale degree *k* above the root at `octave[sel]`. With `kGScaleRoot`=A, MIN and octave 1, the keys play A1 B1 C2 D2 E2 F2 G2 A2. Pentatonic scales run on into the next octave, and CHRO gives 8 semitones. MIDI note = `12*(octave+1) + root + degreeOffset`, clamped to 0..127.
* **Lead:** the note becomes a chord according to `chordMode` (section 9.3).
* **Drum track selected:** key *k* plays that drum at **velocity** 16, 32, 48, 64, 80, 96, 112 or 127 (natural pitch). Drum pitch is a per-step field (section 7), not a live control.
* **Legato = slide.** If a note key is still held when the next one is pressed on BASS or LEAD, the new `NoteOn` carries `kStepSlide` (glide, no retrigger). That's how you play and record acid slides without a modifier. In live record the slide is written on the PREVIOUS step (with a gate reaching the new note), not on the new one: a step's SLD means "this note runs into the next".
* SHIFT+note = the same with `kStepAccent`.
* `;` / `.` = octave up/down for the selected synth track. A toast shows "OCT 2". It does nothing on drums.
* `l` = next chord mode (lead), and SHIFT+`l` = previous. On other tracks `l` does nothing.

---

## 5. Value editing: the make-or-break interaction

### 5.1 What is focused

`focus` changes when:
* you move a macro in SOUND (its key pair, 10.3), which focuses that macro and stores it in `trackFocus[sel]` (developer mode's parameter pages: a column key focuses that parameter);
* you choose a row in MIX (`z`..`b`), which focuses param 24..28 of the selected track;
* you press a column key in FX, which focuses a global;
* you select a track with a pad in any view except FX, which sets focus to `{t, trackFocus[t]}` (on a track with an instrument a macro: its remembered one, else macro 1); in MIX it's `{t, row}`, and in FX/DUCK it's `{t, kMixDuck}`;
* the track's instrument changes: a focus on its sound follows to the new instrument's macro.

It **doesn't** change when you change view. So you can set `focus = DDEP` in FX, go home and pump the ducker with `-`/`=` while watching the grid. The status-bar chip always shows the owner tag (track colour, or cyan "FX"), the short name and the value.

### 5.2 The `-` / `=` keys (timings)

| Gesture | Effect |
|---|---|
| Tap `-` or `=` | ±1 raw unit, applied on key-down |
| Hold | After **300 ms**, repeats every **40 ms** (25 Hz). Step per repeat: 1 for the first 0.5 s of repeating, then 2 for 0.5 s, then 4. A full 0..127 sweep takes about 2.2 s, and fine moves stay easy. |
| SHIFT + tap/hold | ±8 per event, or ±1 for params whose range is ≤ 16. Same repeat timing, no acceleration. |
| `-` and `=` together (2nd key down within the hold) | Reset to `ParamInfo::def`. Toast "DEFAULT". The held repeat stops. |
| Enum / ranges ≤ 16 values | One step per tap. Hold repeats every 200 ms, no acceleration, and doesn't wrap. |
| TAB held + `-`/`=` | BPM ±1 (SHIFT ±10), same timing. |

**Macros** (a focused macro, or its own keys in SOUND) are positions 0..255, shown as 0..100 %: a tap moves 2 (0.8 %), a held repeat 2, then 4, then 8 per 40 ms (a full sweep takes about 2.3 s), SHIFT 16 per event without acceleration; both keys of a pair (or `-` and `=`) = the instrument's default position.

Every change sends one `SetTrackParam`, `SetMacro` or `SetGlobal` with the absolute value. The UI keeps its own "pending value" so that fast repeats don't wait for the round trip. The value pop-up (section 11.3) appears on every change.

### 5.3 Performance sweeps (always live, independent of focus)

| Keys | Target | Behaviour |
|---|---|---|
| `[` hold | `kGMasterFilter` toward 0 (low-pass closing) | Constant-rate glide of **48 units/s** while held. There's no acceleration, because a filter sweep must be musical and predictable. It latches where you let go. Crossing 64 pauses there for 150 ms (a "detent at OFF"). |
| `]` hold | `kGMasterFilter` toward 127 (high-pass opening) | Same. |
| `[`+`]` together | `kGMasterFilter` = 64 (off) | Instant. Toast "DJ FILTER OFF". |
| SHIFT+`[` / `]` | the bass's **macro 1** down/up (a CLASSIC bass: its CUT) | The same glide end to end (96 units/s over 0..255 for a macro, 48 over 0..127 for CUT). SHIFT+both = back to the value it had when the sweep started (the *bass return*). |
| tap `[` or `]` (< 120 ms) | ±1 | Fine nudge. |

The pop-up shows "DJ FILTER LP26", "BASS OPEN 62%" or "BASS CUTOFF 74" with a bipolar or unipolar bar. If a bass step locks that macro (or CUT), the lock still wins on that step. This is the contract's behaviour, and it's documented in help.

---

## 6. Key maps (all 56 keys, every mode)

Images: `docs/ux/keys-groove.png`, `keys-step-held.png`, `keys-step-write.png`, `keys-sound.png`, `keys-mix.png`, `keys-fx.png`, `keys-song.png`, `keys-project.png`, `keys-text.png`, `keys-modifiers.png`. A dash "-" means the key is ignored, with no sound and no toast. **G** means "global: same as GROOVE".

### 6.1 Plain keys

| Key | GROOVE | STEP HELD (GROOVE) | STEP-WRITE | SOUND | MIX | FX | SONG | PROJECT | NAME ENTRY |
|---|---|---|---|---|---|---|---|---|---|
| `` ` `` | close overlays; home | (G) home, step released | REC off, home | home | home | home | home | home | cancel |
| `1`-`8` | step 1-8 tap/hold | copy held step here | move cursor | step tap/hold (strip) | step tap/hold | step tap/hold | put P01-P08 in slot | - | type 1-8 |
| `9` | SOUND | SOUND (still holding: lock mode) | SOUND | next instrument (dev parameter pages: next page) | SOUND | SOUND | SOUND | SOUND | type 9 |
| `0` | MIX | MIX (lock mode) | MIX | MIX | next row | MIX | MIX | MIX | type 0 |
| `-` | focus -1 | field -1 | focus -1 | focus -1 | fader -1 | focus -1 | repeats -1 | - | type - |
| `=` | focus +1 | field +1 | focus +1 | focus +1 | fader +1 | focus +1 | repeats +1 | - | - |
| `del` | hold: erase (if playing) | remove p-lock | rest + advance | (held step) remove lock | (held step) remove lock | - | remove slot | delete file (confirm) | backspace |
| `tab` | tempo layer / tap | - | G | G | G | G | G | G | INT/SD |
| `q`-`i` | step 9-16 | copy held step here | move cursor | step | step | step | put P09-P16 | - | type |
| `o` | FX | FX | FX | FX | FX | next FX tab | FX | FX | type O |
| `p` | SONG | SONG | SONG | SONG | SONG | SONG | - | SONG | type P |
| `[` `]` | DJ filter sweep | G | G | G | G | G | G | G | - |
| `\` | PROJECT | PROJECT | PROJECT | PROJECT | PROJECT | PROJECT | PROJECT | LOAD/SAVE tab | - |
| `a`-`k` | pad: play + select | field VEL GATE/PTCH PROB RTCH ACC SLD CHRD LOCK | pad: play + select | pad (show its instrument) | pad (select strip) | pad; DUCK tab: focus its duck | pad | pad | type |
| `l` | chord mode (lead) | chord next (lead) | chord mode | - (developer mode: instrument page / parameter pages) | - | - | - | type L |
| `;` | octave + | note +1 semi (CTRL +12) | octave + | previous instrument (dev pages: page prev) | row prev | tab prev | slot up | list up | - |
| `'` | page | page (to copy across) | page | step page | step page | - | - | - | - |
| `enter` | REC on/off | - | REC off | REC | REC | REC | chain on/off | load / save | OK |
| `z`-`,` | notes 1-8 | set note 1-8 (drum: velocity) | write note + advance | macro keys: `z`/`x` macro 1 -/+, `c`/`v` 2, `b`/`n` 3, `m`/`,` 4 (dev pages: column 1-8) | row LEVEL DUCK DELAY REVERB PAN (`n` `m` `,` -) | column 1-8 | notes | notes | type (`,` -) |
| `.` | octave - | note -1 semi (CTRL -12) | octave - | next instrument (dev pages: page next) | row next | tab next | slot down | list down | - |
| `/` | help (hold) / explain (tap) | G | G | G | G | G | G | G | - |
| `space` | play / stop | G | play (to live rec) | G | G | G | G | G | type `_` |

STEP HELD in SOUND or MIX is the same as the SOUND/MIX column, except that `-`/`=` (and in SOUND the macro keys) write a **p-lock** of the focused macro or parameter on the held step, and `del` removes it.

### 6.2 Modifier and held-key combos (all views except NAME ENTRY)

| Combo | Action |
|---|---|
| FN (hold ≥ 200 ms alone) | Show the MUTE strip. |
| FN + pad | Mute on/off for that track (`SetMute`). |
| FN + note key *k* | Solo track *k* on/off (8.2). |
| SHIFT + step | Pattern length (4.2). |
| SHIFT + pad | Select silently. |
| SHIFT + note | Note with accent. |
| SHIFT + `-`/`=` | ×8 steps (a macro: 16 of 255). |
| SHIFT + macro key (SOUND) | That macro by 16 of 255 per tap or repeat. |
| SHIFT + `[`/`]` | Bass sweep: its macro 1 (CLASSIC bass: cutoff). |
| SHIFT + `'` | Add page. |
| SHIFT + `l` | Previous chord mode. |
| SHIFT + space | Playing: restart from step 1 (`Play a=0`). Stopped: continue from the current position (`Play a=1`). |
| SHIFT + step in SONG | Insert pattern before the cursor. |
| CTRL + `z` | Undo / redo (toggle). |
| CTRL + `;`/`.` (step held) | Note ±12. |
| CTRL + `x` / `c` / `v` | Cut / copy / paste the selected track. |
| CTRL + SHIFT + `x` / `c` | Cut / copy the whole pattern (paste is CTRL+V; the clip knows its type). |
| CTRL + SHIFT + step | Selected track length. |
| CTRL + `s` | Save. |
| CTRL + `n` | New project (confirm). |
| OPT (hold ≥ 200 ms alone) | Show the PATTERN pads. |
| OPT + step *n* | Queue pattern *n* (`QueuePattern`). Stopped: switch now. |
| OPT + SHIFT + step *n* | Copy the current pattern to *n* (undo snapshot of *n*), then queue *n*. |
| ALT (hold ≥ 200 ms alone) | Show the ROLL strip. |
| ALT + pad (held) | Roll that track at `rollRate` until released. |
| ALT + `;`/`.` | Roll rate. |
| TAB (hold ≥ 250 ms) | Show the TEMPO box. |
| TAB + `-`/`=` | BPM ±1 (SHIFT ±10). |
| TAB + `;`/`.` | Swing ±1. |
| TAB tapped (< 250 ms) ×3 or more | Tap tempo: the average of the last ≤4 intervals, set once 3 taps fall within 2 s. A gap over 2 s restarts the count. |
| G0 (side button) held + tilt | **Tilt** (10.3 "Tilt"): the selected track's tilt macros follow the device's roll (X) and tip (Y), from the pose at the press; release: they glide back. From every view. The firmware owns G0 and hands the gesture to the UI. |
| G0 held + `-`/`=` | Codec volume (firmware); cancels the tilt for the rest of that hold. |
| G0 tapped twice | Diagnostics strip (firmware). A tap (< 150 ms) never tilts. |

Modifier overlays appear 200 ms after the modifier goes down, **or immediately** when the first combo key is pressed, and close on release. If the modifier is released without a combo, nothing else happens. A modifier tapped alone has no function. That rule exists so that accidental taps are harmless.

---

## 7. Step detail and parameter locks

**STEP EDIT panel** (GROOVE, any step held ≥ 250 ms; mockups `step-edit.png`, `step-edit-drum.png`, `step-edit-lead.png`). Two keys at most: the step key plus one key tapped or held.

| Pad key | BASS | LEAD | DRUMS | `-`/`=` range |
|---|---|---|---|---|
| `a` VEL | ✓ | ✓ | ✓ | 1..127 |
| `s` GATE / PTCH | GATE | GATE | **PTCH** (drum note, 60 ± 24) | gate 1..64 quarter-steps (shown in steps: 0.25 .. 16); pitch -24..+24 |
| `d` PROB | ✓ | ✓ | ✓ | 0..100 in steps of 5 (tap) |
| `f` RTCH | ✓ | ✓ | ✓ | x1..x4 |
| `g` ACC | flip | flip | flip | (`-`/`=` also flip) |
| `h` SLD | flip | flip | - | |
| `j` CHRD | - | ✓ | - | the 10 ChordTypes, shown as names |
| `k` LOCK / MOVE | ✓ | ✓ | ✓ | CLASSIC: the locked value; instrument track: MOVE, the note's own macro values (see below) |

* When the panel opens, the **last-used field** is selected. It's VEL the first time.
* The note row writes the step's note and auditions it: the scale degree at the track octave for synths, or velocity levels for drums. `;`/`.` move it ±1 semitone (chromatic, not scale-locked, so any note can be reached). CTRL moves it ±12 (not SHIFT: with step 1 held, SHIFT+`;` and SHIFT+`-` close the same scan rectangle, 16.1).
* `l` (lead) moves the chord to the next type.
* **Tap another step key = copy the held step there** (all 8 bytes). Tapping `'` while the step stays held flips the shown page, so a step can be copied across pages. Each copy is one `SetStep`. Everything done while the step is held, copies included, is one undo unit (ui_edit.cpp).
* `del` removes the step's p-lock (CLASSIC) or its own macro values (instrument track: the focused macro's value if the note has one, else all of them).
* **LOCK field (CLASSIC track):** if the step has a lock, `-`/`=` change its value, with display formatted by that parameter's ParamInfo. If it has none, the first `-`/`=` creates a lock on the **focused** parameter (`trackFocus[sel]`), starting from its current value.
* **MOVE field (instrument track, `step-edit-motion.png`):** the note's own macro values (motion, 10.3). Four small marks right of the `k` cap (AMBER: the note has a value of that macro; TEXT: the focused macro without one; LINE: neither), the focused macro's name and the note's value of it (AMBER) or `-` (DIM). `-`/`=` set or move the focused macro's value in whole levels of its 63 (a tap one level, about 4 of 255; SHIFT 4 levels), starting from the macro's position. **`k` again** while the field is selected focuses the next macro of the instrument, so all four are reachable from GROOVE. Footer `DEL CLEAR`.
* Release of the step key closes the panel. If the step was on before the press and nothing was edited and the press was short, it toggles off (4.1).

**A note's macro values from SOUND** (instrument track, `instrument-lock.png`, `motion-held.png`). Hold a step key (the strip at y 114 frames it) and press a macro key or `-`/`=`: that note gets its **own value of that macro** (the macro's position ± one level of 63 the first time, then ± a level per tap, SHIFT 4 levels; both keys of a pair = the default on that note). **Each of the four macros can have its own value on the same note.** The macro's name and handle turn AMBER, the groove fills AMBER_D, a WHITE tick marks the base (the macro's position on the page) and the header shows "STEP n ◆LOCK". The values are glide targets: the macro moves from note to note (10.3 "Motion"). `del` while holding the step clears the focused macro's value, or, when the note has none of it, all its values. There are **no parameter locks on an instrument track** (a version-2 project's mixer and sound locks on instrument tracks are dropped when it loads; its macro locks become the notes' values); `-`/`=` on a mixer row while holding a step toasts "INSTRUMENT: HOLD STEP + A MACRO".

**Locks from SOUND/MIX on a CLASSIC track** (`sound-lock.png`). Hold a step key and press `-`/`=`: `lockParam = focus+1`, `lockValue = current ± delta`. The column bar turns AMBER, a white tick marks the unlocked base value, and the big read-out turns amber with a "◆LOCK" chip. A column key (`z`..`,`) may be tapped while still holding the step, to move the focus to another parameter first. **One lock per step (contract).** Locking a different parameter replaces the old lock. Toast: "LOCK CUT REPLACES RES". Globals (FX) can't be locked; the toast says "FX VALUES CAN'T BE LOCKED". The exception is a track's DUCK amount on the FX DUCK tab, because that's a track parameter.

A view key pressed while a step is held switches view and **keeps the hold**. So the path "hold step → `9` → `-`/`=`" works without letting go.

---

## 8. Performance

### 8.1 Pattern queueing (`pattern-overlay.png`)
OPT held shows 16 pattern pads at the step-pad positions. GREEN is playing, an AMBER double frame is queued, RAISED is in use (any step on), PANEL is empty. OPT+step queues it, and the status bar shows `P01>05` in amber, blinking at 2 Hz until the switch. Queuing the playing pattern cancels a queued one. In song mode, a hand-queued pattern is a one-off detour, and the chain carries on afterwards (engine decision). OPT+SHIFT+step copies the playing pattern to the target and queues it. Toast: "COPIED TO P05 · CTRL+Z UNDO". This is the main workflow for building variations.

### 8.2 Mutes and solo (`mute-overlay.png`)
FN held shows the strip. FN+pad toggles `SetMute`. FN+note key *k* solos track *k*: the UI saves the mute mask and mutes all other tracks. FN+the same note key restores the mask. FN+another note key moves the solo. Changing a mute while soloed ends the solo and keeps the current mutes. Mutes act immediately. Quantised mutes are a later feature. In the overview, a muted track's tag becomes a hollow box, its steps go grey, and its lamp shows "M".

### 8.3 Roll (`roll-overlay.png`)
ALT+pad held retriggers that track at `rollRate`, locked to the grid, at the velocity of the pad, until released. Roll can be held on two tracks at once (two pads), which is within the two-key limit. Roll is never recorded. This needs CCR-1. Without it, the UI times `NoteOn`s from its own loop. That's acceptable at ≤ 1 ms UI jitter, and otherwise roll ships disabled.

### 8.4 DJ filter, bass cutoff: section 5.3.

### 8.5 Erase
Holding DEL while playing in GROOVE clears (`SetStep` with kStepOn cleared) each step of the selected track as the playhead enters it, watched from `status.step[sel]`. The first erase stores an undo snapshot. A small red "ERASE" chip replaces the footer context while DEL is held. When stopped and not in step-write, DEL does nothing.

### 8.6 Live record
REC while playing turns on `SetRecord 1`, the status dot is solid red, and the selected track's pads get a thin red top edge. Pads record their own track, and note keys record the selected track (quantised by the engine). Held time becomes the gate (from the first key that landed on the step to the release of the last of them). Legato gives a slide (see CCR-2): the previous step gets SLD and a gate that reaches the new note (any distance, across pages and the loop point). Recorded gates are capped at the step's loop (min of track and pattern length, 63.75 steps at most), and a step held longer than the loop is not retriggered under the finger. SHIFT gives an accent. Steps can still be toggled.

---

## 9. Note entry

### 9.1 Scale (project-wide)
The scale is `kGScaleRoot` + `kGScaleType` (FX → MASTER columns 7-8). Changing the scale **never rewrites** existing steps. It only changes what the note keys play from then on. The footer shows "A MIN".

### 9.2 Step-write (303 style, `groove-stepwrite.png`)
Press `enter` while stopped. The REC dot blinks at 1 Hz, the footer shows "STEP WRITE" in red, and an AMBER frame marks `stepCursor` on the selected track (start: step 1 of the shown page, or where you last left it).
* **Note key** = write the note at the cursor (synth: note, vel 100, gate 2, on; drum: on, velocity from the key), audition it, and move the cursor +1, wrapping at the track length and following the page.
* **SHIFT+note** = the same plus accent.
* **Previous note key still held when the next is pressed** = set slide on the *previous* step, then write the new one. This is the 303 slide, with no modifier.
* **DEL** = rest: clear the cursor step and advance.
* **Step key tap** = move the cursor there. Holding a step key still opens STEP EDIT.
* SPACE starts playback. Step-write becomes live record, and stopping goes back to step-write. `enter` or `` ` `` leaves REC.

### 9.3 Lead chords
`chordMode` determines what a note key, a new step and a step-write produce on the lead:
* **AUTO** (default): build the triad from scale degrees d, d+2 and d+4. Map the intervals: (4,7) is MAJ, (3,7) is MIN, (5,7) is SUS4. Anything else (a diminished or pentatonic degree) is 5TH if a perfect fifth is in the scale above the root, and otherwise OCT. The result is always in key.
* **AUTO7**: degrees d, d+2, d+4, d+6. (4,7,11) is MAJ7, (3,7,10) is MIN7, (4,7,10) is DOM7. Otherwise use the AUTO rule.
* The fixed types (NONE, OCT, 5TH, MAJ, MIN, MAJ7, MIN7, DOM7, SUS4, MIN9) apply that chord on every key.
* The footer shows the mode ("AUTO"). The step pad shows the stored chord ("m7", "M7", "5", "8", "m", "M", "7", "s4", "m9").

### 9.4 Not in v1
There's no chromatic piano layout. Chromatic notes are reachable with CHRO scale or with `;`/`.` in STEP EDIT. There's no recorded parameter motion and no arpeggiator.

---

## 10. Screens: pixel layout

All coordinates are inclusive pixels on the 240x135 framebuffer, with x to the right and y down. Fonts: **S** = 5x7 glyph in a 6x8 cell (advance 6), **L** = S at 2x (advance 12, cap 14), **XL** = S at 3x. Colours are palette names (section 12). Unless stated otherwise, text is drawn left-aligned at its x. "chip" = a rounded rect 9 px tall, `6*len+3` px wide, with text inset 2 px in BG ink.

### 10.1 Status bar (every view except HELP), y 0..10, fill PANEL

| Region | Pos | Content |
|---|---|---|
| Transport | icon at (3,2) | play triangle GREEN when playing, stop square DIM when stopped |
| Rec | (11,2) | round dot: RAISED when off, RED blinking at 1 Hz in step-write, RED solid in live rec |
| Pattern | (20,2) S WHITE | `P01`. Then either `>05` AMBER (queued, blinking 2 Hz), or a chain icon + slot `02` GREEN when song mode is on |
| Tempo | (64,2) S TEXT | `%3d` BPM |
| Beat | rect (84,4) 3x3 | WHITE for 80 ms on every quarter note, else LINE |
| Focus chip | (92,1) | chip with track name in track colour (or "FX" CYAN), then short name TEXT, value WHITE right-aligned to x 165. SONG shows `SLOT n P05 x2`, PROJECT shows the project name + `*` if dirty. While a **tilt** is engaged (10.3 "Tilt"), except on the instrument page of the tilted track: the tilted track's chip, `TILT` WHITE, a small double arrow (track MID), and a 9x9 RAISED well at x 156 with a 3x3 WHITE dot that shows the pose (X right, Y away = up, 3 px at full range) (`tilt-groove.png`) |
| Level meter | x 171..204, y 3..7 | RAISED well. Fill = `max(peakL,peakR)`·34 px, GREEN up to 26 px, AMBER beyond. A clip (≥ 1.0) draws RED 2x7 at x 203 for 1 s |
| Duck meter | x 208..211, y 1..9 | RAISED well, CYAN filled from the top by `(1-duckGain)·9` px (gain reduction) |
| Battery | frame (217,2) 16x7 DIM + nub (233,4) 2x3 | Fill (219,4) `12·pct` x3: GREEN > 30 %, AMBER > 10 %, RED otherwise. Below 10 % it blinks once per 2 s |

### 10.2 GROOVE (`groove-drum.png`, `groove-bass.png`, `groove-lead.png`, `groove-live.png`, `groove-page2.png`, `groove-stepwrite.png`)

* **Overview**: 8 rows, row *t* at y = 13 + 9t (9 px pitch). Row content is y+1..y+7.
  * The selected row gets a full-width background of track TINT.
  * Pad tag at x 1..7: a filled track-colour box 7x9 with the pad letter (`A`..`K`) in BG. If the track is muted, it's a hollow DIM 7x7 box with a 3x3 DIM centre.
  * Name at x 11, S: WHITE if selected, track MID otherwise, DIM if muted.
  * Mini steps: cell *i* at x = 42 + 11i + 2·(i div 4), 10x7.
    * Off: PANEL, or RAISED on beats (i mod 4 = 0), or LINE under the playhead.
    * On (drums): full track colour, MID if prob < 100, WHITE under the playhead, LINE if muted. Accent = a 1 px WHITE top line.
    * On (synth): a 10x7 TINT cell with a 3 px bar whose height position is the note within the pattern's note range (a mini piano roll, 5 levels). Slide extends the bar 3 px into the gap.
  * Activity lamp x 228..235, y+2..y+6: WHITE for 80 ms after `status.trig[t]` changes, track DIM otherwise. Muted: PANEL with "M" DIM.
* **Step pads**: 16 pads 27x18 (rounded). Pad *i*: x = 4 + 29·(i mod 8) + 2·((i mod 8) div 4), y = 88 (i < 8) or 108.
  * Off: PANEL (RAISED on beats), with the key legend centred at (x+11, y+6) in LINE.
  * On: track bright fill (MID if prob < 100). The key legend at (x+3,y+2) is in track DIM.
    * Synth: note name right-aligned to x+23 at y+9, BG.
    * Drum: velocity bar at (x+3, y+12), 3 px tall, `vel·21/127` wide, BG.
  * Markers along the top, right to left from x+23: chord label (lead), lock ◆, ratchet ticks (1x5 per hit, 2 px pitch), and `%` for prob < 100. A marker is dropped if it would reach x+9.
  * Accent: a 2 px WHITE bar at the top edge. Slide: a WHITE 3 px bar at y+12 that bridges into the next pad.
  * Playhead: WHITE fill (markers in DIM). Held: a double WHITE frame. Step-write cursor: a 1 px AMBER frame outside plus an AMBER underline.
  * Beyond length: a RAISED outline only.
* **Footer** y 127..134:
  * Page indicator at (4,128): 4 segments 7x5 at a 9 px pitch. Unused = RAISED outline, used = LINE, shown = WHITE, and the playing page gets a 3x3 GREEN dot.
  * Range `1-16` TEXT + `/16` DIM at x 46.
  * Context at x 94, which must end ≤ x 200: synth `A MIN` (track colour) + ` OCT1`; lead adds ` AUTO`; drum `NOTES=VEL`; step-write `STEP WRITE` RED; erase `ERASE` RED.
  * `/HELP` DIM right-aligned to x 235.
* **STEP EDIT panel** replaces y 12..85 (PANEL, a 1 px track-colour line at y 12):
  * Title `STEP 14` WHITE at (4,16) + track chip. Right-aligned hint DIM at y 16.
  * Big read-out L at (4,28): note name, or velocity for drums.
  * 8 note cells at x = 50 + 23k, y 27, 22x17. Key letter at (+2,+1). Value right-aligned to +19 at y+9. The current note or velocity is filled in track colour, the rest RAISED.
  * 8 field cards at pad x, y 48, 27x37: key (+3,+2), name centred y 59, value centred y 68, bar 21x3 at y 79. The selected card is RAISED with a WHITE frame, and fields not available for the track are drawn as an empty RAISED outline.
  * The footer becomes `TAP STEP COPY HERE  DEL NO LOCK`.

### 10.3 SOUND: the instrument page (`instrument.png`, `instrument-labels.png`, `instrument-moving.png`, `instrument-lock.png`, `instrument-list.png`, `instrument-lead.png`, `instrument-kick.png`, `instrument-hat.png`, `instrument-silence.png`, `instrument-classic.png`, `motion-held.png`, `motion-playing.png`, `motion-live.png`, `tilt-marks.png`, `tilt-held.png`, `tilt-held-left.png`, `tilt-groove.png`, `tilt-classic.png`)

Real-UI screenshots with real audio in the picture, drawn by `build/host/ui_shot host/ui_scripts/instrument.txt docs/ux` (the script posts a groove and lets the engine play before each shot). Layout code: `drawInstrument` in `lib/pg/src/pg/ui/ui_draw.cpp`; the picture's logic: `lib/pg/src/pg/ui/scope.h`.

* **Header** y 13: track chip at x 4, then the **instrument's name** in FONT_L (WHITE); right: its place in the track's list `2/5` DIM, or while a step is held `STEP n` + an AMBER `◆LOCK` chip. No caption under the name: a set's `about "..."` line (docs/MACROLAB.md) is an author's note and is not displayed, and the picture starts right under the header.
* **The picture** (`kScopeX0`..: x 4..235, y 29..72): a PANEL box, a dotted RAISED centre line, and the selected track's own voice (before level, pan and the mix) as an oscilloscope trace: the edge in the track colour, filled toward the centre line in track DIM (a muted track: MID edge, TINT fill). One trace value per column (232). What the trace shows (`Scope`):
  * synths: about 2 (bass) or 3 (lead) periods of the note the track last started, so the picture has the same scale in every register; the window starts at a rising crossing of the wave's middle, the one that best matches the previous frame, so a held note stands still; a note that started within the window is drawn from its attack at the left edge (`instrument.png`);
  * drums: from the hit's note-on over a fixed time per voice (kick 340 ms, snare and clap 260, closed hat 70, open hat 420, perc 300 ms), growing from the left as it plays and held until the next hit; drawn with a square-root scale so the decay reads (`instrument-kick.png`, `instrument-hat.png`);
  * gain: the trace's peak sits near the top of the box; it drops at once and rises slowly (about 15 dB/s, at most +52 dB), so the hats (15 to 20 dB quieter than the rest) fill the box and a decaying note still decays;
  * silence: when the voice stops, or a synth's window falls far below its recent level (the note's release), the last picture fades to the calm centre line (a 1 px line in track MID) within a few frames (`instrument-silence.png`).
* **The macros**: four horizontal sliders in a 2 x 2 block, the left hand's pairs on the left (macro 1 `z x` at x 4, macro 2 `c v` under it), the right hand's on the right (macro 3 `b n` at x 124, macro 4 `m ,`), each cell 112 px wide. The two slider rows meet in the middle (y 84 and y 94); the top row's labels sit above it (y 75), the bottom row's below (y 104), so every label is next to its own slider. A cell:
  * labels: the low end label left-aligned, the high end label right-aligned, the macro's name centred between them (TEXT; the end labels DIM, TEXT when the macro is lit). Lit = focused, moving, held-step value or live: the name becomes a chip, in the track colour (focused), WHITE (moving now: a key of the pair down, or a step within the last 350 ms), AMBER (the held note has its own value of it) or RED (live: being recorded, "Motion" below). Two macros moving at once are both WHITE. An AMBER **motion marker** left of the name, the ◆ a step with its own value carries (STEP EDIT, the strip, `◆LOCK`): notes of the shown pattern carry their own values of this macro (it has motion), also when stopped; one symbol, one meaning (on a step: this note has a value; by a macro's name: notes of this pattern have values of it). 2 blank columns between it and the name; where the centred name leaves it no room, the marker, the name and the tilt mark are centred together between the end labels, then the same with 1 blank column (to the name and to the low label), then without the tilt mark (the marker wins: it says why the slider moves; TICK's DK-BRT); it is left out only when even the marker and the name alone do not fit (no built-in instrument; `itest_ui_instrument` checks every one). The live (RED) chip of a macro being recorded keeps its marker. A **tilt mark** right of the name (2 px after it, a glyph's 7 rows): a left-right double arrow (7 px wide) on the macro tilt's X axis moves, an up-down one (5 px) on Y's; DIM, WHITE while the tilt is engaged on this track (`tilt-marks.png`, `tilt-held.png`). Where the centred name leaves the mark no room before the high end label, the name and the mark are centred together (`instrument-hat.png`); left out only when even that does not fit.
  * the slider row (9 px): a key cap at each end (RAISED 9x9, the key letter TEXT, WHITE when lit; the cap turns WHITE with BG ink while its key moves the macro), a 3 px RAISED groove between them filled from the left up to the position (track DIM, MID when lit, AMBER_D when locked), the handle (a 5x9 cap with a 1 px BG outline: track MID, the track colour when focused, WHITE when moving, AMBER for a held note's value, RED when live), the **default** as a small DIM notch under the groove, and the positions where an **enum parameter jumps** (`macro::enumJumps`, the other macros where they are; recomputed one macro per frame when they move) as 1x2 TEXT ticks above it. A held note's value: the handle at the value and a 1x9 WHITE tick at the base (the macro's position) (`instrument-lock.png`, `motion-held.png`). While a pattern with motion plays, the handle is where the engine has the macro now (`EngineStatus::macroPos`, it moves by itself) and the WHITE tick marks the base (`motion-playing.png`).
  * a macro the set doesn't have: LINE outlines of the caps and a flat RAISED groove.
* Step strip y 114 as before (a note with own values = AMBER top), footer `;/. INSTRUMENT  HOLD STEP+KEY ◆` (a sampler: `;/. INSTRUMENT  SFT+;/. SLOT`), or while held `MOVE A MACRO = NOTE VALUE  DEL`. While the **focused macro has motion** in the shown pattern (and no step is held) the footer says how to change it instead: `STEP+KEY ◆  REC+MOVE  DEL+KEY` (`STEP` `REC` `DEL` WHITE, the rest DIM, the ◆ AMBER): hold a step + the macro's keys = that note's value, REC on + move it while playing = record it again, hold `del` + one of its keys = clear it ("Motion" below; `motion-playing.png`). The value pop-up is not shown here (the sliders show the value; the status-bar chip shows the focused one's).
* **The instrument list** (`instrument-list.png`): after `;` `.` `9`, for 1.5 s, a column over the picture's right end (x 144..235, y 29..72, RAISED, framed in the track colour): the current instrument as a chip with its place (`2/5`), the previous one above and the next one below (the list wraps, marked with DIM arrows).
* **CLASSIC** (`instrument-classic.png`): the chip, `CLASSIC` in FONT_L, `OWN SAVED SOUND` (DIM) at the header's right, the same picture, then `PICK AN INSTRUMENT` (WHITE) with `5 FOR BASS` (DIM) and two cards where the slider block would be: the `.` `9` key caps and the first instrument's name in FONT_L (track colour), the `;` cap and the last one's; under them `CTRL+Z RIGHT AFTER = BACK TO THIS` (DIM). An id this firmware doesn't know shows `UNKNOWN` and `NOT IN FIRMWARE` (both amber); it plays as saved.
* **Redraw.** The page redraws as any other when its state changes; on frames where only the picture moved, only the picture's band (rows 29..72, with the status bar if that changed too) is redrawn and pushed (`Ui::dirtyRows`). When something is drawn over the band (an overlay, the list, a toast) the frame is redrawn whole.

**Design notes.** Sliders, not bars or dials: at 240x135 a horizontal slider 112 px wide gives the position, the default, the jump ticks and both end words room on one line each, and the key caps at its ends say which key goes which way. Vertical faders beside the picture left no room for the end words (up to 8 characters) or the names; dials of 25 px could not carry two end words in a 60 px column. The 2 x 2 block keeps each hand's pairs on its own side of the screen, as on the keyboard.

**Keys.** `z`/`x` macro 1 down/up, `c`/`v` macro 2, `b`/`n` macro 3, `m`/`,` macro 4: tap = one small step, hold = sweep with acceleration (5.2), SHIFT = coarse (16 of 255), both keys of a pair = the instrument's default position. Each key repeats on its own, so two macros move at once with two fingers (any key of one pair with any key of another closes no rectangle of the scan matrix, also with SHIFT held: docs/ghosting-audit.md). Moving a macro focuses it (`-`/`=` move it from any view). `;` previous / `.` next instrument, `9` again = next; the change is audible at once, a new instrument starts at its default positions or where it was when the track last left it in this session; from CLASSIC `.` picks the first, `;` the last. A run of steps while the list is up is one undo unit: CTRL+Z goes back to what the track had before (CLASSIC included, its stored sound exactly), CTRL+Z again redoes. A step held + a macro key or `-`/`=` = that note's own value of the macro (7); `del` clears it. **Hold `del` + a key of a macro** = clear that macro's motion: its values leave every note of the selected track in the shown pattern (the notes stay); toast "OPEN MOTION CLEARED  CTRL+Z UNDO" (one undo unit), or "OPEN HAS NO MOTION HERE".

**Motion** (project version 3; the engine's contract: engine.h "MOTION"; docs/MACROLAB.md "Motion"). A note (a step that is on) can carry its own value of each macro. While the pattern plays, each macro moves in a straight line from one such note's value to the next one's, arriving exactly when that note starts, through silence and notes without a value, across the pattern's end into whatever plays next (its first such note, or the base when it has none). One note with a value: the macro stays there. No such note in the playing pattern: the macro sits at its base, the position the page shows when stopped.
  * *Playing* (`motion-playing.png`): the sliders of macros with motion move by themselves; the scope keeps running. A key move of such a macro changes its **base** only (the WHITE tick moves, the stored position; heard where the pattern has no values). Because that move is not heard, the first one (its keys or `-`/`=`, on this page, not recording) shows the toast "OPEN FOLLOWS NOTES: HOLD STEP+KEY ◆" (the macro's name; kind 1, 2.5 s), at most once every 6 s; never while recording (the move is recorded) or stopped (the move is heard). What the move does is unchanged.
  * *Recording* (`motion-live.png`): with REC on and the transport running, moving a macro (its keys, tapped or held; the lab's sliders and pad) makes it **live**: from its first move until it has been still for **one bar** (16 steps at the tempo then), or REC goes off, or Stop. A live macro is heard as the hand moves it (not the old motion), its name chip and handle turn RED, and every note of the track that starts meanwhile takes its current value (rounded to 63 levels; empty steps get nothing). The pass is one undo unit together with the notes recorded in it ("RECORDED MOTION" / "RECORDED NOTES", 13.1).
  * *Stopped*: the page shows the base positions. The sound rests where the motion left it until a macro moves (it glides to its base) or Play starts a pattern without motion (back to the base).
  * GROOVE (`groove-motion.png`): a note with own values carries the ◆ marker a lock carries, and its strip pad an AMBER top.

**Tilt** (stage D; the engine's contract: engine.h "TILT OFFSET"; the maths: `lib/pg/src/pg/tilt/tilt.h`; the sets' choices: docs/MACROLAB.md "Tilt"). Hold **G0** (the side button) and tilt the device: two macros of the selected track's instrument move with it, X with the roll (right side down = up), Y with the tip (far edge away = up). The pose at the press is the zero, so it works at any holding angle; release glides back in 150 ms.
  * *Which macros*: the instrument's `tilt` lines, by default macro 1 on X and macro 2 on Y; each axis has the response its control asks for (`fast`, `smooth`, `fine`, `wide`: smoothing, dead zone and range). The marks next to the names say which (above).
  * *Additive and momentary*: what is heard is the macro's position (or its motion) plus the tilt's offset, clamped to 0..255. The position (the base tick, the stored byte) does not move; nothing about tilt is stored or saved. An axis at its full range moves the macro 160 of 255: from the middle an end is reached at 79 % of the range (about 28 degrees with the default response).
  * *On the page* (`tilt-held.png`, `tilt-held-left.png`): the handles move where the engine has the macros (`EngineStatus::macroPos`) and the WHITE tick marks the position, as with motion; the marks turn WHITE; for a moment after the release the handles follow the glide back. On every other view the status bar shows the gesture instead of the focus (10.1, `tilt-groove.png`).
  * *Timing*: a hold engages after 150 ms (`Ui::kTiltEngageMs`), so a tap, and the double tap of the diagnostics strip, never moves anything; the tracked pose is still the one at the press. The track (and its macros) are taken when it engages; selecting another track during the hold changes nothing until the next hold. Each new reading (100 per second on the device) glides there in 15 ms.
  * *G0 + `-`/`=`* (the firmware's volume keys) cancels the tilt for the rest of that hold: it glides back, the key never reaches the UI.
  * *Recording*: with REC on and the transport running, once the tilt has left the dead zone in a hold the tilted macros are **live** (RED, as with the keys) and every note that starts takes the heard value; the release ends it at once (the keys' "still for a bar" rule is for keys, which cannot say when the hand lets go). Over a motion the hand takes the macro where the motion has it. One undo unit with the pass ("RECORDED MOTION"). A hold kept still records nothing.
  * *CLASSIC* (or an instrument whose `tilt` lines give no axis): nothing moves; one toast per hold, `TILT: PICK AN INSTRUMENT (9)` / `NO TILT ON THIS INSTRUMENT` (`tilt-classic.png`).
  * *Explain mode*: G0 explains itself (`G0: TILT / VOLUME / DIAG`) and does not tilt.
  * *Desktop*: no IMU; while G0 (F1 / Esc) is held the mouse is the tilt, through the same path (docs/MACROLAB.md "Tilt").

**Sampler instruments** (stage F1; `sampler-chop.png`, `sampler-slot.png`, `sampler-user.png`, `sampler-stutter.png`, `sampler-tape.png`, `sampler-step-edit.png`; recording, stage F2: `mic-waiting.png`, `mic-recording.png`, `mic-done.png`, `mic-replace.png`, `mic-erasing.png`). PERC's list also holds CHOP, TAPE and STUTTER, which play a **sample** instead of the percussion voice (the set's `voice perc sampler` line, docs/MACROLAB.md). The track plays one of twelve **sample slots** (`TrackSettings::sampleSlot`, saved with the project; slot 1 by default). An empty slot plays a built-in sound (three synthesised voice phrases, slot n plays sound n mod 3), so the instrument makes sound out of the box.
  * *The picture*: the scope's box shows the slot's waveform (its 232-column overview, one column per pixel) instead of the scope. The region the next note plays sits on a RAISED band, drawn in the track colour; the rest is the track's dim shade. With more than one slice, dotted lines mark the slices, the chosen slice's in the track's mid shade. A WHITE line is the playhead while a chop of that slot sounds. Top left: the slot number as a track-colour chip and the sample's name, or `BUILT-IN` (DIM) for an empty slot; top right: its length (`1.2S`). No caption.
  * *Choosing the slot*: **SHIFT+`;`** / **SHIFT+`.`** step to the previous / next slot (wrapping 12 to 1) and toast `SLOT 3 OHYEAH` / `SLOT 4 BUILT-IN`; the next note plays it. A run of steps (each within 2 s of the last) is one undo unit: CTRL+Z takes the slot back, again redoes it. Plain `;` / `.` still step the instrument; on a native instrument SHIFT+`;` / `.` do what `;` / `.` do. With a step held SHIFT is ignored (SHIFT+`;` with a step key is an ambiguous matrix rectangle, 16.1). The footer reads `;/. INST  SFT+;/. SLOT  L MIC` (`INST` since F2, so `L MIC` ends before `/HELP`).
  * *Notes*: on a sampler instrument the note row plays, step-writes and (STEP EDIT) sets the chop's **pitch**: `z` is the sample as recorded (note 60), the others the scale's degrees above it (the scale's root is not added: the sample's own key is unknown). `;`/`.` with a step held: ±1 semitone, CTRL ±12, as before. The STEP EDIT field `s` is **GATE** (the step's gate ends the chop, then its release), not the drums' PTCH; velocity is the level.
  * *Getting audio in*: the desktop's `samples/` folder, a file dropped on the window (DESKTOP.md "Samples"), or `tools/pg sample put` for the device (MANUAL.md "Samples"). While the device writes a slot the toast line shows `SAMPLE 3: 45%`, then `SAMPLE POOL UPDATED`.
  * **Recording** (stage F2; the UI: `lib/pg/src/pg/ui/ui_mic.cpp`; the portable signal path and state machine: `pg/sampler/recorder.h`; the device: `src/platform/mic.*`, `audio_out.h` MICROPHONE; the desktop: `host/app/mic.*`). **Hold `L`** on a sampler instrument's page (no step held) to record the microphone into the track's slot; let go to stop. `L` was free there (on the instrument page it only flips pages in developer mode, which a sampler page now gives up; it is a single held key, so no ghosting rectangle; it is not `enter` (pattern REC), a macro key, the slot gesture or G0). The states, all in the picture's box:
    * *Waiting* (`mic-waiting.png`): the slot chip, a hollow red dot and `WAITING`; a dotted centre line; along the bottom the input level (-60..0 dBFS, the track's mid shade, the track colour once above the threshold, RED near full scale) with a WHITE tick at the threshold. For the first 150 ms nothing can start (the click of `L` itself; the room is measured from 50 ms on: the threshold is the quietest 10 ms window's RMS + 20 dB, kept within -41..-20 dBFS). The first sample above it starts the recording, with the 40 ms before it in front (the pre-roll), so the sample starts on the sound, not on the key.
    * *Recording* (`mic-recording.png`): a filled red dot, `REC` (RED) and the time (`1.7S`, WHITE, right); the wave so far in the track colour, scaled to its own peak, growing from the left with a WHITE write head; **the box is the whole slot** (6.2 s), so the wave reaches the right edge exactly when the slot is full, and recording stops there by itself. The level meter stays.
    * *Done* (`mic-done.png`): on release the last 60 ms are dropped (the click of the key going up), the slot holds the sample (`REC03`, the slot number), the picture is its waveform at once, levelled as it plays, and the toast says `SLOT 3 REC03 1.6S`. A release before any sound: `NO SOUND`, the slot stays as it was. A slot holding a sample asks first (`mic-replace.png`, the confirm dialog's style: `REPLACE SLOT 3?`, `REC03 IS ERASED`, `ENTER  ERASE`, `ESC  CANCEL`); ENTER erases it (`SLOT 3 EMPTY`), then hold `L` again. No undo.
    * *Erasing* (`mic-erasing.png`): a slot that is empty but not fully erased (a transfer cut off, a slot from an older firmware) is erased before it can record: `ERASING` (AMBER, large) is drawn first, the erase runs on the next frame (the device: the output fades out, the audio stalls for the erase, about 1-2 s by estimate, then fades back), and if `L` is still held it goes straight on to *Waiting*. The replace path above shows it too.
    * Elsewhere `L` does what it did (CLASSIC / native instruments: nothing; developer mode: the page flip). Errors are toasts: `NO MIC` (no microphone), `MIC: 44.1K ONLY` (the device at 32 kHz), `MIC: I2S ONLY` (the M5 speaker backend), `MIC: <why>` when it fails. While a recording runs the UI does not autosave, a project save says `RECORDING: SAVE AFTER`, and the device's transfers that write flash answer `ERR RECORDING`.
    * Explain mode: `L` on a sampler page is `HOLD: RECORD THE MIC`. The key-map overlay (hold `/`) does not show it (the INSTRUMENT table is shared by every instrument); the footer does.

**Developer mode** (the desktop app's `--dev`, implied by `--lab`; never on the device): a CLASSIC track shows the parameter pages below, and `l` flips between them and the instrument page on any track.

### 10.3a SOUND parameter pages, developer mode only (`sound.png`, `sound-kick.png`, `sound-lock.png`)
* Header y 14: track chip at x 4, `SOUND` TEXT, page numbers (the current one as a WHITE chip). Right: long name DIM (upper case) + value L WHITE right-aligned to x 235 at y 13.
* 8 columns, column *c* centred at cx = 30c + 15:
  * Name at y 32. The focused column's name is a track-colour chip.
  * Bar well 14x52 at (cx-7, 41): PANEL, or RAISED if focused.
    * Fill is track MID, or bright if focused, or AMBER if showing a lock (with a WHITE 18x1 tick at the base value).
    * Unipolar: the fill rises from the bottom.
    * Bipolar/semitone: the fill runs from the centre line (DIM 18x1).
    * Enum with ≤ 8 values: stacked segments, with the chosen one filled.
    * Enum with > 8 values: a unipolar bar with 1 px separators.
  * Value text centred at y 96 (WHITE if focused). Key letter at y 105.
  * Unused slots: a "-" in LINE.
* Step strip y 114: 16 cells 13x8 at x = 4 + 14i + 2·(i div 4). On = track colour, playhead WHITE, lock = a 2 px AMBER top, held = a WHITE frame.
* Footer: `HOLD STEP + -/= ◆LOCK`, or while held `-/= LOCK VALUE  DEL REMOVE LOCK`.

### 10.4 MIX (`mix.png`, `mix-duck.png`, `mix-pan.png`)
* Row tabs at y 14 from x 4: key letter + name. The active tab is a WHITE chip. PAN is drawn in LINE (mono).
* Strip *t* at x0 = 30t, centre cx = x0+15:
  * The selected strip gets a TINT rect (x0+1, 26) 28x98 with a MID frame.
  * Trig lamp 10x2 at y 29.
  * Fader well 10x56 at (cx-5, 34): track MID fill (bright if selected) rising from y 89, with a 14x2 cap (WHITE if selected, TEXT otherwise). PAN is drawn bipolar from y 62.
  * Value centred at y 93.
  * Name chip at y 103, or a hollow `MUTE`.
  * Pad key at y 115.
* Footer `A-K STRIP  -/= MOVE  Z-B ROW`. On PAN: `PAN: HEADPHONE OUT IS MONO` AMBER.

### 10.5 FX: SPACE and MASTER (`fx-space.png`, `fx-master.png`)
* Tabs at y 14 (`DUCK SPACE MASTER`, the active one a CYAN chip). The rest is identical to SOUND, using CYAN / CYAN_D. Footers: SPACE `MIX VIEW SETS EACH TRACK'S SENDS`, MASTER `[ ] DJ FILTER FROM ANY VIEW`.
* SPACE columns: DTIM DFB DTON RSIZ RDMP DWID.
* MASTER columns: BPM SWNG VOL FILT FRES DRIV ROOT SCAL.

### 10.6 FX: DUCK (`fx-duck.png`, `fx-duck-track.png`)
* Tabs at y 14. `GAIN 68%` CYAN right-aligned at y 15 shows the live `status.duckGain`.
* Curve panel (4,27) 148x50 PANEL; plot area x 6..149, y 30..74. The x axis is 1 beat at the current tempo, and gain runs 1 (top) to 0 (bottom).
  * The curve is computed by the UI from DDEP / DHLD / DREL / DCRV using the engine's formulas (comments in globals.cpp: floor = 1 - DDEP/127; hold = 200 ms·(v/127)²; release = 20 ms·50^(v/127); curve log..lin..exp).
  * The curve is a CYAN 2 px line with a CYAN_D fill below it, plus dotted LINE quarter-beat guides.
  * A trigger line in KICK colour at x 6, labelled with the source name (`KICK`, `BEAT`, or `OFF` in DIM, in which case the curve is flat).
  * The live position (a white 5x5 dot plus a LINE vertical) sits at the current time since the last trigger. The UI estimates this from `status.step[0]` and the tempo, and puts the dot's y at `duckGain`.
* Ducked bars: x 160 + 10t, y 38, 7x28, height = the track's DUCK param, with the pad letter at y 69. The header at (160,27) reads `DUCKED`. After a pad press it reads the track name in track colour + its value WHITE right-aligned. The other bars dim to track DIM, and the chosen one gets a WHITE frame.
* Param cards: 6 at x = 4 + 39c, y 80, 37x32. Key at (+3,+3), name right-aligned at (+33,+3), value right-aligned at (+33,+13), a 31x4 bar at y 104. The focused card is RAISED with a CYAN frame. Enum cards use 3 segments.
* Kick strip at y 116. Footer `A-K EDIT THAT TRACK'S DUCK AMOUNT`.

### 10.7 SONG (`song.png`)
* The status chip shows `SLOT 4` WHITE, `P05` and `x2` AMBER.
* Header: a `SONG` chip, then `CHAIN ON` GREEN chip / `CHAIN OFF` DIM.
* Pattern grid 8x2, cells 17x17 at a 19 px pitch from (4,28). GREEN = playing, RAISED = in use, PANEL = empty.
* Help rows from (4,71), at a 10 px pitch.
* Chain panel (158,13) 82x112 PANEL. Header `CHAIN` + `6/32`. Rows from y 27 at a 10 px pitch, showing 10 visible rows: a play triangle GREEN on the playing slot, the slot number DIM, `P05` TEXT, `x2` AMBER. The cursor row is a WHITE bar with BG text. The last row is `END`. The list scrolls so the cursor stays in rows 2..8.
* Footer `DEL REMOVE SLOT  OPT+STEP QUEUE`.

### 10.8 PROJECT (`project-load.png`, `project-save.png`, `confirm.png`)
* Header: `PROJECT` chip, `LOAD`/`SAVE AS` tabs. Right: `SD ✓` CYAN, or `NO SD` DIM.
* Rows from y 28 at a 10 px pitch, up to 9 visible:
  * Mount tag at x 6: ROM GREEN (the demo, read-only), INT AMBER, SD CYAN.
  * Name at x 30.
  * BPM read from the file header, right-aligned to x 214, plus `BPM` RAISED.
  * The cursor row is a WHITE bar 236x9 at x 2.
  * The SAVE AS tab gets a first row `+ NEW NAME...`.
* Sort order: ROM, then INT A-Z, then SD A-Z.
* Name dialog (30,34) 180x67: an AMBER title bar, the name in L on BG with an AMBER 10x14 cursor block, `TAB SAVE TO INT|SD`, and `ENTER SAVE  ESC CANCEL`. Confirm dialog: same box, with 4 text lines at y 52 + 11n.

### 10.9 Overlays
* **PATTERN** (OPT): y 72..134. AMBER top line. Chip `PATTERN`, `▶01 NEXT 05`, hint `+SHIFT = COPY TO`. 16 pattern pads at the step-pad positions.
* **MUTE** (FN): y 83..134. WHITE top line. Chip `MUTE`, hint. Track cells (4+29t, 99) 27x20 in track colour (muted ones are hollow `MUTE`). Solo cells at y 121, 27x11: key at +3, `SO` at +13, WHITE when soloed.
* **ROLL** (ALT): y 95..134. Chip `ROLL` + rate AMBER. 8 track pads at y 111, 27x20. A rolling pad is WHITE.
* **TEMPO** (TAB): box (44,26) 152x84 PANEL with a WHITE frame. `TEMPO` DIM, BPM in XL at (74,46), `BPM` DIM, 4 beat squares at the top right (the current beat WHITE), a SWING value and bar, then the hints.
* **Value pop-up**: (48,34) 144x33 RAISED with a 1 px frame in the owner colour. Long name TEXT at (+6,+5), owner sub-label at (+6,+15), value L WHITE right-aligned, and a 132x4 bar at y+25 (bipolar from the centre with a WHITE centre tick).
* **Message toast**: centred, y 44..60, RAISED. The frame is LINE (info), AMBER (undoable) or RED (error).
* **Boot banner**: (30,30) 180x44 PANEL with a GREEN frame. `POCKET GROOVE` in L, then `SPACE PLAY  / ALL KEYS`, `1-8 Q-I STEPS  A-K PADS`.

### 10.10 HELP (`help.png`, `help-explain.png`)
* Full screen BG. A header bar 0..10 WHITE with `KEYS <VIEW>` in BG and `FN/SFT/CTL/OPT/ALT/TAB` LINE on the right.
* A key grid of 4x14 cells 16x17 at (1+17c, 14+19r), each with a 2-letter label (same colour groups as the keys-*.png diagrams).
* A legend from y 92 at a 9 px pitch, with 5 lines.
* Holding a modifier while the help is up redraws the grid for that layer.
* **Explain mode**: the same grid, plus the last pressed key framed WHITE and a card (2,93) 236x40 PANEL with an AMBER frame: the key chip + title WHITE, then 2 lines TEXT. Texts come from a per-mode table of 56 entries (one line title ≤ 30 chars, two lines ≤ 38 chars each).

---

## 11. Feedback and timing

| Item | Timing |
|---|---|
| Frame rate | Redraw at 30 fps, the full framebuffer, pushed in one transfer. Only redraw when something changed (status, revision, UI state, a timer). Two smaller pushes: the status bar alone (rows 0..10), and on the instrument page the picture's band alone (rows 29..72, 10.3). |
| Hold threshold (step, TAB) | 250 ms. Modifier overlay delay 200 ms. `/` tap vs hold 400 ms. |
| Value repeat | 300 ms delay, then every 40 ms, step 1→2→4 at 0.5 s intervals (5.2). |
| Sweep (`[ ]`) | 48 units/s, sent every 40 ms. A 150 ms detent at 64. |
| Value pop-up | Shows on change and hides 1200 ms after the last change. While a step is held in SOUND/MIX it stays up. |
| Message toast | 1500 ms (info), 2500 ms (undoable action, AMBER), 3000 ms (error, RED). A new toast replaces the old one. |
| Trigger lamps / beat dot | 80 ms after the change. |
| Queued pattern blink | 2 Hz (250 ms on/off). |
| REC blink in step-write | 1 Hz. |
| Cursor (step-write, name entry) | Steady (no blink: blinking at this size reads as flicker). |
| Clip indicator | 1000 ms hold. |
| Battery blink below 10 % | Once every 2 s. |
| Boot splash | Up to 800 ms, then GROOVE + banner until the first key press. |

**Per-track colours:** KICK red, SNAR orange, CLAP yellow, CHH lime, OHH mint, PERC sky, BASS violet, LEAD pink. Each comes in 4 shades (bright, mid, dim, tint).

---

## 12. Palette and fonts

8-bit indexed framebuffer (`pg::Framebuffer`). Indexes 0..47 are used. Fill 48..255 with black. `python3 tools/ux_mock/render.py --palette` regenerates this table.

| idx | name | RGB888 | RGB565 |
|----:|------|--------|--------|
| 0 | BG | #08090C | 0x0841 |
| 1 | PANEL | #15181F | 0x10C3 |
| 2 | RAISED | #232731 | 0x2126 |
| 3 | LINE | #3A4150 | 0x3A0A |
| 4 | DIM | #7A8396 | 0x7C12 |
| 5 | TEXT | #CFD6E2 | 0xCEBC |
| 6 | WHITE | #FFFFFF | 0xFFFF |
| 7 | AMBER (locks, queue, warnings) | #FFB400 | 0xFDA0 |
| 8 | RED (rec, errors) | #FF3848 | 0xF9C9 |
| 9 | GREEN (play, ROM) | #40E070 | 0x470E |
| 10 | CYAN (FX / globals, SD) | #30D0FF | 0x369F |
| 11 | AMBER_D | #5C4000 | 0x5A00 |
| 12 | RED_D | #60141C | 0x60A3 |
| 13 | GREEN_D | #145028 | 0x1285 |
| 14 | CYAN_D | #104C60 | 0x126C |
| 15 | BLACK | #000000 | 0x0000 |
| 16+4t+0 | track bright | KICK #FF503C, SNAR #FF8C20, CLAP #FFD438, CHH #98E430, OHH #30E4A8, PERC #38B4FF, BASS #9480FF, LEAD #FF60C8 | |
| 16+4t+1 | track mid = mix(bright, BG, 0.62) | e.g. BASS #5F53A3 | |
| 16+4t+2 | track dim = mix(…, 0.36) | e.g. BASS #3A3463 | |
| 16+4t+3 | track tint = mix(…, 0.17) | e.g. BASS #201D35 | |

The full 48-row table with RGB565 values is printed by `--palette`. These are the exact values the mockups use.

**Fonts.**
* The source glyph data is `tools/ux_mock/pgfont.py`, which is good enough to embed as is. **FONT_S** is the classic 5x7 ASCII set (0x20..0x7E) in a 6x8 cell, stored as 5 column bytes with bit 0 at the top and bit 7 as the descender row. That's 95x5 = 475 bytes.
* **FONT_L** and **FONT_XL** are FONT_S at 2x and 3x nearest-neighbour. No separate tables are needed.
* The icon glyphs are at codes 0x80..0x90 (play, stop, rec, ring, up, down, left, right, lock, slide, note, chain, check, dot, block, duck, tri_r), each 5x7, stored the same way.
* Minimum: FONT_S, cap height 7 px. Nothing smaller is ever used.
* All on-screen text is upper case. ParamInfo `longName` is upper-cased at draw time.
* Specimen: `docs/ux/font.png`.

---

## 13. Destructive actions, undo, files

### 13.1 Clipboard and undo
| Action | Keys | Commands | Undo snapshot |
|---|---|---|---|
| Copy track | CTRL+C | none (UI copies `TrackPattern` from the Project into `clip`) | no |
| Cut track | CTRL+X | copy + `ClearTrack` | yes |
| Copy pattern | CTRL+SHIFT+C | UI copies the `Pattern` | no |
| Cut pattern | CTRL+SHIFT+X | copy + `ClearPattern` | yes |
| Paste | CTRL+V | track clip → selected track; drum↔drum or synth↔synth only (else the RED toast "CAN'T PASTE DRUMS ON BASS"); pattern clip → playing pattern | yes |
| Copy pattern to n | OPT+SHIFT+step | `CopyPattern` | yes (of n) |
| Length change / add page | SHIFT+step, SHIFT+' | `SetPatternLength` / `SetTrackLength` (+ SetSteps) | yes |
| Erase (DEL hold) | | `SetStep`… | yes (at first erase) |
| Clear one step | tap | `SetStep` | no |

* **Undo** keeps one snapshot: the full `Pattern` (4168 B) plus its index, taken *before* the action. CTRL+Z swaps the snapshot with the current contents of that pattern (so pressing it again redoes). The toast reads "UNDO: BASS CLEARED" / "REDO". Any new destructive action replaces the snapshot.
* Writing a whole track or pattern back uses CCR-3. The fallback is trickled `SetStep` commands: at most 48 per UI frame. Each frame checks the queue with `post()` and retries what it couldn't send. A pattern restore (512 steps) then takes 11 frames, about 0.35 s. During that time the UI shows the toast and ignores further edits to that pattern.
* **Instrument changes** are undoable too, without a pattern snapshot: the snapshot is the track's instrument id, its four macro positions and its 24 sound parameters (so CLASSIC comes back exactly). A run of instrument steps while the list is up is one unit. Any pattern snapshot replaces it, and it replaces a pattern snapshot (one level). Toast "UNDO: BASS ACID" / "UNDO: BASS CLASSIC".
* A **recording pass** is one unit: the first recorded note or the first macro move recorded into the notes (motion, 10.3) snapshots the playing pattern; everything recorded until REC goes off or the transport stops undoes together. **Clear motion** (hold `del` + a macro key) is a track write with its own snapshot.
* Song slots, mutes, macro moves and sound/mixer/global params aren't covered by undo. They aren't destructive in the same way, because they're always visibly reversible.

### 13.2 Files
* Paths: `/int/pg/<NAME>.pgv` and `/sd/pg/<NAME>.pgv`, plus `/int/pg/_last.pgv` (the session autosave) and `/int/pg/_ui.cfg` (the UI settings blob written by `io::saveUiSettings`: bass and lead octave, chord mode, roll rate, mount and name of the open project, its dirty flag).
* The file format is owned by `lib/pg/src/pg/io/project_io.h`: a 64-byte CRC-checked header (file magic, format versions, tempo, swing, scale, the file stem) followed by the compressed `Project`. The UI never parses files itself; it calls `io::saveProject` / `io::loadProject` / `io::listProjects` / `io::deleteProject` and turns every `io::Status` into a RED toast ("NOT A POCKET GROOVE FILE", "FILE IS DAMAGED", "NEEDS NEWER FIRMWARE", "NOT ENOUGH SPACE", ...). A failed load leaves the project untouched, except `DestClobbered`, after which the UI starts a new project ("LOAD FAILED - NEW PROJECT"). Every load is followed by `Engine::sanitizeProject`. The `name` field mirrors the file name.
* The DEMO is compiled into the firmware (ROM) and listed first. It can be loaded but not overwritten or deleted. CTRL+S on DEMO opens SAVE AS.
* Names are 1..8 characters from A-Z, 0-9, `-` and `_`. Typed letters are upper-cased. The default suggestion is `GROOVE01`, using the next free number. An existing name asks "OVERWRITE ACIDJAM?".
* **Save** (CTRL+S, or ENTER on SAVE AS): if the project has a name and mount, it saves straight away with the toast "SAVED ACIDJAM". Otherwise it opens the name dialog. Saving reads the Project memory directly. The 1-byte race with engine edits is acceptable.
  * **Saving to /int while playing:** if the hardware test (16.4) shows audio dropouts during flash writes, the write is deferred until the transport stops, with the toast "SAVES WHEN STOPPED". SD writes are never deferred.
* **Load**: if `dirty`, confirm first ("LOAD BASSHAUS? / ACIDJAM HAS UNSAVED CHANGES / ENTER DISCARD AND LOAD / ESC CANCEL"). Then `Stop`, `suspend()`, read into the Project, `resume()`, reset the UI state (selTrack BASS, page 0, undo/clip cleared), and go to GROOVE stopped.
* **New** (CTRL+N): confirm if dirty. Then `suspend()`, `Engine::initProject(project)`, name `""`, the first instrument of every track at its default positions (`macros/builtin.txt`), `sanitizeProject` (derives their sound), `resume()`. The ROM demos stay CLASSIC.
* **Delete**: DEL on a row asks "DELETE WAREHOUS?" (ENTER / ESC). It can't be undone.
* **Autosave** (session, `io::saveSession`): never while playing (a flash write stalls the audio task). Written 2 s after the transport stops, or when stopped with no key pressed for 10 s, and only if the project differs from the last session written (CRC). The firmware also calls `Ui::saveSessionNow()` before power-off. Also on a battery warning below 5 %. Autosave doesn't clear `dirty`, because that tracks the named file.
* SD missing: the SD rows are absent, the header shows `NO SD`, and SAVE TO SD gives the RED toast "NO SD CARD". If the card is pulled mid-write, the toast "SAVE FAILED" appears and `dirty` stays.

### 13.3 Read-only BPM column
The BPM column is the tempo field of each file's header, read by `io::listProjects` (headers only, streamed through `FileSystem::open/readSome`). A file whose header can't be read is listed with `BAD` instead of a BPM; ENTER on it shows the reason. ROM demos show the tempo from `demo::tempo()`.

---

## 14. Power-on and the demo

1. The splash is the boot banner over a blank GROOVE, up to 800 ms, while the engine starts.
2. The device loads `/int/pg/_last.pgv` if it's valid, otherwise the ROM demo ACIDJAM. It is **stopped**, the selected track is BASS, the focus is the bass's **macro 1** (in the demo ACID's **OPEN**; on a CLASSIC bass from an old session its CUT, as before instruments), and the banner shows `SPACE PLAY` (`first-boot.png`). Any key dismisses the banner, and that key also performs its normal action. So the very first SPACE both dismisses and plays.
   *Why BASS and OPEN.* The acid line is the demo's lead voice and the 303 filter is the most obvious thing to grab: `-`/`=` (and SHIFT+`[` `]`) move OPEN and the squelch answers at once. Pattern 1 gives OPEN no motion, so the player's hand is always heard; it moves **BITE** by itself instead (each bar the notes growl a little harder), so the first visit to SOUND (`9`) shows the instrument page alive: OPEN where the player left it, BITE's slider gliding with the notes, its amber ◆ saying the notes carry it (`first-sound.png`). Focusing the moving macro instead would have made the first `=` silent (a move of a macro with motion only sets its base).
3. **The factory demos** (stage G, docs/STATE.md): ACIDJAM, BASSHAUS and WAREHOUS (`lib/pg/src/pg/demo/`, docs/MANUAL.md 14) play a built-in instrument on every track, no CLASSIC track, with macro positions chosen by measurement so each keeps the sound and balance of its CLASSIC original (`host/tools/demo_compare`). The CLASSIC demos' per-step parameter locks became the notes' own macro values (motion): ACIDJAM's acid line moves OPEN bar by bar in the arrangement patterns and BITE in the jam patterns (P1-P3), BASSHAUS's growl riff gives every note its own DIRT and MOVE, WAREHOUS's rolling bass moves TONE or TOUCH beat by beat and its dub stab PUSH stab by stab. In the jam patterns (P1-P3, P8) the bass's and lead's macro 1 never carries motion, so its keys and the bass sweep stay the player's (`itest_demo` checks both rules).
   ACIDJAM at power-on: A minor, 124 BPM, swing 54 %, ducker source KICK pulling the bass and stabs; P1 GROOVE: 909 kit (TRIO at 909), off-beat open hat, rim (TRAIL), the ACID line with BITE swelling through each bar, Am9 / Dm9 stabs on HANDS through a dotted-eighth delay. Song mode is OFF at boot; the SONG view holds the chain.
   (The mockups' "DEMO" pattern, `screens.demo_pattern()`, is not a ROM demo: it is the project the screenshot scripts and UI tests start from, `buildMockup` in `host/common/ui_harness.h`.)

---

## 15. The first minute (user who has read nothing)

| t | User does | Instrument does |
|---|---|---|
| 0 s | Switches on | Banner: "SPACE PLAY". The grid is visibly full of coloured steps (`first-boot.png`). |
| 3 s | SPACE | P01 plays: kick, hats, the acid bass pumping against the kick and growling a little harder through each bar by itself (`first-play.png`). The status duck bar flickers cyan on every beat. |
| 10 s | Mashes keys; hits `]` | Hats get thin as the DJ high-pass opens; the pop-up shows "DJ FILTER HP30". They press `[` and it sweeps back through OFF into a low-pass. |
| 18 s | Presses `=` a few times, then holds it | The chip says BASS OPEN. The acid line opens up and squelches. |
| 22 s | Taps `9` | SOUND: the bass's instrument ACID, its picture, OPEN where they left it and BITE's slider moving by itself with the notes (`first-sound.png`). They hold `x` and `z`: OPEN opens and closes; `` ` `` goes home. |
| 25 s | Taps `a` | Kick plays and gets selected: the grid now shows the 4 kick steps. They tap `7` and a kick appears on step 7. |
| 35 s | Taps `j`, plays `z`..`,` | Bass notes, always in key. They hold `z` and press `c`: a slide. |
| 45 s | Holds `/` | Key map of GROOVE. They see "PT+STEP PATTERN". |
| 50 s | OPT+2 | `P01>02` blinks. At the bar line P02 starts, which is a bigger groove. |
| 60 s | ENTER, then pads | REC is on: their finger-drummed claps land on the grid. CTRL+Z if they hate it. |

---

## 16. Edge cases

1. **Ghosting / 3 keys.** The matrix has no diodes (confirmed on the device): three closed keys on three corners of a rectangle of the TCA8418's 7x8 scan matrix make it report the fourth corner too, in the same batch as the third key, in either order. Every key event goes through a phantom filter (`ui/ghost.h`) before the UI sees it. A down whose scan row and column are already connected through closed keys is a phantom and is dropped. A down that would close a loop is held back until its partner corner arrives (same batch, within 3 ms); the two are electrically interchangeable, so the one that, pressed last, completes a known three-key gesture is kept, and if both or neither do, both are dropped. A dropped key's key-up is swallowed; any key-up first releases held-back downs; no release order leaves a key stuck. A held-back down with no partner (no phantom came) is delivered by the next frame. Three keys that close no rectangle all act. docs/ghosting-audit.md has every three-key gesture, its phantom and the outcome, and the one remap it required (STEP EDIT octave: CTRL+`;`/`.` instead of SHIFT).
2. **Key down in one view, up in another.** Every key-up is delivered to whatever owns the key-down: a held note always gets its `NoteOff`, a held step always closes, and a roll always stops. On view change, all sounding UI-initiated notes are kept; releasing the key ends them.
3. **Stop while notes are held.** The engine releases sequenced notes. Live keys still send NoteOff on release.
4. **Pattern switch while a step is held.** The STEP EDIT panel closes (toast "PATTERN CHANGED"). The edit target follows the playing pattern, and edits are never sent to a pattern you can't see.
5. **Track length < shown page.** The overview draws that track's cells beyond its length as empty. Its playhead is drawn only when its current step falls inside the shown page.
6. **Lock on a param that the voice table no longer has** (after firmware update): shown as `P23` with the raw value. DEL removes it.
7. **Queue full** (`post()` false): retry next frame. Value repeats coalesce: only the latest value per parameter is kept pending.
8. **Battery dying.** At 5 % the device autosaves `_last.pgv` once, with the toast "BATTERY LOW - SAVED".
9. **Song mode with an empty chain.** ENTER in SONG gives the toast "ADD A PATTERN FIRST", and chain stays off.
10. **Removing the slot that's playing.** Allowed. The engine moves on at the next boundary.
11. **Paste across lengths.** A track paste also sets that track's length from the clip. A pattern paste sets all lengths.
12. **Mute + solo + pattern change.** Mutes are per track (Project), so they persist across patterns.
13. **Explain mode while playing.** The transport keeps going. Only SPACE and `` ` `` keep their function inside explain mode; everything else is explained, not executed.
14. **ParamInfo with range > 127 or < 3 values.** Same rules. Enums never wrap.
15. **Help or a dialog open when REC is on.** Recording continues. Keys don't reach the engine while a dialog is open, except SPACE.

---

## 17. Contract change requests

None of these are assumed. Each has a stated v1 fallback.

| # | Header | Change | Why | Fallback |
|---|---|---|---|---|
| CCR-1 | engine.h | `CmdType::Roll`: a = track, b = rate (0 = 1/8, 1 = 1/16, 2 = 1/32, 3 = 1/16T), c = 1 start / 0 stop; velocity in `step.vel`. Engine retriggers on the grid (swing ignored) until stop. | Roll must be sample-tight and grid-locked; the UI loop can't be. | UI-timed `NoteOn`s; if jitter > 2 ms on hardware, ship without roll. |
| CCR-2 | engine.h (NoteOn record semantics) | When recording, a live NoteOn with `kStepSlide` should set the slide flag on the **previously recorded step** of that track. `Step::kStepSlide` means "glide into the next step", but `recordNote()` sets it on the new step. Also, a `NoteOff` while recording should set that step's `gate` to the held length (quarter-steps, min 1). | Recorded acid slides currently land one step late. Recorded note lengths are all 2. | UI keeps live slide only (not recorded), and gates stay at the default. |
| CCR-3 | engine.h | Bulk write: `Engine::scratchPattern()` returns a UI-writable `Pattern&`. `CmdType::WriteTrack` (a = pattern, b = track, c = source track in scratch) and `CmdType::WritePattern` (a = pattern) copy from scratch into the Project. The UI won't touch scratch until the command's revision bump. | Cross-pattern paste and undo are 64..512 SetSteps. | Trickled `SetStep` (13.1). |
| CCR-4 | engine.h `EngineStatus` | Add `uint8_t masterStep` (position within pattern length) and `uint8_t songRepeat` (repeat index of the current slot). | Queue countdown and "x2 of 4" display. Today the UI derives the master position from track steps, which is wrong with polymeter. | Use `step[0]` with the kick length; the song display shows repeats without a count. |
| CCR-5 | engine.h | `SetSongMode` with `b` = start slot. | "Play song from cursor" in SONG. | v1 always starts at slot 1. |
| CCR-6 | platform.h | `struct SystemInfo { int8_t batteryPct; /* -1 unknown */ bool sdPresent; };` and a function that fills it. Also a monotonic `uint32_t millis()` (or the UI update entry takes `nowMs`). | Battery icon; every timing in section 11. | The battery icon is hidden; the UI entry point is defined by the ui/ owner with `nowMs`. |
| CCR-7 | platform.h | `FileSystem::read` must document that `cap < file size` returns the first `cap` bytes and true. Optionally add `freeBytes(mount)`. | BPM column in PROJECT; "disk full" message. | Drop the BPM column; generic "SAVE FAILED". |
| CCR-8 | lead.cpp (voice owner, not a header) | Move `WID` (stereo width) off page 1, e.g. swap with `DRV` (index 15). | Output is mono, so page 1 should be all audible tweaks. | Leave as is. |
| CCR-9 | params.h | `kGSwing` is `Percent` over 0..50 but means 50..75 %. `kGMasterFilter` should display as LP/OFF/HP. Add `ParamFormat::Swing` and `ParamFormat::DjFilter`, or accept that the UI special-cases these two ids. | The generic UI would show the wrong text. | The UI special-cases them (as the mockups do). |
| CCR-10 | engine.h | A command that sets mute at the next bar: `SetMute` with `b = 1`. | Quantised mutes (later). | Not in v1. |

---

## 18. Scope

**v1:**
* All 6 views and the overlays listed above.
* Step programming up to 64 steps, pages, pattern and track length.
* STEP EDIT with all fields, one p-lock per step from SOUND/MIX.
* The scale-locked note row, octave, legato slide, accent.
* Lead AUTO/AUTO7/fixed chords.
* Step-write and live record.
* Mutes, solo, pattern queue, copy-and-queue, DJ filter and bass-cutoff sweeps, erase, roll (with CCR-1).
* Tempo, tap and swing.
* SONG chain editing.
* Load, save, save-as, delete, new, autosave, single-level undo, clipboard.
* Help map and explain mode.
* The ROM demo.

**Later:**
* Quantised mutes and fills.
* Parameter-motion recording.
* Per-step micro-timing.
* A chromatic piano layout.
* Arpeggiator.
* Multiple undo levels.
* Pattern names.
* Scenes / performance macros.
* Following the playhead page.
* Euclidean fill generator ("fill track with N hits").
* Randomise (velocity / notes in scale).
* MIDI sync, WAV export.
* Brightness setting.

---

## 19. Open questions for the end user

1. **Pads play a sound when you select a track.** That's great for finger drumming, but surprising mid-set. SHIFT+pad selects silently. Should the default be the other way round?
2. **Resume vs demo at power-on.** The current choice is to resume the last session. Do you prefer to always start on the demo?
3. **SHIFT+`[ ]` is the bass cutoff.** Would you rather have another bass parameter there, or let it follow the BASS focus (e.g. FOLD or DRV for bass-house growl)?
4. **Drum note row = velocity** (8 levels). The alternative is pitch (tuned toms and percussion melodies).
5. **Is A minor at 124 BPM the right demo flavour,** or should it lean bass-house (125-128 BPM, F minor, FM/fold bass)?

---

## 20. Mockup tool

`python3 tools/ux_mock/render.py` (needs `pip install pillow`) writes every PNG into `docs/ux/`:

* Screens at 4x:
  * `boot`
  * `groove-drum`, `groove-bass`, `groove-lead`, `groove-live`, `groove-page2`, `groove-stepwrite`
  * `step-edit`, `step-edit-drum`, `step-edit-lead`
  * `toast-value`, `toast-undo`
  * `sound`, `sound-kick`, `sound-lock`
  * `mix`, `mix-duck`, `mix-pan`
  * `fx-duck`, `fx-duck-track`, `fx-space`, `fx-master`
  * `pattern-overlay`, `mute-overlay`, `roll-overlay`, `tempo-overlay`
  * `song`, `project-load`, `project-save`, `confirm`
  * `help`, `help-explain`
  * `font`
* Key maps at 2x: `keys-groove`, `keys-step-held`, `keys-step-write`, `keys-sound`, `keys-mix`, `keys-fx`, `keys-song`, `keys-project`, `keys-text`, `keys-modifiers`.
* The contact sheet: `overview.png`.

The modules are `pgfont.py` (glyphs), `pgfb.py` (palette and primitives), `pgparams.py` (a copy of the real ParamInfo tables and the value-display rules), `screens.py` (every screen; function names match the sections above) and `keymaps.py` (the key tables of section 6).

### Value display rules (implemented in `pgparams.fmt`)
* Number / Bpm: raw value.
* Percent: `round((v-min)*100/(max-min))%`.
* Bipolar: `v - (min+max+1)/2`, shown as `0` / `+n` / `-n`.
* Semitones: `v - (min+max)/2`, signed.
* Enum: the name.
* OnOff: ON/OFF.
* Special cases: SWNG is shown as `50 + v/2 %`, and FILT is shown as `OFF` / `LPn` / `HPn`. MIX PAN at 64 shows `C`.
* Max 5 characters.

Bar kinds:
* enum, or a range of ≤ 9 values: segments;
* Bipolar / Semitones: from the centre;
* otherwise: from the bottom.
