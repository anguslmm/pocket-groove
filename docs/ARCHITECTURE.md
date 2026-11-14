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
