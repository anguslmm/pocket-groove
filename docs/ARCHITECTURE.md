# Pocket Groove — architecture and ground rules

A house/techno groovebox for the M5Stack Cardputer-Adv (ESP32-S3, 240 MHz dual core,
single-precision FPU, ~300 KB usable RAM, no PSRAM assumed, 240x135 screen, 56 keys,
ES8311 codec to a 3.5 mm jack). Target listening: headphones / line out.

8 tracks: Kick, Snare, Clap, Closed hat, Open hat, Perc (synthesised drums), Bass (mono
acid / bass-house synth), Lead (4-voice poly for stabs and chords). 16 patterns of up to
64 steps per track, parameter locks, probability, ratchets, swing, song chain. Kick-driven
sidechain ducker, tempo-synced delay, reverb, master DJ filter and drive.

## Layout

    lib/pg/src/pg/*.h          CONTRACT headers (config, project, params, voice, engine, platform).
                               Read them first. Do not edit them; if one is wrong or
                               insufficient, say so in your final report with the exact change you want.
    lib/pg/src/pg/<component>/ portable C++17 implementation, one directory per component:
        dsp/      shared DSP building blocks (header-only where possible)
        drums/    six drum voices
        bass/     bass voice
        lead/     lead voice
        engine/   sequencer, command queue, mixer, ducker, send FX, master bus
        ui/       screens and key handling, draws into pg::Framebuffer
        io/       project (de)serialisation over pg::FileSystem
        tape/     the tape: four audio tracks beside the patterns ("The tape" below)
    src/                       firmware only: Arduino/M5 glue (audio out, keys, display, storage)
    host/                      desktop build: tests, WAV renderers, UI screenshot tool
    docs/                      hardware notes, UX spec, manual
    build/                     all build output (never commit)

Host build (each component is its own static lib; a test named
`host/tests/test_<component>_<what>.cpp` links only against `pg_<component>`;
anything in `host/tools/*.cpp` links against all components):

    cmake -S host -B build/host && cmake --build build/host -j4 && ctest --test-dir build/host --output-on-failure

Several agents work in this tree at once. Stay inside the directories you were given,
use your own build directory (`build/<yourname>`), and don't "fix" files you don't own.

## Rules for portable code (lib/pg)

* C++17, no exceptions, no RTTI, no iostream, no heap in the audio path (static or
  member storage only), no Arduino/ESP headers. Must build clean with
  `-Wall -Wextra -Wdouble-promotion -Wfloat-conversion`.
* **float only.** The ESP32-S3 FPU is single precision; any double is software-emulated.
  Write `0.5f`, use `sinf/expf/powf` (and only at control rate), never `std::sin(x)` on a double.
* Audio is rendered in quanta of `kQuantum` = 16 frames. Parameters, envelopes' segment
  changes, filter coefficients and note events update at most once per quantum; inner
  loops over the 16 frames should be tight and branch-light.
* Sample rate is a runtime value passed to `init` (44100 default; 32000 and 48000 must
  also work: "Sample rates" below). Derive every time constant and frequency from it.
* All state must be safe against NaN/inf: clamp filter cutoffs below Nyquist, clamp
  feedback/resonance to stable ranges at every parameter extreme. A test must sweep
  every param to min and max and assert the output stays finite and bounded.
* Every user parameter is a `uint8_t` described by a `ParamInfo`. Put musically useful
  curves behind them (exponential for frequency and time), so that every position of
  every knob produces a usable sound and defaults sound good with zero tweaking.

## Sample rates

The core runs at whatever rate `Engine::init` / `Instrument::init` is given. Three are
tested: **44.1 kHz** (the Cardputer, the reference: `itest_golden`), **32 kHz** (the
Cardputer's USB fallback) and **48 kHz** (the Daisy Seed, which has no 44.1:
`itest_rate48`, `itest_rate48_record`, `itest_golden48`, ctest `macro_audit_48k`).
44.1 and 32 kHz are bit-identical to what they were before 48 kHz was added.

What depends on the rate, and how:

| Part | How it follows the rate |
|---|---|
| Voices (bass, lead, drums, both samplers) | Every frequency and time constant from `init(fs)`: phase increments, filter coefficients, envelope coefficients (per quantum: `fs / kQuantum`), fades, LFOs. Measured at 48 kHz: pitch within 0.8 cents of the note, 48 vs 44.1 within 0.01 cents (synths) / 6 cents (drum attacks); envelope and decay times within 0.6 % (the hats, noise: 1.8 %). |
| Lead wavetables | One table set per Nyquist: `tablesFor(fs)` gives the 44.1 kHz set at fs >= 44100 (also at 48 kHz: band-limited to 0.49 x 44.1 kHz = 21.6 kHz, so nothing aliases at 48; the 21.6 .. 23.5 kHz partials the 48 kHz Nyquist would allow are left out, inaudible). The 32 kHz set below. No 48 kHz set: 0 bytes of flash. |
| Sequencer | Step length `fs * 15 / BPM` in Q24 samples, events on quantum edges: a bar line is within one quantum (0.33 ms at 48 kHz) of the exact grid and never drifts; swing in the same units. |
| Control rate | Quanta of 16 frames: 0.33 ms at 48 kHz, 0.36 at 44.1. The macro control rate (`kFineQuanta`, `kFineEvalQuanta`) is in quanta, so it is 8 % faster at 48 kHz (1.33 / 10.7 ms instead of 1.45 / 11.6): the interpolation between evaluations is finer, nothing else changes. Times in ms (`kMacroRampMs`, `kMacroSettleMs`, SetMacroOffset's ramp, the ducker, the 4 ms mixer smoothing) are converted with the rate. The explicit ramp of `setMacroCommand(..., rampQuanta)` is in quanta (only tests and tools pass one). |
| Delay | Runs at fs / 2. Its line is 0.75 s at every rate up to `PG_MAX_SAMPLE_RATE` (config.h; default 48000: 18,014 half-rate samples = 36 KB; the Cardputer builds with 44100: 16,552 = 33 KB, exactly the old line, and at 32 kHz the same buffer gives 1.03 s as before). Tempo-synced times are exact; what is halved to fit is halved at the same tempos at 44.1 and 48. |
| Reverb | Runs at fs / 2; every line scales with fs / 44.1 kHz (each rounded to a prime) up to the pool's size for `PG_MAX_SAMPLE_RATE` (14,710 samples = 29.4 KB at 48 kHz, 13,452 = 26.9 KB at 44.1). The room is the same size in seconds at 32, 44.1 and 48 kHz (610.1 / 612.9 ms of lines at 44.1 / 48); RT60 comes from the line lengths in seconds. The modes sit on slightly different frequencies (prime rounding), measured decay to -30 dB 3.5 % shorter at 48. |
| Master, ducker, scope | From the rate at init (the scope's `ScopeInfo::sampleRate`, its decimation chosen from it). |
| Sample pool | A slot's header carries its rate (8 .. 96 kHz accepted); the samplers play a sample at header rate / engine rate, so a 22,050, 44,100 or 48,000 Hz sample (and the 22,050 Hz ROM sounds, `kRomRate`) plays at its natural pitch on an engine at any rate. Slice positions, the 10 ms minimum region and the sampler's DOWN (a hold rate in Hz) are in the sample's or the engine's own units. |
| Recording | `RecRates {in, out}` (recorder.h), a property of the platform and its pool: the half-band decimator runs only when out = in / 2. The Cardputer: 44,100 in, 22,050 stored (unchanged). A platform with sample memory to spare stores at its input rate (the Daisy: 48,000 in and stored, 64 MB = ~11 min mono); `recRatesFor(in, poolRate)` picks: half when that is the pool's rate, else the input rate. Settle, window, pre-roll, tail and the meter are in ms. Resampling uses the same rates: at 48 -> 48 a bar of N frames is N samples, the engine's own mono output bit for bit. |

What is audibly different at 48 kHz (measured on the host; nobody has listened):
the hats' six-square metal bank is not band-limited, so its aliases fold to other
frequencies (their strongest peaks move by up to 34 cents; centroid +1 %, level
-0.3 dB in the demos); noise sources (snare, clap, hats, shaker) gain the 22 .. 24 kHz
band (centroids +3 .. 6 %, inaudible content). The demos' mixes are within 0.1 dB and
2 % of centroid of 44.1 kHz (`demo_compare --rates 44100 48000 --song`); every built-in
instrument's mean loudness over its macro space is within 0.6 dB (`macro_audit --rate`).

Buffers that grow with `PG_MAX_SAMPLE_RATE` (engine state, `.bss`): delay line 33.1 ->
36.1 KB, reverb pool 26.9 -> 29.4 KB (79.5 KB of engine state on the host at 48000).
Above `PG_MAX_SAMPLE_RATE` the engine still runs, with a shorter delay line and reverb.

Program placement (`table_attr.h`): `PG_TABLE_ATTR` marks the hot DSP tables (lead
wavetables, bass sine and BLEP), `PG_BULK_ATTR` the big cold data (the ROM sounds, the
built-in instrument library). Both empty by default; a platform defines them as section
attributes to place the data (docs/STATE.md lists the sizes, measured on Cortex-M7).

## Output formats

The engine renders one of two formats; everything up to the last step of the master bus
(mix, ducker, send FX, rumble high-pass, DJ filter, drive, volume, soft limiter) is the same
code, inlined into each entry point (`engine/master.cpp` `MasterBus::chain`, `engine.cpp`
`State::renderT`, `Engine::processT`):

| Entry point | Format | Who uses it |
|---|---|---|
| `Engine::process(int16_t*, frames)` / `Instrument::process` | interleaved int16: the limiter's output x 32767, rounded half up | the Cardputer (its codec takes 16 bits), the desktop app (SDL s16; its recording ring, Listen mix and WAVs are int16, so it stays there), every golden test |
| `Engine::processFloat(float*, frames)` / `Instrument::processFloat` | interleaved float, +-1.0 full scale, the limiter's output as it is | the Daisy (`daisy/app` `App::render`: the output level in float, then libDaisy's 24-bit conversion) |

The two differ only by that rounding: for every sample, |float x 32767 - int16| <= 0.5,
and `floatToInt16()` of the float output is the int16 output bit for bit. The float output
never leaves [-1, 1] (the limiter's ceiling is exactly 1.0). The suspend fade (a few ms
after `suspend()`) is computed per format and may differ by one int16 step. The output tap
(resampling) always observes int16, the same samples in both modes; recording and the
sample pool stay 16-bit. `itest_float_out` checks all of this over the three demo songs, a
hot pattern in the limiter (stereo and mono, drive and volume moving) and a reverb tail.

A build links only the entry points it calls: the Cardputer's firmware has no float render
(flash -12 bytes, static RAM unchanged, `tools/pio_audio_ram.py`'s placement check passes
unchanged), the Daisy's no int16 render. `itest_golden` and `itest_golden48` are unchanged.

What the int16 step costs, measured (`itest_float_out`, a reverb tail at 48 kHz after Stop):
its error is -101.1 dBFS RMS (one LSB / sqrt(12)), white across the band; the tail's
signal-to-error ratio is 47 dB at -54 dBFS, 28 dB at -73, 8 dB at -94, and the int16
output is all zeros 4.5 s after Stop while the float tail lasts to 6.1 s. A 24-bit converter
fed the float output adds -143 dBFS RMS. Cost per frame, callgrind on the host (busy
pattern, stereo 48 kHz, x86 instructions per frame): int16 2130 before the change, 2123
after, float 2108 (no rounding step); the Daisy also drops its int16 -> float pass.

## The tape

`lib/pg/src/pg/tape/` (component `pg_tape`; the contract is `tape.h`'s header comment): four
mono audio tracks the engine mixes and records, beside the patterns. Compiled out with
`PG_TAPE=0` (config.h; the Cardputer's envs and the Daisy's Makefile set it): then the
engine, the UI and the files carry none of it, and `itest_golden_notape` builds the whole
core that way and renders the golden demos against it.

**In the engine.** `Engine::setTape(tape::Tape*)` (before audio starts or suspended). Per
quantum, only while the tape is active (it holds audio and the transport rolls, or a take is
armed or running; otherwise one load and a flag): the instrument tracks mix as before; for a
TRACK take the selected track's contribution to the bus is recorded (the bus before and after
that track: post level, pan, mute and ducking, before the sends); then `Tape::mix` adds the
tape tracks to the bus and to the delay and reverb sends with the instrument tracks' laws
(level 100 = unity cubic, constant-power pan, sends `(v/127)^2`, 4 ms smoothing); the send
effects run; `Tape::post` records MIX there (the master bus's input: tracks, effect returns,
other tape tracks; the armed track is added after the capture and without sends so it cannot
feed itself back), writes the take, and the master bus runs on it all. With no tape, or an
empty one recording nothing, the output is bit for bit what it was (`itest_tape`, int16 and
float). The sequencer tells the tape about every master step (`masterStep`: bar lines, the
steps to the next one, the song position at a song pattern boundary, and `gridCd`, how far the
bar's exact time lies from the quantum's first frame), Play / Stop, the tempo, and starts the
transport after a take's count-in (`countInDone`).

**Position.** In samples at the engine's rate. At each bar line the true address of the
quantum's first frame is the bar's start on tape (the steps played times a step at the tape's
tempo, Q32 as the sequencer counts) minus `gridCd`; between bar lines one sample per frame. At
the tape's own tempo that is exactly where the tape already was, so it never jumps, and a take
lands on the tape where the exact grid puts it: a bar line's impulse on the bar's address. The
sequencer fires notes on quantum edges up to half a quantum off that grid, so a live note and
its copy on tape coincide sample for sample when the phases match (a bar of whole quanta, 120
BPM at 48 kHz) and are within one quantum (0.33 ms) otherwise. Pattern mode: the tape loops
over `loopBars` bar lines (the address is taken modulo the loop's length); song mode: the
address is the song position (re-derived at each song pattern boundary; a hand-queued detour
just runs on). At another tempo than the tape's, each bar line moves the address: playback
goes over to the new place at once (a downbeat stays whole) while the old place fades out over
256 frames, a bar longer than its stretch of tape fades out 64 frames before its end and is
silent to the bar line, and a take writes no further than its bar's end.

**Storage.** One block of memory from the platform (`Tape::init(ptr, bytes, rate)`: on the
desktop the heap, on the Daisy SDRAM in phase 2); the core carves its tables from it and
allocates nothing. Blocks of `kBlock` = 16,384 samples (32 KB: 0.34 s at 48 kHz) and 98 bytes
of bookkeeping each (the four tracks' table entries, 64 overview peaks, the free, quarantine,
take and undo lists): 5.78 MB per minute of track time at 48 kHz, 5.31 at 44.1. A track's table
has one 32-bit entry per block of its address space, a base and a take block id (atomic: the
audio thread reads both at once); silence has no block. Takes write into take blocks only
(zeroed blocks the main thread keeps in a 64-deep ready ring), so the audio thread never
copies; playback reads base + take through the overdub guard (identity to 75 % of full
scale, a soft knee above, never a wrap); replace writes input - base. After the take the main
thread (`Tape::service`, every UI frame) merges each take block into a new block and swaps it
in; the replaced bases are the undo (undo and redo swap them back and forth); blocks taken out
of use go to a quarantine and are reused once the audio thread has finished two more quanta.
Running out of ready blocks ends a take cleanly (`MEMORY FULL`). The int16 samples hold the
bus at half scale (`kHeadroom` 2: a full-scale tape sample is +6 dB over the master's 1.0).

**Threads.** UI edits go through the engine's queue (`CmdType::Tape`, `tape::tapeCommand`);
the audio thread publishes the view (state, address, read head, count-in, take length) in
atomics the UI reads to draw; the settings are atomics the audio thread writes from commands
(loading writes them directly with the transport stopped). The main thread owns the block
pool, the merge, undo, erase, clear and the files; it touches the tables only with atomic
stores and only while no take runs. The line input (LINE) is a ring the platform's capture
thread fills (`Tape::lineIn`) and the audio thread drains at a steady fill; the take writes
each quantum's input at the address the tape had a round trip earlier (a history of 1,024
quanta), and a take's end waits that long for the input still on its way.

**Tricks.** The read head is an offset from the true address with a speed and an
acceleration (Q32): BRAKE ramps the speed to 0 over 0.6 s and fades the level with it, its
release starts a spin-up ahead of the true address by as much as the spin-up will lag, so it
lands on it; HALF and REVERSE run at -0.5 and -2 samples per frame relative to the tape;
releases cross-fade (256 frames) back onto the true address. Transitions wait for a running
cross-fade to finish (at most 5.8 ms). Only playback moves; recording and the bars do not.

**The count-in click.** A take from stop counts in one bar; the tape clicks on its four beats
(a 30 ms sine burst, 1760 Hz on beat 1, 880 Hz on the others). It is a MONITOR signal: the
engine never mixes it into what `process()` writes, so nothing that takes the output has it
(the tape's sources, which are all inside the engine; the output tap and so resampling; the
platform's recording of the output). The platform adds it after them: `Engine::monitor(out,
frames)` right after `process()` on the same buffer, on the audio thread (the desktop: after
the F9 ring, before SDL plays it). The engine counts the frames its `process()` calls write;
`countInDone(frame)` (every quantum) notes the count-in's first frame and where it ended (Play,
Stop, a cancel), and `Tape::clickInto` adds the bursts that fall in the buffer, from those
numbers alone: no state per sample, nothing allocated, a compare outside a count-in. The last
burst ends at least 0.27 s before the bar line (a beat at 200 BPM is 0.3 s), so LINE, which writes the
input a round trip back from the take's start, does not record the speakers' click either
(`itest_tape` loops the heard output into LINE). Compiled out with the tape.

**Files** (`tape_io.h`): `<mount>/pg/NAME/tape.txt` and `track1.wav` .. `track4.wav` beside
`NAME.pgv`, written and read incrementally through `FileSystem`'s streaming calls (the UI runs
256 K samples a frame on the desktop), each WAV written to a `.tmp` and renamed. A save can
be limited to some tracks. The session keeps a tape too, in `/int/pg/_last/` beside
`_last.pgv` (`tape::kSessionTape`; no project can have that name): the UI writes it from the
autosave (only `tape.txt` and the tracks whose `Tape::trackRev` moved since they were last
written there, a piece per frame; a take, an erase, an undo or a project's load or save stops
the job, the tracks it finished count), and `Ui::tapeQuit()` at quit (ends a take, which is
kept, then writes what changed); power-on loads it instead of the named project's folder.

**Cost** (callgrind, x86 instructions per frame, the busy pattern stereo at 48 kHz:
`host/tools/bench_tape`): no tape 2101, an empty tape 2107, four tracks playing 2236 (+135),
and one of them recording MIX over itself 2310 (+209), four playing with HALF held 2608
(+507: per-sample interpolated reads). On the Daisy (1.0..1.6 cycles per instruction at 480
MHz) four tracks playing are an ESTIMATED 1.4..2.2 % of the core, a take 2.1..3.3 %, a trick
held 5..8 %.

## CPU budget (per sample at 44.1 kHz, ESP32-S3 @ 240 MHz = ~5400 cycles)

The whole engine must stay under ~55% of one core. Rough allowances, all voices sounding:
six drums together 600 cycles/sample, bass 500, lead (4 voices) 1100, mixer + ducker 100,
delay 80, reverb 450, master 150. Treat a float multiply-add as ~4-6 cycles, a table
lookup with interpolation ~15, a divide ~40, a libm call 300-1500. Skip silent voices.
Host tests should include a benchmark (ns per sample on this x86 box) so regressions
are visible; x86 is roughly 15-25x faster than the target for this kind of code.

## Sound target

Danceable house and techno through headphones: a kick with weight and click that holds
the low end, bass that can do both squelchy acid lines (slide + accent + resonance) and
modern bass-house growls (drive, wavefolding, tempo-synced filter movement), bright
tight hats, a clap with body, and chord stabs with width. The sidechain ducker should
give the classic pumping feel with one parameter turned up.
