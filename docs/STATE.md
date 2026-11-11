# Work state (paused 2026-11-11 02:12 PST, resume 05:20 PST)

Done and tested on host: drums, bass, lead, engine (contract v2, mono device mode), instrument,
UI (phases 1+2, all 32 screens), project storage (pg::io), analysis tool (tools/ears.py),
platform layer + reviewed hardware smoke test (build/firmware/pocket-groove-smoke.bin; user
has it, results from real hardware not in yet).

Stopped mid-task (all were Opus subagents; resume them, or start fresh ones from the files):
1. Optimiser: lookup tables moved to flash (done, tests passing, bit-identity of renders and
   RAM table not yet verified). NOT yet done: half-rate delay line, CPU cuts (-25% target),
   suspend() flushing the command queue, final RAM/CPU tables. Target: instrument static RAM <= 140 KB.
2. Composer: three demos in lib/pg/src/pg/demo (ACIDJAM, BASSHAUS, WAREHOUS) drafted; was
   checking flat-tops at -6.7 dBFS in WAREHOUS (suspected driven-kick clipper) and solo levels.
   NOT yet done: final verification, itest_demo, default-mix report.
3. Firmware app (src/app, platformio.ini cardputer-adv env now compiles lib/pg): builds
   warning-free; was starting the RAM breakdown. NOT yet verified complete: storage streaming,
   G0 volume, diagnostics strip, boot menu, BRINGUP "Flashing the instrument", pocket-groove.bin.

Then: rebuild everything, user manual (docs/MANUAL.md), send firmware + demo renders to user.
Rule from the user: subagents run on Opus (Sonnet for trivial tasks), never Fable.
