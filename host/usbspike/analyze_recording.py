#!/usr/bin/env python3
"""Check a recording of the USB audio spike's test signal (docs/usb-audio.md, test step 6).

    afconvert -f WAVE -d LEI16 recording.m4a recording.wav     # macOS: QuickTime's file to PCM WAV
    python3 host/usbspike/analyze_recording.py recording.wav

The spike sends: left 440 Hz, right 660 Hz, both sines at -12 dBFS peak, plus a
2 ms click on both channels every `rate` frames (one second of the device's I2S
clock). From the recording this prints:

  * per channel: dominant frequency and RMS level (expect 440 / 660 Hz, about -15 dBFS RMS)
    -> wrong frequencies = wrong sample rate on one side; swapped = channel order.
  * click spacing in samples (expect exactly the file's sample rate if nothing was lost
    and the recorder did not resample; +-2..3 samples of jitter after AAC encoding)
    -> a short interval = frames lost (underrun on the host side or dropped packets),
       a long one = frames duplicated / silence inserted; a steady offset of N ppm =
       the recorder resampled (device clock vs. Mac clock), which is normal for some apps.
  * discontinuities in the sines outside the clicks (dropouts, glitches) and silent gaps.

Needs numpy (python3 -m pip install numpy). Reads 16/24/32-bit PCM WAV.
"""
import sys
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        sr, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float64) / float(1 << 23)
    elif sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / float(1 << 31)
    else:
        sys.exit("unsupported sample width %d" % sw)
    return sr, x.reshape(-1, ch)


def dominant_hz(x, sr):
    n = 1 << int(np.floor(np.log2(min(len(x), sr * 8))))
    seg = x[:n] * np.hanning(n)
    spec = np.abs(np.fft.rfft(seg))
    k = int(np.argmax(spec[1:]) + 1)
    if 1 <= k < len(spec) - 1:   # parabolic interpolation
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        k = k + 0.5 * (a - c) / (a - 2 * b + c)
    return k * sr / n


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sr, x = read_wav(sys.argv[1])
    print("file: %d Hz, %d channel(s), %.1f s" % (sr, x.shape[1], len(x) / sr))
    if x.shape[1] < 2:
        print("FAIL: mono recording -- the device is stereo; pick 2 channels in the recorder")
        x = np.repeat(x, 2, axis=1)
    L, R = x[:, 0], x[:, 1]
    problems = 0

    # --- clicks: large first differences on L+R (the sines' max slope is ~0.02)
    m = 0.5 * (L + R)
    d = np.abs(np.diff(m))
    hits = np.flatnonzero(d > 0.3)
    onsets = []
    for h in hits:
        if not onsets or h - onsets[-1] > sr // 50:
            onsets.append(int(h))
    # a click cut by the start or end of the file has a shifted onset: drop it
    onsets = np.array([o for o in onsets if sr // 100 <= o < len(m) - sr // 100])
    print("clicks found: %d" % len(onsets))
    if len(onsets) >= 3:
        iv = np.diff(onsets)
        bad = np.flatnonzero(np.abs(iv - np.median(iv)) > 3)
        ppm = (np.mean(iv) / sr - 1.0) * 1e6
        print("click spacing: median %d, min %d, max %d samples (file rate %d); mean %+.0f ppm vs. file rate"
              % (np.median(iv), iv.min(), iv.max(), sr, ppm))
        if abs(np.median(iv) - sr) > 0.002 * sr:
            print("  NOTE: spacing is not ~1 s of the file's rate: the recorder resampled or the device runs another rate")
        for i in bad[:20]:
            print("  irregular interval at %.3f s: %d samples (%+d)" % (onsets[i] / sr, iv[i], iv[i] - int(np.median(iv))))
        if len(bad):
            print("FAIL: %d irregular click interval(s): frames lost or inserted" % len(bad))
            problems += 1
        else:
            print("ok   every click interval within 3 samples of the median")
    else:
        print("FAIL: fewer than 3 clicks: no signal, or the recording is shorter than 3 s")
        problems += 1

    # --- tones and levels, with the click windows cut out
    mask = np.ones(len(x), dtype=bool)
    for o in hits:   # every click sample, including ones cut by the file edges
        mask[max(0, o - sr // 200): o + sr // 100] = False
    for name, ch, want in (("left", L, 440.0), ("right", R, 660.0)):
        f = dominant_hz(ch[mask], sr)
        rms = np.sqrt(np.mean(ch[mask] ** 2))
        db = 20 * np.log10(rms + 1e-12)
        ok = abs(f - want) < 2.0 and -18.0 < db < -12.0
        print("%s %s: %.1f Hz (want %.0f), %.1f dBFS RMS (want about -15)" % ("ok  " if ok else "FAIL", name, f, want, db))
        problems += 0 if ok else 1

    # --- discontinuities: the second difference of a clean sine is tiny (A w^2 < 0.003)
    for name, ch in (("left", L), ("right", R)):
        d2 = np.abs(np.diff(ch, 2))
        ev = np.flatnonzero((d2 > 0.03) & mask[1:-1])
        groups = []
        for e in ev:
            if not groups or e - groups[-1] > sr // 100:
                groups.append(int(e))
        silent = np.flatnonzero(np.convolve((np.abs(ch) < 1e-4).astype(np.int32), np.ones(sr // 1000, dtype=np.int32), "valid")
                                == sr // 1000)
        gaps = 0 if len(silent) == 0 else 1 + int(np.sum(np.diff(silent) > 1))
        if groups or gaps:
            problems += 1
            print("FAIL %s: %d glitch(es) in the sine, %d silent gap(s) of 1 ms or more; first at %s"
                  % (name, len(groups), gaps, ", ".join("%.3f s" % (g / sr) for g in groups[:8])))
        else:
            print("ok   %s: no glitches or silent gaps outside the clicks" % name)

    print("RESULT: %s" % ("PASS" if problems == 0 else "%d problem(s)" % problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
