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
   Facts found for the redesign (host/tools/zipper_probe; stage B, below, fixed what they found):
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
   the IRAM placement is unchanged (85 / 84 functions, its checks OK). Note (fixed 2026-11-13 in tools/pio_audio_ram.py): an incremental pio
   build after editing lib/pg can trip the placement check (an object not recompiled); clean
   the env (`pio run -e ENV -t clean`) and build again.
   STAGE E (first pass) DONE (2026-11-13, branch of the instrument-page worktree): THE PAGE HAS A
   PICTURE. SOUND shows a live oscilloscope of the selected track's own voice (engine scope tap:
   engine.h "SCOPE TAP", a 256-float ring, box-averaged decimation, single writer / reader, off
   unless the page is shown; UI side lib/pg/src/pg/ui/scope.*: synth windows of 2-3 periods of
   the last note, triggered at a rising crossing matched to the previous frame, drums from the
   hit's note-on, slow auto-gain, settles to a calm line), the macros as four horizontal sliders
   in a 2 x 2 block (key caps at the ends, end words, default notch, enum-jump ticks, focused /
   moving / locked states), a player's `about "..."` line per set (new .pgm statement, 24 lines
   written; the header comment stays the lab's), the list as a compact column over the picture,
   CLASSIC with the picture and two pick cards. Frames where only the picture moved push only its
   band (rows 37..72). Audio unchanged (itest_golden; itest_scope renders a demo with the tap on
   and off: bit-identical). Host-verified only (itest_scope, itest_ui_instrument, screenshots in
   docs/ux/instrument*.png); not seen on the real panel. Firmware: both envs warning-free; static
   RAM +1424 B (USB 228212 -> 229636, jack 227668 -> 229092: ring 1 KB, tap state, the UI's
   scope state); flash +9.3 KB (1085859 -> 1095187, 1023283 -> 1032703); IRAM placement
   unchanged (85 / 84 functions, checks OK).
   STAGE B DONE (2026-11-13, branch of the macro-motion worktree): MACROS MOVE CONTINUOUSLY.
   The engine keeps a trajectory per track and macro: a SetMacro ramps to its target in 72 ms
   (longer than the 33/66 ms gaps between key repeats on the device, so a held key, the lab's
   sliders and XY pad and later tilt join into one movement; setMacroCommand takes an explicit
   ramp, for the motion stage's "arrive at the note"). While a macro moves the instrument is
   evaluated unrounded every 11.6 ms where the macros will be at the end of that interval, and
   the voices get fractional parameters every 1.45 ms (drums every 11.6 ms), the mixer a
   fractional level and sends; entering and leaving take 50 ms from / into the stored bytes,
   then the track is on the 7-bit path again, bit for bit. Stored and shown values stay 7-bit.
   Bass: every non-enum parameter continuous (its mapping has no libm); lead: the 14 tone
   parameters continuous through cheap mappings, times and enums stepped as before; drums:
   pitch, filter, drive and mix amounts continuous (polynomials, no libm per tick), envelope
   times and the perc model latched at the next hit (libm once per hit). Jumps: instrument
   changes, ramp 1, macro locks (a lock wins over a moving macro), enums and semitones.
   Details: MACROLAB.md "What a moving parameter sounds like", engine.h "MACRO MOTION", the
   voice headers. Proof (host): ctest zipper_motion (held-key sweeps of bass ACID OPEN, lead
   HANDS OPEN and kick BOOM SIZE over a roll through the engine: the voice's own cutoff /
   pitch path moves at most 0.9 / 0.9 / 2.7 cents per quantum against 19 / 5.8 / 17.5 on the
   7-bit path, as smooth as one ideal ramp; the bass's harmonics too); itest_motion (bytes
   when still, no jump on entering, steady glide, exact return, jumps, lock wins, rebind,
   ramp timing); itest_golden and macro_audit unchanged. Cost: bench_motion (two macros on
   each of bass and lead sweeping on the busy pattern): callgrind ratio 1.052 stereo / 1.056
   mono, about 2.7 % / 2.4 % of a core on the ESP32-S3 by estimate (2.1..3.3); nothing moving:
   +5 instructions per frame (0.1 %). The set is evaluated from flash 86 times a second per
   moving track; the per-tick part, the bass and lead mappings are in IRAM. Firmware (both
   envs warning-free): IRAM functions 85 -> 93 (USB) / 84 -> 92 (jack), .iram0.text +3.9 KB
   (121471 -> 125399, 114535 -> 118467), static RAM +2.5 KB (228212 -> 230700, 227668 ->
   230156), flash +11 KB (1085859 -> 1097103, 1023283 -> 1034595): about 6.4 KB less heap.
   HARDWARE CHECK (not done): flash env:profile-ram (the shipping IRAM placement; env:profile
   is the all-flash A/B) and read the MOTION lines (RUN mot-still vs
   RUN mot-sweep at 44.1 and 32 kHz: the "% of a core" figure should be under ~3, and the
   sweep's max block should not stand out); then on the instrument hold z/x on ACID's OPEN
   over a held bass note and compare with stage A (no staircase should be heard), sweep the
   lab's XY pad, and check the AUD load (`r`) and the minimum-ever heap with two macros
   moving (the build adds about 6 KB of static RAM + IRAM, so expect the 34 KB minimum to
   drop by about that).
   RAM WALL (found 2026-11-13 when stage B and the scope were merged): the USB build's static
   data + IRAM overflowed the linker's data segment (dram0_0_seg, 341.7 KB) by 1.2 KB. Static
   growth costs heap 1:1 until that segment is full, and then the link fails; the rest of the
   heap lies above the segment where static data cannot go. Fix: the firmware's 32.9 KB frame
   buffer is now allocated from the heap before anything else (src/app/main.cpp), which frees
   that much of the segment; total RAM use is unchanged. USB build now: _heap_start 0x3FCD3B30
   (31.7 KB of the segment left for static growth). Expected free heap about 44 KB and
   minimum-ever about 24 KB (was 54 / 34 measured before stages A, B and the scope, which cost
   about 9.6 KB together); NOT MEASURED on the device yet. Stage C (motion) cannot add 16-32 KB
   of step data: pack the per-note macro values into the existing 8-byte Step instead (done, below).
   STAGE C DONE (2026-11-13, branch of the motion worktree): NOTES MOVE THE MACROS. Project
   version 3 packs the step (still 8 bytes, Project unchanged at 67136; static_asserts in
   project.h): note, flags, vel, gate, prob as before, the chord in the spare bits (flags bits
   3/6/7, prob bit 7), and the last three bytes are a CLASSIC track's one parameter lock (param,
   value) or, on an instrument track, four 6-bit macro codes (0 = no own value, 1..63 = 63 levels
   0..255). Versions 1 and 2 load losslessly (Engine::sanitizeProject repacks; a v2 instrument
   track's macro lock becomes that macro's value on the note; sound and MIXER LOCKS ON
   INSTRUMENT TRACKS ARE DROPPED; CLASSIC tracks keep their lock; the demos are bit-identical,
   itest_golden unchanged). The engine (engine.h "MOTION"): per track and macro the notes with
   own values are keyframes at their grid time with swing, firing or not; a straight line from
   one to the next arriving in the quantum it fires, through silence, across the wrap and into
   what plays next (queued pattern, song slot; none there: the base at the boundary); one
   keyframe = constant; late queue, edits, tempo and swing re-aim from where it is; Play starts
   where the loop would be; Stop rests; a move of a macro with motion only sets its base, except
   while recording (live: from a move until a still bar; every note it passes takes the value).
   The old macro lock is gone (no locks on instrument tracks). UI: hold a step + macro key =
   that note's value of that macro (all four on one note; DEL: the focused macro's, then all;
   STEP EDIT field MOVE, `k` again = next macro), sliders follow the engine while playing (base =
   white tick), amber diamond (was a ~ until 2026-11-13) = the macro has motion here, red chip = live, hold DEL + macro key =
   clear its motion (undo), a recording pass is one undo unit. Docs: UX.md 7 and 10.3 "Motion",
   MANUAL.md 4.4 "Motion" and 10, MACROLAB.md "Motion"; screenshots docs/ux/motion-*.png,
   groove-motion.png, step-edit-motion.png. Host tests: itest_format (v1/v2 files built byte
   for byte -> expected v3), itest_motion_notes (exact keyframe values, lines, swing, wrap,
   queue / song / late / tempo / polymeter / first pass / base / stop / live), itest_ui_instrument
   (held values, recording pass + undo, clear gesture, sliders following, three key-matrix
   models), test_io_xfer (v2 and v3 files through the transfer), zipper_motion (two ACID notes a
   bar apart: cutoff path as smooth as one ideal ramp, within 0.9 cents of it). Firmware (both
   envs warning-free; before -> after, USB / jack): flash 1106527 -> 1117131 / 1044031 ->
   1054527, static RAM 199212 -> 199580 / 198668 -> 199036, _heap_start 0x3FCD3B30 ->
   0x3FCD3FA0 / 0x3FCD1E10 -> 0x3FCD2280, IRAM functions 93 -> 95 / 92 -> 94,
   .iram0.text 125963 -> 126891 / 119023 -> 119951 (motionStep, per step, and macroPos,
   for the published positions, joined the placement lists).
   HARDWARE CHECK (not done): on the device, ACID on the bass, two notes a bar apart with
   different OPEN (hold step 1, z z z; hold step 9, x x x): the filter should open in one smooth
   sweep between them with no steps; the sliders should move with it; a recording pass (enter,
   hold c/v while playing) should come back on the next loop and undo with ctrl+z; AUD load (`r`)
   with motion on bass and lead at once (a glide is a moving macro: stage B's ~1.3 % per track by
   estimate); minimum-ever heap (the build moved _heap_start by about 1.1 KB). The owner's ears:
   whether 63 levels per note are enough, whether one bar is the right "still" time for live, and
   whether resting at Stop (instead of returning to the base) feels right.
   STAGE D DONE (2026-11-13, branch of the tilt worktree): TILT MOVES THE MACROS. Hold G0 and
   tilt: two macros of the selected track's instrument follow the roll (X) and the tip away /
   toward (Y), from the pose at the press (any holding angle), from every view; release glides
   back in 150 ms. A hold engages after 150 ms, so a tap (and the diagnostics double tap) never
   moves anything; G0 + -/= (volume) cancels the tilt for that hold. Which macros and which of
   the spike's four responses (fast / smooth / fine / wide = acc-fast / acc-smooth / fused /
   fused-wide) belong to the instrument: `.pgm` lines `tilt x NAME [response]`, `tilt y none`
   (default X macro 1, Y macro 2, smooth; MacroSet +2 bytes: sizeof 1896 -> 1900). Assignments
   (where the default was not the best: macros/*.pgm, docs/MACROLAB.md "Tilt"): BOOM Y FAR, PUSH Y
   FLICK, SWARM X RING / Y none, TAP X PRESS fast / Y HIT, TRAIL X DROP / Y DIRT, DICE X GLITCH
   fast / Y none, POLES Y DIRT, SURGE Y none, HANDS Y SHAKE, KEYS Y none, DRIFT X WANDER wide /
   Y BLOOM fine, SWAP X WARP fast / Y none, VOYAGE X GRIT / Y SPACE wide; the other 11 sets keep
   the default. Additive and momentary: heard = clamp(base or motion + offset, 0, 255); an
   axis at its full range = 160 of 255 (from the middle an end at 79 % of the range: ~28 deg
   smooth, 24 fast / fine, 48 wide); nothing stored. Engine (engine.h "TILT OFFSET"):
   SetMacroOffset (track, macro, signed 1/16 units, ramp in ms, hold flag), a second trajectory
   slot per macro on stage B's ramps; with every offset 0 the output is bit for bit the old
   engine's (itest_tilt: a render with motion, key moves and a recording pass hashes to what
   the engine before stage D gave). Recording: record on + playing + a tilt that left the dead
   zone = live (the notes that start take the heard value); the release ends live at once; over
   a motion the hand takes the macro where the motion has it. UI: marks next to the tilted
   macros' names (left-right / up-down arrows, white while held), handles follow the engine;
   other views: `[TRACK] TILT` + a pose dot in the status bar; CLASSIC: one toast per hold;
   explain mode explains G0. Device: src/platform/imu.* (BMI270 at 0x69/0x68: ODR 100 Hz, +-4 g,
   +-1000 dps; one 12-byte burst per reading; a task on core 1, prio 9, that sleeps unless G0
   is held and then reads at 100 Hz, sensor ODR 200 Hz), src/app/tilt_input.* (pg::tilt::Tracker on the IMU task:
   one gravity estimate per response in use, one sqrtf per normalisation, a polynomial atan2,
   coefficients computed at the press; atomics to the UI loop, which wakes every 5 ms while G0
   is held and posts each reading at once with a 15 ms glide). Desktop: G0 (F1 / Esc) held +
   the mouse = tilt through the same Ui path (0.5 deg per screen pixel); ui_shot `tilt X Y`.
   Host tests (76, all pass): test_tilt_math (angles at any holding angle within 0.01 deg, the
   four responses, a gyro bias stays within bias x tau, a jolt rejected by the fused ones),
   test_macro_tilt (the `tilt` lines, errors, round trips), itest_tilt (engine: additive,
   clamp, glide back, over motion every quantum, recording, bit-identical; UI: tap, hold,
   sensitivity, volume cancel, CLASSIC, track taken at engage, recording + undo, explain;
   every built-in axis moves a continuous parameter), test_desktop_app part 9 (the mouse);
   check_builtin, macro_audit, itest_macro_files pass. Screenshots: docs/ux/tilt-*.png, the
   instrument page ones regenerated with the marks.
   Firmware (all four envs warning-free, cardputer-adv-usb, cardputer-adv, hw-spike, profile;
   before -> after, USB / jack): flash 1117223 -> 1125295 / 1054619 -> 1062743 (+8 KB), static RAM
   199580 -> 200340 / 199036 -> 199796 (+760 B: the engine's offset slots 512, the tracker, the
   UI's tilt state, the reader), _heap_start 0x3FCD3FA0 -> 0x3FCD4398 / 0x3FCD2280 -> 0x3FCD2678
   (+1016 B with the IRAM), IRAM functions 95 -> 95 / 94 -> 94 (placement checks OK; macroPos
   grew: .iram0.text 126891 -> 127163 / 119951 -> 120219), plus the IMU task's 3072 B stack and
   its control block from the heap at boot (about 3.4 KB).
   CPU (estimates, not measured): idle (G0 up) nothing (the task is blocked; one compare per
   UI loop pass). While tilting, core 1: the bus 100 x ~0.6 ms busy-waited = ~6 % (the spike
   measured 728 us for 18 bytes; 12 now), the tracker ~1.5-6 k cycles per reading from flash
   = ~0.06-0.25 %, the UI loop's 5 ms wakeups ~0.1 %: about 6.4 %, over the 4 % target on
   purpose and only while G0 is held (the lead's choice after the merge: feel over core 1 time). Core 0
   (audio): the tilted track is on stage B's fractional path while the hand moves (~1.3 % per
   track by stage B's estimate), nothing once it holds still (7-bit path where it stands).
   Latency (estimate): reading age ~5 ms + smoothing 6-27 ms + loop wakeup <= 5 ms + the 15 ms
   glide + the audio buffer 17 ms = about 45-70 ms (the spike: 31-36 ms at 200 Hz without the
   UI hop; the stage as first built read at 50 Hz with a 30 ms glide, 65-90 ms).
   HARDWARE CHECK (not done; only the device can confirm): the chip's orientation (roll right
   must move X up, tipping the far edge away Y up: else flip kSignX / kSignY or kSwapXY in
   src/app/tilt_input.cpp; the spike's `rest:` line answers it), the [imu] report line (reads,
   12-byte read time, stack free of 3072), key events while tilting (none lost: type fast with
   G0 held), the CPU load of core 1 while held, whether 30 deg from the middle to an end feels
   right per axis (pitch is less comfortable than roll), whether the fused responses start
   cleanly from the press without a boot gyro calibration (a creep would show as a slow drift
   inside the dead zone), the 65-90 ms feel against the spike's, the 150 ms tap threshold
   against the double tap, and the minimum-ever heap (about 1 KB more static RAM + IRAM, 3.4 KB
   of task stack and control block from the heap).
   SHAKE (2026-11-13, branch of the shaker worktree): A SHAKER INSTRUMENT FOR PERC, closing stage
   G's shaker gap. macros/perc/shake.pgm on the voice's SHKR model (the model was always there;
   no instrument used it), in builtin.txt after TAP and TRAIL. Four Opposites macros, each
   centred on the model's default values (the CLASSIC BASSHAUS shaker): TONE DARK..BRIGHT (band
   2.5 -> 11 kHz, resonance and dry hiss with it), LENGTH TIGHT..LONG (8 -> 250 ms), GRAIN
   SAND..BEADS (dense wide-band grains -> sparse grains through a narrow ringing band),
   STROKE SOFT..HARD (slow swell, darker -> instant attack, brighter, driven, dry noise); base
   lvl 108 (+5.9 dB over the perc default, a shaker otherwise sits ~15 dB under TAP); tilt X
   TONE, Y GRAIN. macro_audit: loudness -30.7..-23.0 dB (range 7.7), peak max -6.8 dBFS, no hot
   point, no dead travel, no enum jump, travel TONE 15 LENGTH 33 GRAIN 15 STROKE 18, centroid
   4.2..13.2 kHz. BASSHAUS's PERC is SHAKE with every macro at 127 (fitted with demo_compare:
   sound distance 0.00, the derived parameters equal the CLASSIC ones) and level 72 (x 108/86 =
   the CLASSIC 90): its stem is bit-identical to the CLASSIC demo's in every pattern and the song
   (16-bit WAVs); against stage G's TRAIL: centroid 1.6 -> 7.9 kHz, level within 0.5 dB, the
   mix within 0.1 dB. itest_golden: the two BASSHAUS instrument hashes retaken (CLASSIC hashes
   unchanged). demo_compare --wav-track T also writes a track's stem. Sampler screenshots retaken
   (CHOP is 4/6 in PERC's list; ui_scripts/sampler.txt steps one further). Not done: no metal
   (a tambourine's jingles ring at fixed partials; the SHKR model is filtered noise, so BEADS is a
   rattle, not a jingle); every perc parameter latches at the next hit, so a tilt is heard hit by
   hit. Firmware (cardputer-adv-usb, warning-free): flash 1288151 -> 1290079 (+1928: one set),
   static RAM unchanged (200892). Host: 84/84. NOT LISTENED TO.
   STAGE G DONE (2026-11-13, branch of the demos worktree): THE DEMOS USE INSTRUMENTS. Every
   track of ACIDJAM, BASSHAUS and WAREHOUS plays a built-in instrument (no CLASSIC track left
   in the firmware), chosen from the whole table by measurement, not by ear: host/tools/
   demo_compare (--fit ranks every instrument of a track over a grid of macro positions, first
   by the derived parameters, then by rendering the track's stem against the CLASSIC one:
   10 octave bands and the 10 ms envelope, level-normalised, then refines the best of each
   instrument; --compare renders every pattern and the song before / after through the stem
   tap and prints per track RMS, peak, centroid, low/mid/high balance, plus clipping, faults
   and the ducker; --inverse maps a CLASSIC lock value to a macro position). Mixer levels set
   so every track of every pattern and song measures within 1.5 dB of the CLASSIC demo (the
   widest: 1.4 dB); reverb sends lowered where an instrument adds reverb. No clipping, no faults,
   ducker unchanged (lowest and mean gain equal within 0.01). The CLASSIC per-step locks became the
   notes' own macro values (motion): ACIDJAM's cutoff ramps -> ACID OPEN (intro, build, break,
   outro), its ENV ramps -> BITE (jam patterns P1-P3), the stabs' cutoff -> HANDS OPEN, the
   break's held chords -> HOLD; BASSHAUS's per-note fold / FM / LFO-rate / LFO>timbre locks ->
   a colour per riff note on ROAD's DIRT and MOVE, its intro / build / outro cutoff -> ROAD;
   WAREHOUS's roll cutoff -> POLES TONE, its ENV -> TOUCH (jam), the stab ENV -> HANDS PUSH,
   the break's stab cutoff -> OPEN. Rule (itest_demo): in the jam patterns (P1-P3, P8) the
   bass's and lead's macro 1 carries no motion, so its keys and SHIFT+[ ] are always heard.
   Lost in the conversion: the delay throws (mixer locks on single lead stabs; instrument
   tracks have no locks): the delay return of the outros is 4.5-18 dB lower. Power-on: BASS
   selected, focus its macro 1 (ACID OPEN; boot no longer forces param 0, which on an
   instrument bass was the bare CUT), GROOVE; P1 moves BITE, so SOUND shows a slider moving by
   itself at first power-on (docs/ux/first-*.png, host/ui_scripts/firstboot.txt; UX.md 14, 15).
   Gaps (no instrument covers them; macros/ untouched): BASSHAUS's shaker (no perc instrument
   reached above 5 kHz: TRAIL at its zap end, centroid 7.9 kHz -> 1.6 kHz; CLOSED since by the
   perc instrument SHAKE, see "SHAKE" below: the track is now the CLASSIC shaker exactly); the rim snares of
   BASSHAUS and WAREHOUS (WOOD, the closest); BASSHAUS's organ is brighter above 2 kHz (KEYS)
   and its growl decays on long notes (ROAD's amp decay; the break's growls are played harder
   to keep the level). Tests: itest_golden keeps every CLASSIC hash, rendering a frozen copy of
   the old demos (host/itests/classic_demos.inc), and pins the new demos (7 new cases);
   itest_demo (instruments, the jam rule, motion at power-on), itest_ui_files (first boot:
   focus OPEN, BITE moving); tools/pg.py's LS retries as often as the link retries a line
   (test_pg_client's 10 %-fault case failed on half the seeds once the demo files changed).
   Manual screenshots regenerated (docs/manual/shots.txt now runs with developer mode off, as
   on the device). Firmware (both envs warning-free, clean builds; USB / jack): flash 1117223
   -> 1116679 / 1054619 -> 1054043 (the demo builders 9754 -> 9581 bytes: the note tables
   became bytes), static RAM unchanged (199580 / 199036), IRAM placement unchanged (checks OK).
   Host: 73/73. Before / after WAVs: build/stageg/wav/ (demo_compare --wav). NOT LISTENED TO.
   STAGE F1 DONE (2026-11-13, branch of the sampler worktree, not yet on the device): THE
   SAMPLER INSTRUMENT ("simple processed vocal chops"). A set's voice line may name a voice
   kind (`voice perc sampler`); PERC hosts a SamplerVoice besides its percussion voice and the
   engine switches at the quantum edge where the instrument changes kind (Engine::setAltVoice;
   the old voice is choked and fades). Sample pool: twelve slots of 68 sectors in app1 (stock
   table; refused unless app1 is at 0x340000 + 0x330000), a 500-byte header per slot (name,
   peak, CRCs, 232-column overview), up to 6.2 s mono int16 at 22,050 Hz; memory-mapped once
   at boot (src/platform/sample_flash.*). An empty slot plays one of three synthesised voice
   phrases (rom_samples_data.cpp, 137 KB of flash, host/tools/gen_rom_samples). The track's
   slot is TrackSettings' free byte (now `sampleSlot`; Step and Project unchanged, old files
   load unchanged). SamplerVoice (pg/sampler/sampler.h): one chop per track, 32.32 linear
   interpolation, region checked before every read, slices / start / length / reverse /
   speed through stopped into reverse / pitch / attack / release / SVF LP-BP-HP / bit crush /
   sample-and-hold / drive / tempo-synced repeats with pitch and fade per repeat / loop
   (20 parameters, docs/MACROLAB.md "The voice line"). Sets: perc CHOP (SLICE PITCH TONE
   LENGTH), TAPE (SPEED START WORN ECHO), STUTTER (RATE RISE TONE LENGTH), in builtin.txt
   after TAP and TRAIL (the demos are unchanged). Engine: a sampler note with its own macro
   values is primed with them before its note-on (primeNote), so CHOP's per-note SLICE is the
   note's. Desktop: samples/ next to projects/ (01-name.wav .. 12-name.wav, polled, any WAV
   converted), drop a file on the window. Device: tools/pg sample list | put | get | rm over
   new transfer requests (SLS SPUT SGET SRM; refused while playing; an interrupted put leaves
   the slot empty). Converter: host/common/sample_convert.h and tools/pg_sample.py, the same
   algorithm, checked against each other (identical on this host's test vectors). UI: the
   waveform in the scope's box (region, slices, playhead, slot and name / BUILT-IN), SHIFT+;
   / . steps the slot (undo), note keys = the chop's pitch, GATE is a gate (UX.md 10.3
   "Sampler instruments"; MANUAL.md chapter 20; docs/ux/sampler-*.png).
   FIGURES (USB / jack, clean builds, warning-free in our sources; before = main 68b6678):
   flash 1124655 -> 1287235 / 1062159 -> 1224667 (+162.6 KB: the ROM sounds 136.7 KB, the
   rest code and the three sets); static RAM 200340 -> 200892 / 199796 -> 200364 (+552 / +568);
   .iram0.text 127163 -> 129807 / 120219 -> 122863 (+2,644 each); _heap_start 0x3FCD4398 ->
   0x3FCD50C0 / 0x3FCD2678 -> 0x3FCD33B0 (3,368 / 3,384 bytes less heap: expect the ~20 KB
   minimum free heap to drop by about that); IRAM functions 95 -> 107 / 94 -> 106 (the sampler's
   render and per-quantum work; its note-on path, updateTone and primeNote stay in flash).
   CPU (ESTIMATE, not measured): a plain chop ~120 cycles per output sample (the hw-spike's
   measured 44-46 cycles for a mapped-flash read + interpolation, plus envelope, de-click and
   bookkeeping; on the host it costs what PERC's tonal model does, 6.1 ns): ~2.2 % of a core
   at 44.1 kHz while it sounds; with every stage on (filter, crush, hold, drive) ~230 cycles,
   ~4 %: over the 1-2 % target. env:profile does not have a sampler line yet.
   Host: 84/84 (76 on main; itest_voice_kind, itest_sampler, itest_ui_sampler, test_sampler_voice, test_desktop_samples,
   test_sample_convert, macro_audit_test_sample, check_rom_samples new). WAVs (not committed):
   build/sampler/{chop,tape,stutter}.wav (with kick and hats), build/sampler_solo/ (alone),
   from host/tools/sampler_demo. NOT LISTENED TO.
   ONLY THE DEVICE CAN CONFIRM: mapping 3.19 MB of app1 next to the app's own mappings
   (boot log `[boot] samples: app1 mapped ...`); the CPU cost (and that a chop from mapped
   flash does not glitch beside the full groove); that esp_partition_write leaves the
   mapping coherent (commit checks it and answers ERR VERIFY if not); the audio gap while a
   slot is written (stopped transport only, but notes played by hand stutter); the transfer
   speed (~273 KB of base64 lines at 128 bytes each, about 2,150 round trips: est. 20-40 s for a
   full slot); the minimum free heap after the IRAM growth; and all of it by ear.
   F2 (microphone recording) NEEDS FROM F1: the pool's write side is ready (SampleWriter:
   begin / write in order / commit, erase per sector as data arrives, header last), so a
   recorder only has to stream mono int16 at 22,050 Hz into it (resample the codec's 44.1 kHz
   by 2 with a short half-band filter) and commit; the mic DIN fix (hardware-notes 11.6) and
   a RAM ring for the flash write stalls (the spike measured 351 us per 256-sample chunk,
   nothing lost) are F2's. A UI to arm / record / trim, and a decision whether recording is
   allowed while the transport plays (the spike recorded while streaming fine; writing stalls
   the CPU, the audio DMA ring must cover it).
   LATER STAGES (not built): (F2) microphone recording into the sample pool.
   STANDING TASTE RULE (owner, 2026-11-13: "get the stupid tag lines for each synth out of there"):
   no descriptive captions for instruments on the screen. `about "..."` stays in the .pgm as an
   author's note, not displayed; the instrument page gives those rows to the picture (y 29..72).
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
- BUILT as stage C (above; engine.h "MOTION", docs/MACROLAB.md "Motion").
- Decision: one set of macro values per NOTE (no values on empty steps for now). Between notes
  the value moves in a straight line through time, arriving exactly at the next note's start
  and continuing through silence ("rule A"); so a note held across a sweep plays a ramp. A value
  cannot rise and fall within one note. The last note of a pattern glides toward the first note
  of whatever plays next; a track with one note stays constant. Motion recording writes the live
  macro values (keys or tilt) into the notes as the pattern passes.
- Known limit, accepted: a note followed by a long gap can only glide slowly. If it bites, allow
  values on empty steps later; that would not change how existing recordings play.
- Engineering: voices that accept smooth parameter changes cheaply (stage B: Voice::setParamsFine,
  the engine's ramps) and RAM for the values: none (stage C packed 4 x 6 bits into the 8-byte step).
- Tilt verified on hardware (hw-spike): fun, steady, no key loss; all four response presets
  useful, so response speed should belong to the control being moved, not be global.
  BUILT as stage D (above: the instrument's `tilt` lines choose the macros and responses).

Known follow-ups:
- USB audio build: what takes the 28 KB between free and minimum-ever heap (its `r` report
  prints the minimum by boot phase); if a mounted SD card was not part of the 66 / 38 KB
  measurement, a card costs about 25 KB of heap in the default build. RAM options with costs:
  docs/usb-audio.md 6.4.
- pg::io: FAT (SD) save-replace has a power-cut window inside FatFs rename; make it safe.
- Per-pattern sound settings (data-model change). (A second p-lock per step: instrument tracks now
  carry four macro values per note instead; CLASSIC tracks keep one lock.)
- Autosave while playing, if the measured flash stall allows it.
- Performance recording: log the timestamped Commands the UI posts and replay them through
  host/common/bounce.h (Pass timeline), so live moves end up in bounces.
- CPU: reciprocal for the bass divide, reverb tank, shared hat bank (if hardware load is high).
- UX.md / mockup key maps lag the implementation in a few places (see MANUAL.md, which is accurate).
Rule from the user: subagents run on Opus (Sonnet for trivial tasks), never Fable.
