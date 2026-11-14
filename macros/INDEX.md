# Macro sets, round 1

Since stage A (2026-11-13) these sets are the **built-in instruments**: `builtin.txt` lists them per voice (the first of each line is what a new project starts with) and `host/tools/macro_builtin` compiles them into the firmware's table (`cmake --build build/host --target macro_builtin && build/host/macro_builtin`, then commit; ctest `check_builtin` fails while it is stale). In `tools/pg-desktop --lab` a file here replaces the built-in of its voice and name while the app runs (docs/MACROLAB.md "Instruments").

Round 1 is not trying to be right. Every set on a voice is built on a different idea of what a macro *is*, so that your GOOD / BAD marks tell us which kinds are fun. Start the lab with `tools/pg-desktop --lab`, pick the track, step through the sets with `<` `>`. The first two macros of every set are the natural pair for the XY pad (the lab puts X and Y on them). Each file's header says the idea and what it should feel like if it works; its `about "..."` line (at most 38 characters) is an author's note and is not displayed. Full format: [docs/MACROLAB.md](../docs/MACROLAB.md).

Model v2 (2026-11-13) changed five sets to use its new features, so marks on them made before that date name the old version (the mark's `@hash` says which): KEYS and TRIO are native morphs (the pad's edges sound as before, the inside mixes the patches), POLES and CRACK carry their words as end labels and have readable macro names (POLES sounds the same; CRACK's PITCH now also lengthens or shortens the body, which round 1 meant but a later line undid), and VOYAGE covers its last wave jump. Every other set sounds exactly as in round 1; TICK, BOOM, WOOD and ALLOY gained end labels only.

## Bass

| set | file | family | idea | macros | try first |
|---|---|---|---|---|---|
| ACID | bass/acid.pgm | Interaction | OPEN is the 303 filter (cutoff + resonance); BITE growls when OPEN is low and squeals when OPEN is high | OPEN BITE WOBL SLIDE | OPEN and BITE on the pad, a slow line with slides |
| POLES | bass/poles.pgm | Opposites | four word-axes, neutral in the middle, a usable bass at every end (the words are end labels) | TONE TOUCH LENGTH DIRT (were DK-BRT SF-HRD SH-LNG CL-DRT) | start in the centre, push the pad to each edge |
| SURGE | bass/surge.pgm | Energy | MORE = brighter, driven, faster wobble, all at once; CALM = deep, round, sub an octave down, gliding | MORE CALM | ride MORE up over four bars, snap to CALM |
| ROAD | bass/road.pgm | Journey | ROAD travels DEEP > ACID > HOLLOW > GROWL > SCREAM | ROAD DIRT MOVE | a one-note groove, walk ROAD slowly left to right |
| DICE | bass/dice.pgm | Odd | stepped: five flavours, five intervals over a fixed sub root; GLITCH only wakes up with the later flavours | FLAV INTV GLITCH | click around the pad on the beat |

## Lead

| set | file | family | idea | macros | try first |
|---|---|---|---|---|---|
| HANDS | lead/hands.pgm | Gesture | macros named for the hand: opening brightens and loosens, pushing drives and shortens | OPEN PUSH SHAKE HOLD | OPEN and PUSH on the pad with chords; SHAKE on a held note |
| DRIFT | lead/drift.pgm | Movement | how the sound moves: BLOOM = longer notes with vibrato fading in (vibrato + decay), WANDER = LFO depth + speed, SWELL = pluck to swell | BLOOM WANDER SWELL | a slow line of long notes, BLOOM up |
| KEYS | lead/keys.pgm | Morph | three patches on the pad: organ, pulse pluck, FM bell (native morph; top right mixes pluck and bell) | PLUCK BELL (was OR-PLK BELL) | chords, sweep slowly round the pad |
| VOYAGE | lead/voyage.pgm | Journey | TRIP goes FLUTE > SAW LEAD > HOLLOW BP > SUPERSAW > FM METAL; every wave jump now covered by osc 2 | TRIP GRIT SPACE | hold a chord, walk TRIP over eight bars |
| SWAP | lead/swap.pgm | Odd | stepped: six whole leads, six parallel intervals; WARP is dull alone, wild on the FM / organ / bell steps | FLAV INTV WARP | click on the beat; then WARP with FLAV past the middle |
| PLUCK | lead/pluck.pgm | Gesture (sampler) | the sample struck like a key: a one-shot from one start point, the strike's tone and length are the macros | TONE LENGTH START SNAP | chord mode AUTO, TONE x LENGTH; a line with slides |
| CLOUD | lead/cloud.pgm | Movement (sampler) | the sample as a pad: a held note loops a window forward and backward; SCAN walks the window through the sample while chords hold | SCAN SPAN SWELL SHADE | hold a chord and tilt SCAN; SPAN low for a buzzing drone |
| REEL | lead/reel.pgm | Material (sampler) | the sample on worn tape: one slow pitch drift for every note, a crushed dull copy, held loops | WOW WORN HOLD ECHO | a slow line, WOW x WORN; chords with HOLD up |

The three sampler sets play the track's sample slot from the keyboard (`voice lead sampler`, docs/MACROLAB.md "The melodic sampler"): note C3 (the first note key at the lead's default octave) plays the sample as recorded, up to four notes at once.

## Kick

