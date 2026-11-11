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
* Sample rate is a runtime value passed to `init` (44100 default; 32000 must also work).
  Derive every time constant and frequency from it.
* All state must be safe against NaN/inf: clamp filter cutoffs below Nyquist, clamp
  feedback/resonance to stable ranges at every parameter extreme. A test must sweep
  every param to min and max and assert the output stays finite and bounded.
* Every user parameter is a `uint8_t` described by a `ParamInfo`. Put musically useful
  curves behind them (exponential for frequency and time), so that every position of
  every knob produces a usable sound and defaults sound good with zero tweaking.

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
