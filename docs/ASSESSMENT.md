# Pocket Groove: assessment sheet

A working sheet for one pass through every sound and every setting on the desktop build. Fill it in while you play; it is the raw material for the redesign, where each voice gets a few designed macro controls instead of these bare parameters. What matters most: where in its range each parameter sounds alive, where it is dull, broken or ugly, which do nothing you can hear, which only make sense together, and what you wished for that no parameter does.

## How to do this pass

**Start.** From the repository root: `tools/pg-desktop` (headphones on, Mac volume low at first). Quit with Cmd+Q; the session is saved and comes back next time. `tools/pg-desktop --mono` plays what the device's headphone jack plays.

**Mac keys** (docs/DESKTOP.md). Letters, digits and punctuation are the Cardputer's keys; the modifiers are:

| Cardputer | Mac | | Cardputer | Mac |
|---|---|---|---|---|
| `shift` | left Shift | | `alt` | right Option |
| `fn` | right Shift | | `del` | Delete (backspace) |
| `ctrl` | Control | | `enter` | Return |
| `opt` | left Option | | `` ` `` (home) | the key left of `1` |

The strip under the screen lights each modifier while it is held. Hold `/` for the key map of the view you are in.

**Reach a sound and change a value.** Press the track's pad (`a` KICK, `s` SNAR, `d` CLAP, `f` CHH, `g` OHH, `h` PERC, `j` BASS, `k` LEAD), then `9` for SOUND. The note keys `z` `x` `c` `v` `b` `n` `m` `,` pick a column; `9` again or `.` is the next page, `;` the previous. `-` / `=` change the value: tap = one step, hold = sweep, left Shift = 8 at a time, `-` and `=` together = back to the default. In the tables, Key `2 x` means SOUND page 2, key `x`; a number in brackets after a default is the raw 0-127 value behind what the screen shows. `0` is MIX (`z` LEVEL, `x` DUCK, `c` DELAY, `v` REVERB, `b` PAN rows; a pad picks the track), `o` is FX (`o` again cycles the DUCK, SPACE, MASTER tabs; note keys pick a column). Pages, rows and tabs are remembered when you come back. `` ` `` always goes home. `space` plays and stops. In SOUND, a pad also plays the track, so you can audition while stopped.

**Solo.** Hold right Shift (`fn`) and press a note key: `z` KICK, `x` SNAR, `c` CLAP, `v` CHH, `b` OHH, `n` PERC, `m` BASS, `,` LEAD. The same key again brings the mix back. `fn`+pad mutes one track. A soloed bass does not pump: the ducker follows the kick you hear (set DSRC to `BEAT` to keep it).

**Before you start: build TESTBED** (two minutes; the demos lock the bass cutoff on most steps, so CUT does nothing in them).

1. Control+N (Return if it asks). A new project: C minor, 124 BPM, default sounds, 16 steps.
2. Kick: left Shift+`a`, tap `1` `5` `q` `t`. Clap: left Shift+`d`, tap `5` `t`. Closed hat: left Shift+`f`, tap `3` `7` `e` `u`.
3. Bass: left Shift+`j`, tap `3` `4` `7` `e` `u`; hold `4` and press `,` (an octave up), hold `e` and press `v`.
4. Lead: left Shift+`k`, tap `4` `7` `e` `y` (chord stabs).
5. Save it (below) as `TESTBED`.

Each section names what to load: TESTBED, or a factory demo (ACIDJAM, BASSHAUS, WAREHOUS, in PROJECT under ROM). Demo patterns: hold left Option (`opt`) and tap a step key (`4` = P4); stopped, it switches at once.

**Load.** `\` opens PROJECT (LOAD tab); `;` `.` move; Return loads. With unsaved changes it asks: Return discards them, `` ` `` cancels.

**A/B.** Sound and mixer values are not undoable (Control+Z only undoes pattern edits: notes, step edits, recording, lengths, cut and paste). To compare: note the number, move, come back; or `-` and `=` together for the default; or reload the project you started from (Load, above).