| set | file | family | idea | macros | try first |
|---|---|---|---|---|---|
| BOOM | kick/boom.pgm | Material | what drum it is: SIZE, SKIN tight/loose, WOOD-METAL, NEAR-FAR | SIZE SKIN WD-MTL FAR | four on the floor, SIZE x SKIN |
| TRIO | kick/trio.pgm | Morph | 808, 909 and gabber kicks on the pad (native morph; top edge goes 909 to gabber) | 909 GABBA (was 8-TO-9 GABBA) | drag a slow circle |
| PUSH | kick/push.pgm | Gesture | PUSH = drive + punch + shorter, HOLD = held body, FLICK = sharper click | PUSH HOLD FLICK | ride PUSH into a break |

## Snare

| set | file | family | idea | macros | try first |
|---|---|---|---|---|---|
| CRACK | snare/crack.pgm | Opposites | low-high, dry-rattle, soft-hard, a 909-ish snare in the middle (the words are end labels; PITCH now also moves the body) | PITCH WIRES TOUCH (were LO-HI DR-RTL SF-HRD) | the backbeat, PITCH x WIRES |
| WOOD | snare/wood.pgm | Material | what it is made of: wood knock to metal ring, piccolo to field drum, tight to loose | WD-MTL SIZE LOOSE | WD-MTL x SIZE on the backbeat |
| BITE | snare/bite.pgm | Interaction | BITE = distortion on a low drum, wires and brightness on a high one | PITCH BITE ROOM | sweep the pad diagonally |

## Clap, hats, perc

| set | file | family | idea | macros | try first |
|---|---|---|---|---|---|
| HYPE | clap/hype.pgm | Energy | MORE = more hands, tighter, brighter, crunchier; CALM = a scattered crowd in a big room | MORE CALM | claps on 2 and 4, ride MORE into a drop |
| SWARM | clap/swarm.pgm | Odd | a countable, pitched clap: HANDS steps the number of hands, PITCH + RING play a pentatonic clap melody | PITCH RING HANDS | RING high, click PITCH in time |
| TICK | chh/tick.pgm | Opposites | dark-bright, short-long, soft-hard closed hat | DK-BRT SH-LNG SF-HRD | 16ths, DK-BRT x SH-LNG |
| ALLOY | chh/alloy.pgm | Material | seed shaker to metal bell, tiny to big, ringing to choked | WD-MTL SIZE DAMP | 16ths, WD-MTL x SIZE |
| SWISH | ohh/swish.pgm | Movement | how the hit moves in time: the pedal, echoes that carry the rhythm, a blooming tail | OPEN ECHO SHIMR | off-beat open hats, OPEN x ECHO |
| BLAST | ohh/blast.pgm | Energy | MORE = brighter, metallic, harder; CALM = a dark, soft, far wash | MORE CALM | ride MORE over 8 bars |
| TRAIL | perc/trail.pgm | Journey | tom > rim > cowbell > clave > zap, with the pitch continuous across the model changes | TRAIL DROP DIRT | a 16th roll, sweep TRAIL |
| TAP | perc/tap.pgm | Gesture | a hand drum: soft tap to slap, pressing the skin, letting it ring | HIT PRESS LET | 16ths with accents, PRESS while it rolls |
| SHAKE | perc/shake.pgm | Opposites | the shaker model, every macro centred on its default: dark to bright, a tight tick to a long wash, sand to beads, a soft swell to a hard driven snap (BASSHAUS's shaker) | TONE LENGTH GRAIN STROKE | 16ths with ghosts, TONE x GRAIN; TIGHT + HARD + BRIGHT is a hat tick |
| CHOP | perc/chop.pgm | Gesture (sampler) | a sample in 16 slices, each note picks its own; pitch, tone, length | SLICE PITCH TONE LENGTH | a row of notes, each with its own SLICE |
| TAPE | perc/tape.pgm | Movement (sampler) | the whole sample as tape: speed through stopped into reverse, start point, wear, echo | SPEED START WORN ECHO | long notes, SPEED down for a tape stop |
| STUTTER | perc/stutter.pgm | Energy (sampler) | one chop repeated at a tempo-synced rate, rising or falling per repeat | RATE RISE SLICE LENGTH | single steps, RISE up, RATE by hand |

## What round 1 is testing

Read a mark as evidence about the family, not only the set. Each family appears on at least two voices (Morph, Movement and Interaction on two, Opposites on four since SHAKE, the others on three).

| family | the idea of a macro | sets |
|---|---|---|
| Gesture | named for what the hand does; coupled physical moves | lead HANDS, kick PUSH, perc TAP |
| Opposites | an axis between two words, neutral in the middle | bass POLES, snare CRACK, chh TICK, perc SHAKE |
| Journey | one long road through several distinct sounds, spice on top | bass ROAD, lead VOYAGE, perc TRAIL |
| Morph | whole patches at the corners of the pad, crossfaded | lead KEYS, kick TRIO |
| Material / body | what the thing is made of, its size, tension, distance | kick BOOM, snare WOOD, chh ALLOY |
| Energy | one macro means "more", the other "calm in a different way" | bass SURGE, clap HYPE, ohh BLAST |
| Movement | how the sound moves in time, not its static tone | lead DRIFT, ohh SWISH |
| Interaction | one macro changes what another means (`by`) | bass ACID, snare BITE |
| Odd | stepped jumps between flavours; macros that only work combined | bass DICE, lead SWAP, clap SWARM |

Questions the marks answer: do continuous coupled moves (Gesture, Energy) beat named axes (Opposites)? Is one long road (Journey) more fun than a pad of corners (Morph)? Do stepped, dice-like macros (Odd) feel playful or random? Is the `by` interaction (ACID, BITE) noticed at all?
