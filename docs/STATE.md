# Work state (2026-11-11)

v0.1 complete and host-verified: 45/45 host tests pass; both firmware envs build clean.
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
Projected RAM: free 55 KB, minimum-ever 27 KB (default 66 / 38 KB). Waiting for the hardware
checklist in docs/usb-audio.md section 10; then decide whether it becomes the default.

Agreed order of work (2026-11-12, set by the owner):
1. Owner tests on hardware what is on main: backlight/ghost fixes, tools/pg ls/pull/bounce,
   env:usb-audio-spike (done), env:cardputer-adv-usb (docs/usb-audio.md section 10). Fix what
   that turns up.
2. Desktop build of the instrument for macOS: same core and UI in a window (SDL2), Mac
   keyboard as the Cardputer keys, stereo audio out, projects as files, push/pull to the device.
   Faithful to the device first; desktop-only extras only where the owner feels the pinch.
   Nice to have: MIDI controller input for playing notes. Open design question: what a MIDI
   key does on the lead in a chord mode, and how played chords fit the one-note-plus-chord-type
   Step without changing the project format.
3. Full functionality assessment: the owner tests every instrument and setting (on the
   desktop build, faster) and gives feedback.

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