**Save a snapshot** of anything worth keeping: `\`, `\` again (SAVE AS tab), cursor on `+ NEW NAME...` (top row), Return, Delete to erase the suggested name, type up to 8 characters (A-Z, 0-9, `-`; space types `_`), Return. Write the name in the note. Files land in `projects/NAME.pgv`. After SAVE AS, Control+S saves over that name, so use SAVE AS again for the next snapshot.

**If words fail, record.** Cmd+R starts and stops a recording of what you hear (`bounces/live/YYYYMMDD-HHMMSS.wav`); write the time in the note.

**Answer lines.** One line per parameter, under each table. Write the values the screen shows.

```text
NAME  alive: 40-90  dull: 0-20  ugly: 120-127  dead: x  with: RES  note: anything
```

`alive` = where it sounds good and useful, `dull` = boring or no different, `ugly` = broken, harsh or unusable, `dead` = put `x` if you could hear it do nothing, `with` = a parameter it only makes sense with. Leave a field blank if you have nothing. A note can continue on the next line if that line starts with spaces. About 20-30 seconds per parameter is enough; first impressions are what we need.

**Desktop and device.** The desktop is stereo; the device is mono, so PAN, the lead's WID and the delay's DWID are heard only here. The desktop accepts every key combination; the device ignores some three-key combinations (`--device-keys` models that). The device's G0 volume is the Mac's volume here. macOS may take Control+number, Control+arrow and Control+Space for itself (System Settings > Keyboard > Keyboard Shortcuts).

---

## 1. KICK (pad `a`)

Load TESTBED. Solo: right Shift+`z`.

<!-- BEGIN ASSESS KICK -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `TUNE` Tune | 0 .. 127 | 60 | Pitch of the boom (28-101 Hz; 66 is A). | With the bass playing, find where kick and bass stop fighting. |
| 1 `x` | `DCY` Decay | 0 .. 127 | 46 | Length of the boom. | Kick soloed, hold `=` from 46 to 127: thump to 808 tail. |
| 1 `c` | `PNCH` Punch | 0 .. 127 | 76 | How far the pitch drops at the hit. | Set SWP to 100 first so the drop is slow, then sweep PNCH. |
| 1 `v` | `SWP` Sweep Time | 0 .. 127 | 58 | How fast that pitch drop settles. | PNCH to 110 first, then sweep SWP: click-thump to laser "dooo". |
| 1 `b` | `CLK` Click | 0 .. 127 | 72 | Level of the click on top. | DCY to 10 first so only the attack is left, then sweep CLK. |
| 1 `n` | `DRV` Drive | 0 .. 127 | 28 | Saturation: denser, louder, harder. | Sweep 0 to 127 with the bass on: where does it start to eat the bass? |
| 1 `m` | `TONE` Tone | 0 .. 127 | 100 | Low-pass over the whole kick: round to bright. | CLK to 127 first, then sweep TONE down from 127. |
| 1 `,` | `HOLD` Hold | 0 .. 127 | 30 | Keeps the boom at full level before it decays. | DCY to 20 first, then HOLD 0 against 127. |
| 2 `z` | `HARM` Harmonics | 0 .. 127 | 36 | Adds a second harmonic: presence on small speakers. | Compare 0 and 127 on the laptop speakers, then on headphones. |
| 2 `x` | `CTON` Click Tone | 0 .. 127 | 56 | Character of the click: tick to thwack. | CLK to 127 first, then sweep CTON. |

```text
TUNE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
PNCH  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SWP   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
CLK   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TONE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
HOLD  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
HARM  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
CTON  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS KICK -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 2. BASS (pad `j`)

TESTBED for page 1 (the filter). ACIDJAM P1 to ride RES and ENV on a real acid line; BASSHAUS P1 for page 2 (FM, FOLD, LFO): its notes lock FOLD, FM or FMIX one by one. Solo: right Shift+`m`. Left Shift+`[` / `]` sweeps CUT from any view; left Shift+both returns it.

