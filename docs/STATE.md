# Work state (2026-11-13)

v0.1 complete and host-verified: 55/55 host tests pass; both firmware envs build clean.
Nothing has run on real hardware yet. Waiting on the user for: smoke-test report
(build/firmware/pocket-groove-smoke.bin), first boot of the instrument
(build/firmware/pocket-groove.bin), and listening feedback on renders.

Stem bounce + USB project transfer (tools/pg, docs/MANUAL.md chapter 15): host-verified
(itest_bounce, itest_golden, test_io_xfer, test_pg_client: the Python client against the
device protocol code on a pty); the firmware side (src/app glue, real USB-Serial/JTAG timing,
DTR/RTS not resetting the board, LittleFS/FAT behaviour) has not run on hardware.

USB audio (stereo input over USB, UAC1 + CDC): the spike (env:usb-audio-spike) ran on the
owner's hardware and Mac (enumerates, records, 0 underruns; one start-of-stream overrun, since
fixed). The real feature is env:cardputer-adv-usb (2026-11-12, branch of the USB audio worktree):
the instrument renders stereo, sends it to the Mac as the "Pocket Groove" input and plays
(L+R)/2 on the jack; serial is TinyUSB CDC (boot log replayed, tools/pg finds both ports).
Built and checked offline only (all envs warning-free, host tests, run_checks.sh with the new
stream-start model); env:cardputer-adv is byte-identical apart from build timestamps.
Run on the owner's hardware and Mac (2026-11-12, no SD card): AUD mid-40s % with spikes just
over 50 (jack-only build: about 30), heap 54 KB free / 34 KB minimum-ever (jack-only: 66 / 38),
0 USB under/overruns, recording free of clicks. It is now the default env (default_envs);
env:cardputer-adv stays as the jack-only build. Not yet checked on hardware: 32 kHz, unplug and
sleep, flashing back to the jack-only build, heap with an SD card mounted (expect about 17 KB less).
Stem bounce via tools/pg confirmed working on hardware (2026-11-12).

Desktop app (tools/pg-desktop, host/app, docs/DESKTOP.md): SDL2 window + audio + Mac keyboard
around the unchanged core and UI; host-verified headless (test_desktop_app: screenshots identical
to ui_shot, F9 recording bit-identical to the offline engine, session saved at quit). Run on the
owner's Mac: works and sounds right. Fast follows a + b below are built (2026-11-12, branch of the
desktop-strip worktree): the strip under the screen (modifier lights, LISTEN / STEMS / REC / STATS),
Listen (Cmd+L, the "Pocket Groove" USB input mixed in, PI rate match: test_desktop_listen, an hour
at +-500 ppm with no underrun) and Stems (Cmd+B, tools/pg --json as a subprocess). Host-verified
headless only (simulated device, xfer_sim); not yet run on macOS (mic permission prompt, real
device clock, Finder reveal).

Order of work (owner, updated 2026-11-12):
1. DONE and confirmed on hardware: real-time performance (IRAM placement), stereo USB audio as
   the default firmware, stems via tools/pg, desktop app with strip / Listen / Stems, backlight
   noise gone, ghost filter (pad k + SHIFT + n harmless), explain-mode text.
2. NOW: the owner's assessment of the sounds on the desktop build. The full parameter-by-
   parameter pass in docs/ASSESSMENT.md is DROPPED (2026-11-13: the owner played every bass
   and lead setting and most of kick and snare, is happy overall, and the document is far too
   long to fill in). Instead: play the remaining voices and effects by ear and answer a handful
   of free-form questions (what was fun, what was ugly or useless, what was missing).
   ASSESSMENT.md stays as a reference only. In parallel: env:hw-spike (tilt sensor and mic
   sampling to/from flash) to learn what the hardware can do before designing around it.
