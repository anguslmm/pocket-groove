# Key ghosting audit (three keys down)

Status: written 2026-11-11 after the device test confirmed that the Cardputer-Adv keyboard matrix has no diodes. The filter and remap it describes are in `lib/pg/src/pg/ui/ghost.*` and `ui.cpp`; the tests are in `host/itests/itest_ui_ghost.cpp`.

## 1. Where the rectangles are

The keyboard is a TCA8418 scanning a **7 x 8 matrix**, not the printed 4 x 14 layout. M5Cardputer's decoder (`TCA8418.cpp remap()`, docs/hardware-notes.md 4.5) gives, for the printed position (row, col):

    scan row = col / 2          (0..6)
    scan col = row + 4 * (col & 1)   (0..7)

| scan col \ scan row | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| **0** | `` ` `` | 2 | 4 | 6 | 8 | 0 | = |
| **1** | tab | w | r | y | i | p | ] |
| **2** | fn | a | d | g | j | l | ' |
| **3** | ctrl | alt | x | v | n | , | / |
| **4** | 1 | 3 | 5 | 7 | 9 | - | del |
| **5** | q | e | t | u | o | [ | \ |
| **6** | shift | s | f | h | k | ; | enter |
| **7** | opt | z | c | b | m | . | space |

Three closed keys on three corners of a rectangle of **this** table make the controller report the fourth corner as well (and more generally any key whose scan row and scan column are connected through closed keys). Keys that look far apart are often neighbours here, and keys in one printed row often form rectangles: `a`+`s`+`d` ghosts `f`, `q`+`w`+`e` ghosts `tab`, `z`+`x`+`c` ghosts `alt`. (docs/BRINGUP.md had called `q`+`w`+`e`+`r` and `z`+`x`+`c`+`v` "not rectangles"; in the scan matrix they contain one.)

The phantom down arrives in the same controller batch as the third real key, in either order. The two new downs are electrically interchangeable: the four corners of a rectangle are symmetric, so nothing in the hardware says which of the two is real. Releasing any of the three real keys makes the phantom go up in the same batch.

## 2. Method

Every gesture below was expanded into every concrete key combination it allows (all 16 steps, all 8 pads or notes, both `-` and `=`, and so on), held in the order the gesture is played. For each combination the phantom was computed from the scan matrix, then fed through the new filter with the phantom both after and before the real key (the counts below are what the code does, not estimates). "Today" is the behaviour of the UI before this change: its 16.1 rule ignored a third non-modifier key while two non-modifiers were held, and accepted modifiers always, so which of the pair got through depended on the order inside the batch.

"Dropped" means the filter could not tell the real key from the phantom and dropped both: the key does nothing, nothing wrong happens, and releasing the keys leaves nothing stuck.

## 3. Gestures that hold three keys

### 3.1 Designed gestures (MANUAL chapter 4, help tables)

| # | Gesture | Combinations / with a phantom | Phantom | Today | Now |
|---|---|---|---|---|---|
| 1 | `ctrl`+`shift`+`x` cut pattern | 1 / 1 | `f` | Both delivered; `ctrl`+`f` means nothing, so harmless. | `x` acts, `f` dropped. |
| 2 | `ctrl`+`shift`+`c` copy pattern | 1 / 0 | - | Works. | Works. |
| 3 | `ctrl`+`shift`+step track length | 16 / 0 | - | Works. | Works. |
| 4 | `opt`+`shift`+step copy pattern | 16 / 0 | - | Works. | Works. |
| 5 | `shift`+`[`+`]` bass cutoff back | 2 / 0 | - | Works. | Works. |
| 6 | step held + `shift`+`-`/`=` (x8 in STEP EDIT and lock mode) | 32 / 1 | step `1` + `shift` + `-` ghosts `;` | Order-dependent: with `;` first the note jumps an octave (wrong edit) and `-` is ignored. | `-` acts (VEL -8), `;` dropped, in both orders; also when `shift` is pressed last. |
| 7 | step held + `shift`+`;`/`.` note ±12 | 32 / 2 | step `1`: `;` ghosts `-`; step `q`: `;` ghosts `[` | Order-dependent: VEL -8 instead of the octave, or a BASS CUTOFF sweep (`shift`+`[`) that runs until release. | **Unusable for step 1: remapped**, see 4. |
| 7' | step held + `ctrl`+`;`/`.` note ±12 (new) | 32 / 0 | - | (`ctrl`+`;` did nothing) | Works for every step. |
| 8 | step held + `shift`+`l` previous chord | 16 / 0 | - | Works. | Works. |
| 9 | step held + `-` and `=` together: field default / remove lock | 16 / 0 | - | **Broken**: three non-modifiers, the second value key was ignored. | Works. |
| 10 | `tab`+`shift`+`-`/`=` tempo ±10 | 2 / 0 | - | Works. | Works. |
| 11 | `tab` + `-` and `=` together: default tempo | 1 / 0 | - | **Broken** (`tab` counts as a non-modifier). | Works. |
| 12 | step held + `tab`+`-`/`=` tempo (MANUAL 4.2: tab works while a step is held) | 32 / 1 | step `1` + `tab` + `-` ghosts `p` (SONG) | **Broken** for every step (third non-modifier). | `-` acts, `p` dropped. |
| 13 | step held, field pad still down, `-`/`=` | 256 / 14 | `'` (page), `enter` (record), `l` (chord), `;` (note +1), e.g. `2`+`a`+`=` ghosts `'` | `-`/`=` ignored until the pad is let go. | `-`/`=` acts, phantom dropped. |
| 14 | lock mode: step held, column note key still down, `-`/`=` | 256 / 16 | `ctrl`, `alt`, `space`, `.`, `/`, `,`, `x`, `v`, e.g. `2`+`z`+`=` ghosts `space` | `-`/`=` ignored; the `ctrl` and `alt` phantoms (`1`+`,`+`-`, `3`+`,`+`-`) were **accepted**: that layer stayed active while the keys were held. | `-`/`=` acts, phantom dropped. |

### 3.2 Playing situations

| # | Situation | Combinations / with a phantom | Phantom | Today | Now |
|---|---|---|---|---|---|
| 15 | note held + `shift`+note (accented slide, step-write) | 56 / 0 | - | Works. | Works. |
| 16 | pad held + `shift`+note | 64 / 7 | `ctrl` or `opt`: `f`/`h`/`k`+`shift`+`x`/`v`/`n` ghost `ctrl`, `s`/`f`/`h`/`k`+`shift`+`z`/`c`/`b`/`m` ghost `opt` | **Dangerous.** The phantom modifier was always accepted. Arriving first it turns the note into a combo: `f`+`shift`+`x` = `ctrl`+`shift`+`x` **cuts the pattern**, `h`+`shift`+`v` pastes, `k`+`shift`+`n` = `ctrl`+`n` **new project**; with `opt` the note is silent. Arriving second it stays held, so the next key is a `ctrl`/`opt` combo (`z` = undo, a step = queue pattern) and the pattern overlay pops up. | Note plays with accent, phantom dropped. |
| 17 | note held + `fn`+pad (mute) | 64 / 7 | `opt` or `ctrl` | Phantom modifier accepted; `fn` wins the layer, so the mute worked. | Works, phantom dropped. |
| 18 | pad held + `fn`+pad | 56 / 8 | `shift` | Phantom `shift` accepted, harmless here. | Works, phantom dropped. |
| 19 | pad held + `fn`+note (solo) | 64 / 7 | `opt` or `ctrl` | As 17. | Works, phantom dropped. |
| 20 | note held + `fn`+note | 56 / 0 | - | Works. | Works. |
| 21 | `alt`+pad held + `alt`+second pad (two rolls) | 56 / 12 | a note: `alt`+`a`+`d` ghosts `x`, `alt`+`s`+`f` ghosts `x`, ... | Order-dependent: second roll, or nothing. | Both roll, phantom dropped. |
| 22 | rolling + `alt`+`;`/`.` rate | 16 / 1 | `alt`+`s`+`;` ghosts `,` | Order-dependent: rate change or nothing. | Rate changes. |
| 23 | note held + `alt`+pad (roll over a held note) | 64 / 14 | another pad (12) or `l`/`;` (2) | Order-dependent: the right or the **wrong track** rolls. | 2 work; **12 dropped** (list in 5). |
| 24 | note held + `opt`+step (queue a pattern over a drone) | 128 / 22 | another step (12), `` ` `` (home), `tab`, `9`, `o` | Order-dependent: right pattern, **wrong pattern**, or the phantom acts (`` ` `` leaves the view and ends record, `9`/`o` switch view). | 10 work; **12 dropped** (list in 5). |
| 25 | pad held + `opt`+step | 128 / 0 | - | Works. | Works. |
| 26 | three pads (finger drumming) | 168 / 72 | the fourth pad of the rectangle | Third pad ignored (two-key rule). | **72 dropped** (both); the other 96 now play all three pads. |
| 27 | three notes (legato runs, chords) | 168 / 54 | `alt` (z+x+c, b+v+z, ...), `.`, or a note | Third note ignored; the `alt` phantom was **accepted**: the next pad rolled instead of playing. | 18 work (phantom `alt`/`.` dropped); **36 dropped** (note phantom); the other 114 play all three. |
| 28 | two notes + pad | 224 / 42 | a pad, a note, `l`, `;` | Third key ignored. | 6 work, **36 dropped**; the other 182 play. |
| 29 | name entry, typing fast (three letters overlap) | - | a letter, `del`, `enter`, `tab`, `` ` ``, ... | Third key ignored. | Pairs in a rectangle dropped (a lost letter, never a stray `enter`/`del`); other overlaps type correctly. |
| 30 | resampling (UX.md 10.3, added with it): `shift`+`l`; then `l` held + `shift`+pad or note (select, play), one pad, one note, `` ` `` | 1 + 8 + 8 + 8 + 8 + 1 / 0 | - | - | Works on all three matrix models (`itest_ui_resample`). |
| 30' | `l` held (resampling, stopped) + two pads or two notes at once | 56 / 7 | the rectangle's fourth key: `a`+`s`, `d`+`f`, `g`+`h`, `j`+`k`; `x`+`,`, `v`+`,`, `n`+`,` | - | The pair is dropped (two hits lost, nothing wrong happens); the take goes on. Play them one after the other. |

**Totals.** 14 designed or incidental three-key gestures (rows 1-14, counting 7 and 7' once), 15 playing situations (15-29), and resampling's holds (30, 30'). Ghost-prone among the designed ones: 1, 6, 7, 12, 13, 14. The filter saves all of them except row 7 on step 1, which is remapped. Row 7 on step `q` would also need the gesture table to prefer `;` over `shift`+`[`; it no longer matters after the remap.

## 4. Remap

| Before | After | Why |
|---|---|---|
| STEP EDIT: `shift`+`;` / `shift`+`.` = note ±12 (drums: pitch ±12) | **`ctrl`+`;` / `ctrl`+`.`** | With step `1` held, `shift`+`-` (VEL -8) and `shift`+`;` (octave) close the same rectangle (`1` `shift` `-` `;`): electrically identical, both meaningful. `ctrl`+`;`/`.` closes no rectangle with any step. `shift`+`-`/`=` keeps x8. |

`shift`+`;`/`.` with a step held is now ±1 like the plain keys. Because of the hardware, pressing the old `shift`+`;` on step 1 now reads as `shift`+`-` (VEL -8); on step `q` it does nothing.

No other designed gesture needed a remap. The playing situations that remain ambiguous (5) are inherent to the matrix and no key change inside the gesture can fix them.

## 5. Combinations that still do nothing (both keys dropped)

Release the held note (or pad) first:

- note `x` / `v` / `n` held + `alt`+pad: `x`+`alt`+{`a` `s` `d` `f`}, `v`+`alt`+{`a` `s` `g` `h`}, `n`+`alt`+{`a` `s` `j` `k`}.
- note `z` / `c` / `b` held + `opt`+step: `z`+`opt`+{`1` `3` `q` `e`}, `c`+`opt`+{`1` `5` `q` `t`}, `b`+`opt`+{`1` `7` `q` `u`}.
- three pads on a rectangle: any three of {`a` `s` `d` `f`}, {`a` `s` `g` `h`}, {`a` `s` `j` `k`}, {`d` `f` `g` `h`}, {`d` `f` `j` `k`}, {`g` `h` `j` `k`} (pads pair up as `a`/`s`, `d`/`f`, `g`/`h`, `j`/`k`: two pads of one pair plus one of another pair).
- three notes on a rectangle: any three of {`x` `c` `v` `b`}, {`x` `c` `n` `m`}, {`v` `b` `n` `m`} (pairs `x`/`c`, `v`/`b`, `n`/`m`).
- two notes + a pad on a rectangle, e.g. `z`+`c`+`a` (ghost `d`), `x`+`v`+`d` (ghost `g`): 36 combinations.

## 6. How the filter decides (ghost.h)

Per key-down, using the keys the controller currently reports closed (delivered ones and dropped ones):

1. If the key's scan row and column are already connected through the settled closed keys, it is a phantom: dropped; its key-up is swallowed later. (A lone completing key: the third real key came earlier.)
2. If the key would close a loop, its partner (the corner that will read closed) is expected in the same batch. The key is held back. When the partner arrives within 3 ms of event time, the pair is resolved: the one that, pressed last with the keys already held, forms a known gesture (the table in `ghost.cpp`, the rows of section 3 with their play order) is real and the other is dropped; if both or neither do, both are dropped. If no partner comes (diodes, or a lost event), the key is delivered by the next UI frame (about one frame, up to ~37 ms, late).
3. Otherwise the key is delivered at once, behind any held-back key so order is kept.

Every key-up first releases held-back downs (as real), then: a dropped key's up is swallowed, a real key's up is delivered. A down for a key the filter thinks is a phantom means its up was lost; it starts afresh. So no release order and no lost event can leave a key stuck down or produce a spurious one. The old "third non-modifier ignored" rule is gone: three keys that close no rectangle all act.

Limits: keys the firmware swallows before the UI (the wake-up key after the backlight dimmed, `-`/`=` used with G0 for volume) are not seen by the filter, so a rectangle involving one of them is not recognised.

## 7. Tests

`host/itests/itest_ui_ghost.cpp` drives each gesture above through a diode-less matrix model (`host/common/ghost_matrix.h`, written independently of the filter) with the phantom both last and first in its batch, checks the intended action and the absence of the phantom's action, and releases 34 chords in all 6 orders (pressed forwards and backwards). The behaviour, edit, files and fuzz tests run through the same model in both orders; the fuzz checks that no key is left down whenever everything has been released.

## 8. To check on the device

- `q`+`w`+`e` lights `tab`, `z`+`x`+`c` lights `alt` on the smoke test's KEYS page (confirms the scan-matrix model beyond `a`+`s`+`d`).
- Instrument: hold step `1`, hold `shift`, tap `-`: VEL goes down 8 and the note does not jump. Hold step `1`, `ctrl`+`;`: octave up.
- Hold pad `k`, hold `shift`, tap `n`: accented lead note, no NEW PROJECT, no change to the pattern.
- `ctrl`+`shift`+`x`: pattern cut (and undo with `ctrl`+`z`).