<!-- BEGIN ASSESS BASS -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `CUT` Cutoff | 0 .. 127 | 46 | Filter cutoff: dark to bright. Also left Shift+`[` `]` from any view. | In TESTBED only: the demos lock CUT on most bass steps, so it does nothing there. |
| 1 `x` | `RES` Resonance | 0 .. 127 | 78 | Resonance: squelch, then self-oscillation above about 123. | CUT 30, ENV 100 first, then sweep RES to 127. |
| 1 `c` | `ENV` Env Mod | 0 .. 127 | 70 | How far each note opens the filter (the "wow"). | CUT 20, RES 100 first, then sweep. |
| 1 `v` | `DCY` Decay | 0 .. 127 | 44 | How long that "wow" lasts: blip to vowel. | ENV to 100 first, then sweep. |
| 1 `b` | `ACC` Accent | 0 .. 127 | 90 | How much accented steps jump out. | Accent two steps (hold step, `g`, `=`), then ACC 0 against 127. |
| 1 `n` | `DRV` Drive | 0 .. 127 | 30 | Grit and growl, into and after the filter. | RES to 100, then sweep 0 to 127. |
| 1 `m` | `LFO` LFO Depth | 0 .. 127 | 0 | Tempo-synced wobble of the cutoff (it only opens). | CUT 20 and ADCY 127 first, long gates, then raise LFO. |
| 1 `,` | `RATE` LFO Rate | `2BAR` `1BAR` `1/2` `1/4.` `1/4` `1/8.` `1/4T` `1/8` `1/8T` `1/16` `1/16T` `1/32` | 1/8 (7) | Speed of the wobble as a note length. | LFO to 80 first, then try `1BAR`, `1/4`, `1/8T`, `1/16`. |
| 2 `z` | `WAVE` Waveform | 0 .. 127 | 0 | Oscillator shape: saw, square, thin pulse. | CUT 100, ENV 0 first, then sweep. |
| 2 `x` | `SUB` Sub Level | 0 .. 127 | 56 | Clean sine under the bass. | Bass soloed, CUT 0: SUB 0 against 127, on headphones. |
| 2 `c` | `FMIX` FM Osc Mix | 0 .. 127 | 0 | Crossfade from saw/pulse to the FM pair (growl, metal). | FM to 100 first, then sweep FMIX 0 to 127. |
| 2 `v` | `FM` FM Depth | 0 .. 127 | 64 | Brightness of the FM pair. | FMIX to 127 first (at 0 FM does nothing), then sweep. |
| 2 `b` | `RATI` FM Ratio | `0.5` `1` `1.5` `2` `3` `3.5` `4` `5` `7` | 2 (3) | FM pitch ratio: warm, hollow, metallic. | FMIX 127, FM 90, then step through all nine. |
| 2 `n` | `FOLD` Fold | 0 .. 127 | 0 | Wavefolder: buzzy bass-house growl. | CUT to 80, then sweep; then hear the locks in BASSHAUS P1. |
| 2 `m` | `TENV` Timbre Env | 0 .. 127 | 90 | Lets the filter envelope also move FM and fold, per note. | FOLD 80 (or FMIX 127) and ENV 100 first, then TENV 0 against 127. |
| 2 `,` | `LSHP` LFO Shape | `TRI` `SINE` `DOWN` `UP` `SQR` `S&H` | TRI (0) | Shape of the wobble. | LFO 80, RATE `1/4` first, then step through; S&H is random. |
| 3 `z` | `SLID` Slide Time | 0 .. 127 | 65 | Glide time between slid notes. | SLD on a step before a different note (hold step, `h`, `=`), then sweep. |
| 3 `x` | `ADCY` Amp Decay | 0 .. 127 | 96 | How long a held note sounds: pluck to sustain. | Long gates first (hold step, `s`, hold `=` to 4), then sweep. |
| 3 `c` | `REL` Release | 0 .. 127 | 32 | Tail after the note ends. | ADCY 127, then sweep REL. |
| 3 `v` | `ATK` Attack | 0 .. 127 | 18 | Soft start of each note. | Sweep 0 to 127 and listen to the front of the notes. |
| 3 `b` | `KTRK` Key Track | 0 .. 127 | 40 | How much the cutoff follows the note. | CUT 40, notes `z` and `,` an octave apart: KTRK 0 against 127. |
| 3 `n` | `LTMB` LFO>Timbre | 0 .. 127 | 0 | The wobble also moves FM and fold. | LFO 80, FOLD 80 first, then LTMB 0 against 127. |
| 3 `m` | `SUBO` Sub Octave | `UNI` `-1OCT` | UNI (0) | Sub on the note or an octave below. | SUB 127, CUT 0, then flip. |
| 3 `,` | `TUNE` Osc Tune | -24 .. +24 | 0 (24) | Transposes the main oscillator; the sub stays. | SUB 60, then try +7, +12, -12. |

```text
CUT   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RES   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ENV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ACC   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
LFO   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RATE  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
WAVE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SUB   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FMIX  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FM    alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RATI  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
FOLD  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TENV  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
LSHP  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
SLID  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ADCY  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
REL   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ATK   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
KTRK  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
LTMB  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SUBO  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
TUNE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS BASS -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 3. SNARE (pad `s`)

TESTBED, then left Shift+`s` and tap `5` `t`. BASSHAUS layers a tight snare under its clap. Solo: right Shift+`x`.

<!-- BEGIN ASSESS SNAR -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `TUNE` Tune | 0 .. 127 | 60 | Pitch of the drum body. | SNAP to 20 first so the body is in front, then sweep. |
| 1 `x` | `DCY` Decay | 0 .. 127 | 56 | Length of the snare wires (the noise). | SNAP to 127, then sweep DCY. |
| 1 `c` | `SNAP` Snappy | 0 .. 127 | 80 | Wires against body: tom-like to crack. | Sweep 0 to 127. |
| 1 `v` | `TONE` Tone | 0 .. 127 | 70 | High-pass on the wires: thick to thin. | SNAP 127, DCY 80, then sweep. |
| 1 `b` | `BODY` Body Decay | 0 .. 127 | 52 | Length of the drum body. | SNAP to 20, then sweep. |
| 1 `n` | `BEND` Pitch Bend | 0 .. 127 | 60 | Downward pitch sweep of the body. | SNAP 20, BODY 100, then sweep BEND. |
| 1 `m` | `RING` Ring | 0 .. 127 | 64 | Level of the upper ring tone. | SNAP to 20, then RING 0 against 127. |
| 1 `,` | `DTUN` Detune | 0 .. 127 | 50 | Interval of the ring against the body. | RING to 127 first, then sweep. |
| 2 `z` | `DRV` Drive | 0 .. 127 | 26 | Saturation of the whole snare. | Sweep 0 to 127, then hear it against the clap. |

```text
TUNE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SNAP  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TONE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
BODY  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
BEND  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RING  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DTUN  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS SNAR -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 4. CLAP (pad `d`)