3. STARTED (2026-11-13): the macro redesign (see Design direction). The owner will make a song
   only AFTER the redesign: the goal is a fun experience that is not hindered by the tiny device.
   First step built: the macro lab (docs/MACROLAB.md), a workbench, not the design. A portable,
   heap-free macro model and .pgm text format (lib/pg/src/pg/macro: per track a base patch and up
   to 4 macros, each driving parameters through breakpoint curves with set/add/mul and "by"
   scaling; model v2 below), and `tools/pg-desktop --lab`: hot-reloaded sets from macros/, sliders and an XY
   pad (standing in for tilt), good/bad marks with a note to lab-notes.txt. The loop: Claude
   writes or edits sets -> the owner `git pull`s (the app picks them up while playing) -> plays,
   marks -> pastes lab-notes.txt back. Host-verified only (headless: test_desktop_app part 8); not
   yet run on the owner's Mac.
   ROUND 1 of candidates (2026-11-13): 24 sets in macros/, every track, each set built on a
   different family of macro idea (gesture, opposites, journey, morph, material, energy, movement,
   interaction, odd), listed in macros/INDEX.md with what each family is testing. Written blind
   but checked by host/tools/macro_audit (renders a phrase over a grid of macro positions; ctest
   macro_audit fails any silent, clipping or NaN point; docs/MACROLAB.md "Auditing a set"). NEXT:
   the owner plays and marks them; round 2 keeps the families that score and drops the rest.
   DIRECTION (owner, after playing round 1, 2026-11-13: "these are all so good... it almost
   makes me think we can offer these as different instruments in each slot"): A MACRO SET IS AN
   INSTRUMENT, a character plus up to four controls; a track slot holds one, and the device will
   later load the same .pgm files from its flash file system, eight loaded at once.
   MODEL V2 built for that (2026-11-13, docs/MACROLAB.md "The .pgm format"): pooled storage per
   set (64 targets, at most 32 per macro; 192 curve points, at most 16 per curve; 4 named patches
   over 96 parameter=value pairs; 8 morph and 8 trim lines), packed points (position and value
   in thousandths, exact for every number the format writes); one loaded set is 1856 bytes (v1:
   2420), evaluation 130-340 ns per set on the host, no libm. New in the format: `patch` +
   `morph` (blend from the base toward a named patch; several morphs mix the patches, scaled down
   when their amounts add up to more than 1; enums switch at 0.5 or a stated point), end labels
   and a description per macro (`macro TONE "DARK" "BRIGHT" "..."`), the file's first comment as
   the set's about line, `trim` (dB on the level, applied last), warnings (a plain set that
   undoes an earlier line; `override` silences it). The lab shows labels, the about line,
   warnings and ticks where an enum jumps; macro_audit lists warnings and enum jumps and prints
   ready-to-paste trim lines. All 24 round-1 files load unchanged and evaluate bit-identically
   (itest_macro_pin over frozen copies in host/tests/macro_v1/). Rewritten with v2: KEYS and
   TRIO (native morphs; pad edges as before, the inside now mixes the patches), POLES and CRACK
   (end labels, readable macro names; CRACK's PITCH now really moves the body: the new warning
   found that a later plain set had undone it), VOYAGE (its last wave jump now covered). The
   other sets sound exactly as before (TICK, BOOM, WOOD, ALLOY got end labels). Not built, not
   designed out: loading sets on the device, device UI, project format changes, smoothing
   (the device UI and the project format are stage A, below; sets come from flash, not files).
   Found on the way: the engine's delay and reverb returns produce subnormal floats on default
   settings (one-pole states decaying in silence); harmless on the host so far, not checked on
   the ESP32-S3.
   Facts found for the redesign (docs/MACROLAB.md, last section; host/tools/zipper_probe):
   parameters are 7-bit (one bass CUT value = 73 cents of cutoff, lead 83). Smoothing: bass
   cutoff CV ~1.2 ms and level-type params ~1.6 ms one-poles, the rest unsmoothed; lead ~5 ms on
   cutoff/reso/shape/width/drive/osc mix, the rest unsmoothed; drums none at all (a change
   recomputes coefficients with powf/expf, mid-hit too); mixer rows ~4 ms. A slow 7-bit cutoff
   sweep does not click (step transients equal a smooth sweep's within 2 dB) but is a staircase:
   at RES 110 the harmonics near the peak jump ~11 dB at each step with stillness between, very
   likely audible as stepping on slow, narrow sweeps (not listened to: WAVs in build/zipper/).
   So the redesign needs finer internal values or a longer slew for swept parameters, and cheap
   per-quantum updates in the drums.
   STAGE A DONE (2026-11-13, branch of the instruments worktree): INSTRUMENTS ARE REAL. A macro
   set is an instrument; every track slot holds one; its four macros replace the pages of bare
   sound parameters on the device and the desktop. Built: the 24 sets in macros/ are compiled by
   host/tools/macro_builtin into a checked-in const table (lib/pg/src/pg/macro/builtin_data.cpp,
   flash, used in place; order and first-instrument choice in macros/builtin.txt; ctest
   check_builtin fails when stale; regenerate: `cmake --build build/host --target macro_builtin
   && build/host/macro_builtin`); projects name an instrument by a 16-bit id (FNV-1a of
   voice:NAME, folded) plus four macro positions 0..255, in TrackSettings' reserved bytes
   (Project unchanged in size, kProjectVersion 2; a version-1 project and the demos are CLASSIC
   and sound bit-identical); the engine derives the sound parameters on the audio thread on a
   change (and at a step for macro locks, lockParam 33..36), the mixer rows stay the player's
   (instrument level = gain trim, sends add; pan and duck the player's); an unknown id plays the
   values it was saved with; the SOUND view is the instrument page (macro key pairs z/x c/v b/n
   m/, so two macros move at once, ; . 9 step through the list with an overlay, CTRL+Z undoes a
   run of changes, CLASSIC page invites a choice; new projects start with the first instrument
   of every track; SHIFT+[ ] moves the bass's macro 1); the parameter pages remain behind
   developer mode (desktop --dev, implied by --lab); the lab is the library's overlay (a .pgm
   replaces the built-in of its voice and name while the app runs, others are extra instruments)
   and its sliders move the track's own macros. Details: docs/MACROLAB.md "Instruments",
   docs/UX.md 10.3. Host-verified only (itest_instrument, itest_ui_instrument, itest_golden,
   test_desktop_lab, test_desktop_app part 8); not run on the device or the owner's Mac yet.
   Firmware: both envs build warning-free; static RAM +840 B (USB 227372 -> 228212, jack
   226828 -> 227668), flash +61 KB (1024603 -> 1085859, 962039 -> 1023283; the table is 44 KB);
   the IRAM placement is unchanged (85 / 84 functions, its checks OK). Note: an incremental pio
   build after editing lib/pg can trip the placement check (an object not recompiled); clean
   the env (`pio run -e ENV -t clean`) and build again.
   LATER STAGES (not built, not designed out): (B) finer-than-7-bit, smoothed parameter motion
   for what macros sweep (MACROLAB.md "What a moving parameter sounds like"), cheap per-quantum
   updates in the drums; (C) "motion": per-note values of all four macros with glide (see
   Motion below; the step's single macro lock is the seed of it); (D) tilt as a macro control;
   (E) a polished, reactive picture on the instrument page (enum jump ticks, end labels as
   pictures); (F) a sampler instrument; (G) converting the demos from CLASSIC to instruments.
   STANDING RULE (owner, 2026-11-13): development does not wait for macro assessment. The owner
   will request macro changes continually and there will be no single final assessment; a
   retuned .pgm is regenerated into the table and committed like any other change.
4. LATER: a design sprint on the flow between desktop and device, giving each its own UX.
ON HOLD, do not build: MIDI controller input. The owner is not convinced they want it (it
distracts from the intended flow) and it needs more design than a quick proposal.
No further desktop-specific features for now beyond the strip, Listen and Stems (and the macro lab,
a development workbench behind --lab, not a product feature).

Design direction (owner, 2026-11-12; brainstorm only, nothing built yet): capture the
playfulness of the OP-1 without its feature set. Sounds are shaped through a few designed
macro controls with a reactive picture, not through bare parameters; the bare parameters are
NOT exposed to the player ("be bold and confident in our choices"). Tilt (the IMU) as a
control appeals. Candidates: characters per voice, dice/mutate/morph, punch-in effects,
mic sampling (try samples in flash on the Cardputer before considering a bigger device).
The macros get designed from the owner's short free-form feedback on the sounds (see item 2), not from a per-parameter survey.

Motion (decided with the owner 2026-11-12, to be built as part of the macro redesign; needs a
new project format, which is fine because the owner writes no real song before the redesign):
- Today: one parameter lock per step (set by hand, applied at the note), live NOTE recording,
  ratchets stored per step; ALT+pad rolls and parameter movements are not recorded.
- Decision: one set of macro values per NOTE (no values on empty steps for now). Between notes
  the value moves in a straight line through time, arriving exactly at the next note's start
  and continuing through silence ("rule A"); so a note held across a sweep plays a ramp. A value
  cannot rise and fall within one note. The last note of a pattern glides toward the first note
  of whatever plays next; a track with one note stays constant. Motion recording writes the live
  macro values (keys or tilt) into the notes as the pattern passes.
- Known limit, accepted: a note followed by a long gap can only glide slowly. If it bites, allow
  values on empty steps later; that would not change how existing recordings play.
- Engineering this needs: voices that accept smooth parameter changes cheaply (some drums
  recompute expf/powf on every change), and RAM for the values (about 16-32 KB for 4 macros on
  every step of 16 patterns x 8 tracks x 64 steps, against a 34 KB minimum-free margin).
- Tilt verified on hardware (hw-spike): fun, steady, no key loss; all four response presets
  useful, so response speed should belong to the control being moved, not be global.

Known follow-ups:
- USB audio build: what takes the 28 KB between free and minimum-ever heap (its `r` report
  prints the minimum by boot phase); if a mounted SD card was not part of the 66 / 38 KB
  measurement, a card costs about 25 KB of heap in the default build. RAM options with costs:
  docs/usb-audio.md 6.4.
- pg::io: FAT (SD) save-replace has a power-cut window inside FatFs rename; make it safe.
- Per-pattern sound settings and a second p-lock per step (data-model change, v0.2).
- Autosave while playing, if the measured flash stall allows it.
- Performance recording: log the timestamped Commands the UI posts and replay them through
  host/common/bounce.h (Pass timeline), so live moves end up in bounces.
- CPU: reciprocal for the bass divide, reverb tank, shared hat bank (if hardware load is high).
- UX.md / mockup key maps lag the implementation in a few places (see MANUAL.md, which is accurate).
Rule from the user: subagents run on Opus (Sonnet for trivial tasks), never Fable.
