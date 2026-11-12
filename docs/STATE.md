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
2. NOW: the owner's full pass through every instrument and setting on the desktop build, using
   docs/ASSESSMENT.md. In parallel: env:hw-spike (tilt sensor and mic sampling to/from flash)
   to learn what the hardware can do before designing around it.
3. THEN: the macro redesign (see Design direction). The owner will make a song only AFTER the
   redesign: the goal is a fun experience that is not hindered by the tiny device.
4. LATER: a design sprint on the flow between desktop and device, giving each its own UX.
ON HOLD, do not build: MIDI controller input. The owner is not convinced they want it (it
distracts from the intended flow) and it needs more design than a quick proposal.
No further desktop-specific features for now beyond the strip, Listen and Stems.

Design direction (owner, 2026-11-12; brainstorm only, nothing built yet): capture the
playfulness of the OP-1 without its feature set. Sounds are shaped through a few designed
macro controls with a reactive picture, not through bare parameters; the bare parameters are
NOT exposed to the player ("be bold and confident in our choices"). Tilt (the IMU) as a
control appeals. Candidates: characters per voice, dice/mutate/morph, punch-in effects,
mic sampling (try samples in flash on the Cardputer before considering a bigger device).
The macros get designed from the owner's full pass through every sound on the desktop build.

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