TESTBED. Solo: right Shift+`c`. For the room, raise its REVERB send (MIX, `v` row).

<!-- BEGIN ASSESS CLAP -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `TONE` Tone | 0 .. 127 | 57 | Pitch of the clap: thick to papery. | RES to 100 first so it is obvious, then sweep. |
| 1 `x` | `DCY` Decay | 0 .. 127 | 56 | Length of the room tail. | TAIL to 127 first, then sweep DCY. |
| 1 `c` | `SPRD` Spread | 0 .. 127 | 64 | Gap between the hand bursts: one clap to a loose group. | BRST to 4 first, then sweep SPRD. |
| 1 `v` | `TAIL` Tail | 0 .. 127 | 70 | Level of the tail against the bursts. | DCY to 100, then TAIL 0 against 127. |
| 1 `b` | `BRST` Bursts | 0 .. 4 | 3 | Number of hand bursts before the tail; 0 = one snap. | SPRD to 100 first so they are spaced, then step 0 to 4. |
| 1 `n` | `RES` Resonance | 0 .. 127 | 50 | Narrow, ringing band (808-ish). | Sweep with TONE at 30, then at 100. |
| 1 `m` | `AIR` Air | 0 .. 127 | 48 | High sizzle on the attack. | AIR 0 against 127; listen to the front of the clap. |
| 1 `,` | `DRV` Drive | 0 .. 127 | 30 | The 909-style crunch. | Sweep 0 to 127. |

```text
TONE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SPRD  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TAIL  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
BRST  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RES   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
AIR   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS CLAP -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 5. CLOSED HAT (pad `f`)

TESTBED. Solo: right Shift+`v`.

<!-- BEGIN ASSESS CHH -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `TUNE` Tune | 0 .. 127 | 64 | Pitch of the metal. | MTL to 127 first (all metal), then sweep. |
| 1 `x` | `DCY` Decay | 0 .. 127 | 58 | Length of the hat; it also cuts a ringing open hat. | Sweep 0 to 127. |
| 1 `c` | `TONE` Tone | 0 .. 127 | 80 | High-pass: thick to crisp. | Sweep 0 to 127. |
| 1 `v` | `MTL` Metal | 0 .. 127 | 84 | Noise against metal. | Sweep 0 (pure noise) to 127 (pure metal). |
| 1 `b` | `COLR` Colour | 0 .. 127 | 56 | Ringing colour of the filter. | TONE to 40 first, then sweep. |
| 1 `n` | `SNAP` Snap | 0 .. 127 | 40 | Short tick at the attack. | DCY to 0 first, then SNAP 0 against 127. |
| 1 `m` | `SPRD` Spread | 0 .. 127 | 64 | Spacing of the metal tones: tonal to clangy. | MTL to 127 first, then sweep. |

```text
TUNE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TONE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
MTL   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
COLR  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SNAP  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SPRD  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS CHH -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 6. OPEN HAT (pad `g`)

TESTBED, then left Shift+`g` and tap `3` `7` `e` `u`. A closed-hat hit cuts the open hat off; mute the closed hat (right Shift+`f`) to hear the whole ring. Solo: right Shift+`b`.

<!-- BEGIN ASSESS OHH -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `TUNE` Tune | 0 .. 127 | 64 | Pitch of the metal. | MTL to 127 first, then sweep. |
| 1 `x` | `DCY` Decay | 0 .. 127 | 52 | Length of the ring, 40 ms to 1 s. | CHH muted (`fn`+`f`): sweep; then unmute it to hear the choke. |
| 1 `c` | `TONE` Tone | 0 .. 127 | 72 | High-pass: thick to crisp. | Sweep 0 to 127 with DCY at 90. |
| 1 `v` | `MTL` Metal | 0 .. 127 | 84 | Noise against metal. | Sweep 0 to 127 with DCY at 90. |
| 1 `b` | `COLR` Colour | 0 .. 127 | 56 | Ringing colour of the filter. | TONE to 40 first, then sweep. |
| 1 `n` | `SNAP` Snap | 0 .. 127 | 32 | Short tick at the attack. | SNAP 0 against 127. |
| 1 `m` | `SPRD` Spread | 0 .. 127 | 64 | Spacing of the metal tones: tonal to clangy. | MTL to 127 first, then sweep. |

```text
TUNE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TONE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
MTL   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
COLR  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SNAP  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SPRD  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS OHH -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 7. PERC (pad `h`)

TESTBED, then left Shift+`h` and tap `4` `8` `r` `i`. WAREHOUS has a clave and a rim, ACIDJAM a rim. Solo: right Shift+`n`. The knobs mean different things per model (manual chapter 19): set MODL first and say which model in the note.

<!-- BEGIN ASSESS PERC -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `MODL` Model | `TOM` `RIM` `COWB` `CLAV` `SHKR` `ZAP` | RIM (1) | Which instrument: tom, rim, cowbell, clave, shaker, zap. | Step through all six while it plays; rate each one in the model lines below. |
| 1 `x` | `TUNE` Tune | 0 .. 127 | 64 | Pitch (shaker: filter frequency). | On TOM and ZAP, sweep while it plays. |
| 1 `c` | `DCY` Decay | 0 .. 127 | 64 | Length of the tail (zap: length of the sweep). | On TOM and SHKR, sweep. |
| 1 `v` | `TONE` Tone | 0 .. 127 | 64 | Colour; means something different per model (manual chapter 19). | On COWB (band-pass) and SHKR (resonance), sweep. |
| 1 `b` | `BEND` Bend | 0 .. 127 | 64 | Pitch drop (cowbell up or down, shaker attack, zap depth). | On ZAP with DCY 100, sweep. |
| 1 `n` | `HARM` Harmonic | 0 .. 127 | 64 | Interval between the two tones (shaker: grain). | On COWB, sweep and listen for the interval. |
| 1 `m` | `NOIS` Noise | 0 .. 127 | 64 | Stick, click, breath or noise layer. | On RIM and TOM, 0 against 127. |
| 1 `,` | `DRV` Drive | 0 .. 127 | 32 | Saturation. | On TOM, sweep 0 to 127. |

```text
MODL  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
TUNE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
TONE  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
BEND  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
HARM  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
NOIS  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS PERC -->

One line per model, after trying all its knobs:

```text
TOM   keep? _  best settings:  note:
RIM   keep? _  best settings:  note:
COWB  keep? _  best settings:  note:
CLAV  keep? _  best settings:  note:
SHKR  keep? _  best settings:  note:
ZAP   keep? _  best settings:  note:
```

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 8. LEAD (pad `k`)

TESTBED for the basics. ACIDJAM P1 for MIN9 house stabs, BASSHAUS P1 for the organ (WAVE ORGN). `l` in GROOVE changes the chord mode for new steps; a step keeps its own chord (hold it, `j`, `-` `=`). Long gates: hold the step, `s`, hold `=`. Solo: right Shift+`,`.

<!-- BEGIN ASSESS LEAD -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| 1 `z` | `CUT` Cutoff | 0 .. 127 | 72 | Filter cutoff: the held brightness. | ENV 0 first, then sweep. |
| 1 `x` | `RES` Resonance | 0 .. 127 | 30 | Resonance (level-compensated, so it doesn't jump out). | CUT 40, ENV 80 first, then sweep. |
| 1 `c` | `ENV` Env Amount | 0 .. 127 | 48 | Bite of the filter envelope. | CUT to 20 first, then sweep. |
| 1 `v` | `FDCY` Env Decay | 0 .. 127 | 60 | How long the bite lasts. | CUT 20, ENV 100 first, then sweep. |
| 1 `b` | `WAVE` Osc1 Wave | `SAW` `PULS` `TRI` `SUPR` `FM1` `FM2` `BELL` `ORGN` | SAW (0) | Oscillator 1 type. | Step through all eight with SUS 100 and long gates. |
| 1 `n` | `SHP` Shape | 0 .. 127 | 0 | Shape; what it does depends on WAVE. | For each WAVE in turn, sweep SHP (PULS: width, ORGN: drawbars). |
| 1 `m` | `DET` Detune | 0 .. 127 | 56 | Detune of oscillator 2; spread of SUPR. | WAVE SAW, MIX 0: DET 0 against 127; then WAVE SUPR. |
| 1 `,` | `DCY` Decay | 0 .. 127 | 70 | Amp decay to the sustain level. | SUS to 0 first, then sweep. |
| 2 `z` | `ATK` Attack | 0 .. 127 | 0 | Amp attack: stab to pad. | Long gates (hold step, `s`, hold `=`), SUS 100, then sweep. |
| 2 `x` | `WID` Width | 0% .. 100% | 76% (96) | Stereo width. Desktop only: the device is mono. | Headphones, WID 0% against 100%. |
| 2 `c` | `SUS` Sustain | 0 .. 127 | 80 | Sustain level: stab to held chord. | Long gates first, then SUS 0 against 127. |
| 2 `v` | `REL` Release | 0 .. 127 | 44 | Tail after the note ends. | SUS 100, REV send 0, then sweep. |
| 2 `b` | `FATK` Env Attack | 0 .. 127 | 0 | Filter envelope attack: swell instead of pluck. | CUT 20, ENV 100, long gates, then sweep. |
| 2 `n` | `FLT` Filter Type | `LP` `BP` `HP` | LP (0) | Low-pass, band-pass, high-pass. | CUT 60, RES 80, then flip through. |
| 2 `m` | `KTRK` Key Track | 0% .. 100% | 50% (64) | How much the cutoff follows the note. | CUT 30; play `z`, then `,` an octave up: 0% against 100%. |
| 2 `,` | `DRV` Drive | 0 .. 127 | 0 | Drive into a soft limiter: fatter stabs. | On four-note chords (ACIDJAM's MIN9 stabs), DRV 0 against 127. |
| 3 `z` | `OSC2` Osc2 Wave | `SAW` `PULS` `TRI` `SINE` | SAW (0) | Oscillator 2 wave. | MIX to +63 (oscillator 2 alone) first, then step through. |
| 3 `x` | `INTV` Osc2 Pitch | -24 .. +24 | 0 (24) | Oscillator 2 interval in semitones. | MIX 0, then try -12, +7, +12. |
| 3 `c` | `MIX` Osc Mix | -64 .. +63 | 0 (64) | Balance of oscillator 1 and 2. | INTV +7 first so you can tell them apart, then sweep. |
| 3 `v` | `LFO` LFO Rate | 0 .. 127 | 94 | LFO speed, 0.1 to 20 Hz (not tempo-synced). | VIB to 127 first, then sweep. |
| 3 `b` | `VIB` Vibrato | 0 .. 127 | 0 | Vibrato depth. | Long gates, then sweep. |
| 3 `n` | `LSHP` LFO>Shape | 0 .. 127 | 0 | LFO to SHP (pulse-width movement on PULS). | WAVE PULS, SHP 64, LFO 60, then 0 against 127. |
| 3 `m` | `LCUT` LFO>Cutoff | 0 .. 127 | 0 | LFO to cutoff. | CUT 50, RES 60, then sweep. |
| 3 `,` | `GLID` Glide | 0 .. 127 | 64 | Glide time on slid notes. | Two steps with different notes, chord off (hold step, `j`, `-` and `=` together), SLD on the first; sweep. |

```text
CUT   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RES   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ENV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FDCY  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
WAVE  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
SHP   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DET   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ATK   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
WID   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
SUS   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
REL   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FATK  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FLT   alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
KTRK  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
OSC2  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
INTV  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
MIX   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
LFO   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
VIB   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
LSHP  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
LCUT  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
GLID  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS LEAD -->

- Best sound (saved as):
- Worst:
- Reached for, not there:
- Three words for this voice:

## 9. Mixer

`0`, then a row key, then a pad for the track. One line per row; name tracks in the note (e.g. `OHH too loud`).

<!-- BEGIN ASSESS MIX -->
| Key | Row | Range | Defaults K S C CH OH P B L | What it does | Try |
|---|---|---|---|---|---|
| `z` | `LVL` Level | 0 .. 127 | 108 96 110 92 94 86 98 104 | Track level; 100 = unity. | Every track against the kick: is the default balance right? |
| `x` | `DUCK` Duck Amount | 0% .. 100% | 0% 0% 16% 0% 28% 28% 87% 50% | How much the ducker pulls this track down. | BASSHAUS P1: BASS 0% against 100%. |
| `c` | `DLY` Delay Send | 0% .. 100% | 0% 0% 9% 0% 8% 22% 0% 38% | Send to the delay. | ACIDJAM, LEAD: 0% against 100%. |
| `v` | `REV` Reverb Send | 0% .. 100% | 0% 22% 35% 8% 19% 24% 0% 35% | Send to the reverb. | CLAP: 0% against 100%. |
| `b` | `PAN` Pan | -64 .. +63 | C C C -6 +6 +10 C C | Left-right placement. Desktop only: the device is mono. | Headphones: CHH hard left, OHH hard right. |

```text
LVL   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DUCK  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DLY   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
REV   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
PAN   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS MIX -->

## 10. Ducker

`o`, then `o` until the DUCK tab. The curve shows one beat; `GAIN` is the live gain. A pad here puts that track's DUCK amount on `-` `=`. BASSHAUS P1 pumps hard; ACIDJAM P4 has no kick (try `BEAT`).

<!-- BEGIN ASSESS DUCK -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| DUCK `z` | `DSRC` Duck Source | `OFF` `KICK` `BEAT` | KICK (1) | What triggers the ducker: the kick, every beat, or nothing. | ACIDJAM P4 (no kick): KICK against BEAT. |
| DUCK `x` | `DDEP` Duck Depth | 0% .. 100% | 79% (100) | Depth of the dip. | BASSHAUS P1: sweep and watch `GAIN` on the curve. |
| DUCK `c` | `DHLD` Duck Hold | 0 .. 127 | 49 | Time the dip stays fully down. | DDEP 100% first, then 0 against 127. |
| DUCK `v` | `DREL` Duck Release | 0 .. 127 | 78 | Recovery time. | BASSHAUS P1: sweep, then try just under one beat (90-100). |
| DUCK `b` | `DCRV` Duck Curve | -64 .. +63 | 0 (64) | Recovery shape: snaps back or swells late. | DREL 90 first, then -64 against +63. |
| DUCK `n` | `DFX` Duck FX | 0% .. 100% | 50% (64) | How much the delay and reverb are ducked too. | LEAD delay send up first, then 0% against 100%. |

```text
DSRC  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
DDEP  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DHLD  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DREL  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DCRV  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DFX   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS DUCK -->

## 11. Delay

`o`, then `o` until the SPACE tab; columns `z` `x` `c` `n`. ACIDJAM P1: the lead is sent to the delay. WAREHOUS P1: a dub delay on the stab.

<!-- BEGIN ASSESS DELAY -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| SPACE `z` | `DTIM` Delay Time | `1/32` `1/16T` `1/16` `1/8T` `1/16D` `1/8` `1/4T` `1/8D` `1/4` | 1/8D (7) | Echo time as a note length. | ACIDJAM, LEAD delay send 100%, then step through all nine. |
| SPACE `x` | `DFB` Delay Fdbk | 0% .. 100% | 46% (58) | How many echoes. | Sweep to 100% and listen for runaway. |
| SPACE `c` | `DTON` Delay Tone | -64 .. +63 | 0 (64) | Echoes darker (negative) or thinner (positive). | DFB 80% first, then -64 against +63. |
| SPACE `n` | `DWID` Delay Width | 0% .. 100% | 79% (100) | Ping-pong width. Desktop only: the device is mono. | Headphones, 0% against 100%. |

```text
DTIM  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
DFB   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DTON  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DWID  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS DELAY -->

## 12. Reverb

`o`, then `o` until the SPACE tab; columns `v` `b`. Raise the clap's REVERB send first (MIX, `v` row, pad `d`).

<!-- BEGIN ASSESS REVERB -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| SPACE `v` | `RSIZ` Reverb Size | 0 .. 127 | 60 | Room size: decay 0.3 to 5 s. | CLAP reverb send 100% first, then sweep. |
| SPACE `b` | `RDMP` Reverb Damp | 0 .. 127 | 48 | Darkness of the room. | RSIZ 100 first, then sweep. |

```text
RSIZ  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
RDMP  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS REVERB -->

- Delay and reverb, what's missing:

## 13. Master

`o`, then `o` until the MASTER tab. The DJ filter is played with `[` (low-pass) and `]` (high-pass) from any view; both together = off.

<!-- BEGIN ASSESS MASTER -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| MASTER `z` | `BPM` Tempo | 60 .. 200 | 124 | Tempo. Also `tab` + `-` `=`, or tap `tab`. | Note where each demo feels best. |
| MASTER `c` | `VOL` Master Vol | 0 .. 127 | 100 | Master level (digital); 100 = unity. | Sweep and watch the level bar's red tick. |
| MASTER `v` | `FILT` DJ Filter | LP64 .. HP63 | OFF (64) | DJ filter: low-pass below OFF, high-pass above. | Hold `[`, then `]`; both together = OFF. |
| MASTER `b` | `FRES` Filter Reso | 0 .. 127 | 36 | Resonance of the DJ filter. | Hold `[` to about LP30, then sweep FRES. |
| MASTER `n` | `DRIV` Drive | 0 .. 127 | 0 | Master saturation: glue and loudness. | Full mix, sweep 0 to 127. |
| MASTER `m` | `ROOT` Scale Root | `C` `C#` `D` `D#` `E` `F` `F#` `G` `G#` `A` `A#` `B` | C (0) | Root of the scale the note keys play. | Play the note keys on BASS after each change. |
| MASTER `,` | `SCAL` Scale | `CHRO` `MAJ` `MIN` `DOR` `PHRY` `MIXO` `MPEN` `JPEN` `BLUE` `HMIN` | MIN (2) | Scale of the note keys. | Lead in AUTO: play `z` to `,` in each scale. |

```text
BPM   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
VOL   alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FILT  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
FRES  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
DRIV  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
ROOT  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
SCAL  alive: ____  dull: ____  ugly: ____  dead: _  with: ____  note:
```
<!-- END ASSESS MASTER -->

- Ducker and master, what's missing:

## 14. Sequencer features

Hold a step key in GROOVE (STEP EDIT), then a pad picks the field and `-` `=` change it: `a` VEL, `s` GATE (drums: PTCH), `d` PROB, `f` RTCH, `g` ACC, `h` SLD, `j` CHRD, `k` LOCK. `-` and `=` together = field default.

| Feature | How | Try |
|---|---|---|
| ACC accent | hold step, `g`, `=`; or left Shift+note while recording | BASS in TESTBED, two accents, ACC (SOUND 1 `b`) at 127 |
| SLD slide | hold step, `h`, `=` (bass, lead); or legato in step-write | BASS: slide into a note an octave up |
| RTCH ratchet | hold step, `f`, `=` (x1 to x4) | CHH last step of the bar at x3 |
| PROB probability | hold step, `d`, `-` (5 % steps) | CHH off-beats at 50 % |
| GATE / PTCH | hold step, `s`; synths: length 0.25-16 steps; drums: pitch ±24 | LEAD at 4 steps; KICK pitched up per step |
| VEL velocity | hold step, `a`; or a note key (drums: 8 velocities) | CHH 100/55/80/55 |
| CHRD chord | hold step, `j` (or `l`); chord mode for new steps: `l` in GROOVE | LEAD: MIN9 against AUTO7 |
| LOCK p-lock | SOUND/MIX: focus a value, hold step, `-` `=`; `del` removes | BASS CUT locked higher on one step |
| Step-write | stopped, `enter`; notes write and advance, `del` = rest | type a bassline, hold a note into the next = slide |
| Live record | playing, `enter`; notes land on the nearest step | record the lead live |
| Length | left Shift+step = all tracks; left Shift+`'` = add a page (to 64) | grow TESTBED to 32 steps |
| Polymeter | Control+left Shift+step = selected track only | CHH at 12 steps against 16; WAREHOUS uses it |
| Swing | hold `tab`, `;` `.`; or SWNG below | ACIDJAM at 50 %, 56 %, 66 % |

```text
ACC    ok? _  fun: _/5  wish:  note:
SLD    ok? _  fun: _/5  wish:  note:
RTCH   ok? _  fun: _/5  wish:  note:
PROB   ok? _  fun: _/5  wish:  note:
GATE   ok? _  fun: _/5  wish:  note:
VEL    ok? _  fun: _/5  wish:  note:
CHRD   ok? _  fun: _/5  wish:  note:
LOCK   ok? _  fun: _/5  wish:  note:
SWRITE ok? _  fun: _/5  wish:  note:
LIVE   ok? _  fun: _/5  wish:  note:
LENGTH ok? _  fun: _/5  wish:  note:
POLY   ok? _  fun: _/5  wish:  note:
```

<!-- BEGIN ASSESS SWING -->
| Key | Param | Range | Default | What it does | Try |
|---|---|---|---|---|---|
| MASTER `x` | `SWNG` Swing | 50% .. 75% | 50% (0) | Swing: delays every second sixteenth. | ACIDJAM: hold `tab`, `;` `.`; 54-58% is house. |

```text
SWNG  alive: __-__  dull: __-__  ugly: __-__  dead: _  with: ____  note:
```
<!-- END ASSESS SWING -->

## 15. Workflow

| Item | Keys (Mac) |
|---|---|
| Views | `9` SOUND, `0` MIX, `o` FX, `p` SONG, `\` PROJECT, `` ` `` home; the view key again = next page/row/tab |
| Help | hold `/` = key map (add a modifier for its layer); tap `/` = explain mode |
| Modifiers | hold right Shift (mutes), left Option (patterns), right Option (roll), `tab` (tempo), Control (edit) |
| Mute, solo | right Shift + pad / + note key |
| Pattern queue | left Option + step; left Option + left Shift + step = copy here and queue |
| Roll | right Option + pad while held; right Option + `;` `.` = rate |
| DJ filter, bass sweep | `[` `]`; left Shift + `[` `]` |
| Erase | hold Delete while playing (selected track) |
| Tempo | hold `tab` + `-` `=`; tap `tab` 3+ times |
| Copy, paste, undo | Control + `c` / `x` / `v` (track); + left Shift (pattern); Control+`z` undo, again redo (one level; every pattern edit, including placing, deleting and editing notes; not sound or mixer values) |
| Song mode | `p`; `.` to END, step keys append; `-` `=` repeats; `del` removes; `enter` chain on/off from the cursor |
| Save, load, new | Control+S; SAVE AS and LOAD in `\`; Control+N |
| Strip | the modifier lights, REC, STATS (Cmd+I) |
| Listen | Cmd+L: hear the Cardputer (USB audio firmware) through the Mac; skip without one |
| Stems | Cmd+B: bounce a project on the device or in `projects/`; Return bounces, Esc closes |

```text
VIEWS   ok? _  fun: _/5  wish:  note:
HELP    ok? _  fun: _/5  wish:  note:
MODS    ok? _  fun: _/5  wish:  note:
MUTE    ok? _  fun: _/5  wish:  note:
QUEUE   ok? _  fun: _/5  wish:  note:
ROLL    ok? _  fun: _/5  wish:  note:
FILTER  ok? _  fun: _/5  wish:  note:
ERASE   ok? _  fun: _/5  wish:  note:
TEMPO   ok? _  fun: _/5  wish:  note:
COPY    ok? _  fun: _/5  wish:  note:
SONG    ok? _  fun: _/5  wish:  note:
FILES   ok? _  fun: _/5  wish:  note:
STRIP   ok? _  fun: _/5  wish:  note:
LISTEN  ok? _  fun: _/5  wish:  note:
STEMS   ok? _  fun: _/5  wish:  note:
```

## 16. Overall

- What is fun:
- What is tedious:
- What I would show a friend first:
- The one thing to change first:

## 17. Bugs

Anything that looked wrong, sounded broken, crashed or did not do what the manual says. What you pressed, what happened, what you expected.

1.
2.
3.

---

*The parameter tables between the `BEGIN ASSESS` / `END ASSESS` markers are generated from the code (names, ranges and defaults as the screen shows them) with the texts in `docs/manual/assessment_notes.txt`. After a parameter changes: `build/host/param_reference --assessment docs/ASSESSMENT.md` from the repository root; answers already written are kept. `ctest` (test `check_assessment`) fails while the sheet and the code disagree.*
