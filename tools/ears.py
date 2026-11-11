#!/usr/bin/env python3
"""ears.py - "listen" to a WAV file with numbers and pictures.

For reviewers who can read text and look at PNGs but cannot hear. Give it a
16-bit PCM WAV (mono/stereo, 32 or 44.1 kHz, 0.1 s .. a few minutes).

USAGE
  ears.py FILE.wav [--out DIR] [--bpm N] [--start S --dur S] [--title T]
        writes DIR/FILE.json + DIR/FILE.png, prints a text summary (DIR
        defaults to the WAV's folder). --bpm enables 16th-grid timing stats
        (otherwise the estimated tempo is used). --pitch forces note analysis.
  ears.py FILE.wav --zoom T0 T1      detailed waveform + delta + spectrogram of
        T0..T1 seconds -> FILE.zoom_T0-T1.png (one drum hit, one click...)
  ears.py --compare A.wav B.wav      side-by-side metrics, B-A differences and
        overlaid average spectra -> A_vs_B.png / .json

HOW TO READ THE SUMMARY (healthy values for a dance-music master in brackets)
  peak      sample peak and 4x-oversampled true peak [<= -1 dBFS / <= -0.5 dBTP;
            true peak above 0 = inter-sample overs]
  clipping  samples at full scale, within 0.1 dB of it, and "flat runs": >= 3
            consecutive samples within +-2 LSB, above half the channel peak,
            entered AND left through a corner [all 0]. Many runs at one level
            below 0 dBFS = a clamp / clipper inside the signal chain (the
            output gain came after it). Naive square/pulse waves also produce
            flat runs - judge by level and the waveform (--zoom).
  level     RMS; crest = peak - RMS [mix 8..14 dB; < 6 squashed/clipped;
            a single drum hit 15..25]. DC as a fraction of full scale [|DC| <
            0.002].
  loudness  BS.1770 integrated LUFS [-16..-8 for a finished loop; device
            headroom renders around -20 are fine]; short-term (3 s) min/max,
            momentary (0.4 s) max, LRA [1..6 LU for a loop]. A mono file is
            measured as one channel: it reads 3 LU lower than dual mono.
  bands     energy per band, dB relative to total (20 Hz..Nyquist). Typical
            house/techno: sub -12..-4, bass -6..-1, low-mid -14..-6,
            mid -20..-10, presence -28..-14, air -40..-20. presence/air above
            ~-10 = harsh; sub+bass below ~-12 = thin; air below -50 = dull.
  spectrum  centroid [0.3..2.5 kHz for a full mix] and 85 % rolloff.
  stereo    corr: +1 mono, 0 unrelated, -1 anti-phase [> +0.3; < 0 loses level
            in mono]; low-band (<150 Hz) corr [> +0.9: bass must be mono];
            side/mid [-25..-6 dB; > 0 = mostly out of phase]; balance L-R
            [within +-1.5 dB]. mono sum = level change of (L+R)/2 vs L/R:
            0 dual mono, -3 uncorrelated, < -6 = phase cancellation
            [-0.5..-2 dB for a mix; low band ~0].
  clicks    isolated/cutoff = audible glitches [0]. "hard attacks" are step
            jumps where the level rises afterwards (fine on drums, a bug on a
            pad). "recurring edges" are repeated similar jumps = waveform
            edges of naive saw/square/bit-crush, listed with their rate; a
            rate equal to the audio block rate rather than the note = glitch.
  onsets    count, estimated tempo; "kick-like" = onsets with a low-band hit.
  grid      onset deviation from the 16th grid: offset = constant latency of
            the whole file (harmless), jitter sd / MAD [< 2 ms from a
            sequencer], drift [~0 ms/min], swing = late off-16ths.
  pumping   LF mod depth = low-band (<150 Hz) envelope modulation at the beat
            rate, 0 flat .. 1+ [kick over silence 1..1.7; kick + sustained
            bass 0.2..0.4; ducking adds only ~0.05..0.1 because the kick
            itself dominates - compare against an un-ducked render with
            --compare, or read the duck dips]. kick punch = low-band
            peak above its pre-kick level [> 6 dB = the kick hits]. duck dip =
            how far each band falls below its pre-kick level after the kick
            [0..1 dB none; 3..12 dB sidechain pumping]. A long kick tail can
            fill the low-band hole, then only the rest band shows the dip.
  pitch     (single notes <= 8 s, or --pitch) fundamental, note, cents [+-5];
            THD = harmonics vs fundamental; inharmonic = energy NOT at
            harmonics vs energy at harmonics [clean oscillator < -60 dB;
            naive saw/square -35..-15 dB = aliasing; noise also counts].
            Measured on the first note only. "percussive" = level falls
            > 15 dB across the window: drum modes (toms, cowbells, claves)
            are legitimately inharmonic, so only sustained notes are flagged.
  flags     the heuristics' verdicts; "none" is not proof of a good sound.

THE PNG  waveform (red = clip, black triangle = click), multi-resolution
  log-frequency spectrogram, low-band vs rest envelope (ducking), loudness,
  L/R correlation, key numbers, average spectrum (or the note's harmonic
  spectrum), goniometer (vertical = mono, horizontal = anti-phase) and the
  beat-folded envelope (one beat averaged: kick peak at 0, then the dip).
"""

import argparse
import json
import math
import os
import sys
import wave

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view
from scipy import fft as sfft
from scipy import ndimage
from scipy import signal

BANDS = [("sub", 20, 60), ("bass", 60, 250), ("low-mid", 250, 800),
         ("mid", 800, 2500), ("presence", 2500, 6000), ("air", 6000, 20000)]
LOW_SPLIT_HZ = 150.0
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
EPS = 1e-20

# colours (validated pairs: L/R, low/rest); red is reserved for problems
C_L, C_R = "#2a78d6", "#eb6834"
C_LOW, C_REST = "#6250d6", "#12946a"
C_BAD = "#d0312f"
C_INK, C_INK2, C_GRID = "#0b0b0b", "#52514e", "#d9d8d2"
C_SURF = "#fcfcfb"


# ----------------------------------------------------------------- helpers

def db20(v):
    return 20.0 * np.log10(np.maximum(np.abs(v), 1e-10))


def db10(v):
    return 10.0 * np.log10(np.maximum(v, EPS))


def fnum(v, fmt="{:.1f}", na="n/a"):
    if v is None:
        return na
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v) or v <= -199.0):
        return "-inf" if v < 0 else na
    return fmt.format(v)


def clean(o):
    """Make a structure JSON-safe (numpy -> python, nan/inf -> null)."""
    if isinstance(o, dict):
        return {str(k): clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v) for v in o]
    if isinstance(o, np.ndarray):
        return clean(o.tolist())
    if isinstance(o, (np.floating, float)):
        f = float(o)
        return f if math.isfinite(f) else None
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.bool_,)):
        return bool(o)
    return o


def rnd(v, n=3):
    if v is None:
        return None
    v = float(v)
    return round(v, n) if math.isfinite(v) else None


def read_wav(path, start=0.0, dur=None):
    """Return (x float64 [n, ch] in -1..1, xi int [n, ch] or None, sr, bits, total_s)."""
    xi = None
    try:
        with wave.open(path, "rb") as w:
            sr, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
            i0 = min(n, max(0, int(round(start * sr))))
            i1 = n if dur is None else min(n, i0 + int(round(dur * sr)))
            w.setpos(i0)
            raw = w.readframes(i1 - i0)
        bits = sw * 8
        if sw == 2:
            xi = np.frombuffer(raw, dtype="<i2").astype(np.int32)
        elif sw == 1:
            xi = np.frombuffer(raw, dtype=np.uint8).astype(np.int32) - 128
        elif sw == 3:
            b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            xi = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            xi = np.where(xi >= 1 << 23, xi - (1 << 24), xi)
        elif sw == 4:
            xi = np.frombuffer(raw, dtype="<i4").astype(np.int64)
        else:
            raise ValueError("unsupported sample width %d" % sw)
        xi = xi[: (len(xi) // ch) * ch].reshape(-1, ch)
        x = xi.astype(np.float64) / float(1 << (bits - 1))
        total = n / sr
    except wave.Error:
        from scipy.io import wavfile          # float WAVs etc.
        sr, data = wavfile.read(path)
        data = np.atleast_2d(data.T).T
        total = len(data) / sr
        i0 = max(0, int(round(start * sr)))
        i1 = len(data) if dur is None else min(len(data), i0 + int(round(dur * sr)))
        data = data[i0:i1]
        if np.issubdtype(data.dtype, np.integer):
            bits = data.dtype.itemsize * 8
            xi = data.astype(np.int64)
            x = data.astype(np.float64) / float(1 << (bits - 1))
        else:
            bits = 0
            x = data.astype(np.float64)
    if x.shape[1] > 2:
        x = x[:, :2]
        xi = xi[:, :2] if xi is not None else None
    return x, xi, int(sr), bits, total


# ------------------------------------------------------------ level / clip

def true_peak(x):
    """4x oversampled peak (BS.1770 Annex 2 style), per channel, linear."""
    out = []
    for c in range(x.shape[1]):
        up = signal.resample_poly(x[:, c].astype(np.float32), 4, 1)
        out.append(max(float(np.max(np.abs(up))) if len(up) else 0.0,
                       float(np.max(np.abs(x[:, c]))) if len(x) else 0.0))
    return out


def clipping_metrics(x, xi, bits, sr, t0):
    """Count full-scale samples and flat-topped runs.

    A flat run = >= 3 consecutive samples within +-2 LSB of each other, at
    >= 50 % of the channel peak, that are entered AND left through a corner:
    the mean slope over the 3 samples on each side must exceed 4x the slope a
    smooth (parabolic) peak of that width and flatness would have there
    (8 * tol / length). This catches hard clamps, soft clippers / limiters
    with +-1 LSB noise on the plateau and naive square waves, while the
    rounded top of a loud low sine or a coincidental repeated value inside a
    noisy waveform does not count. Runs of >= 2 samples at digital full
    scale always count.
    """
    n, ch = x.shape
    lsb = 1.0 / float(1 << (bits - 1)) if bits else 1.0 / 32768.0
    tol = 2.0 * lsb
    fs_pos = 1.0 - lsb
    at_fs = int(np.sum((x >= fs_pos - lsb * 0.5) | (x <= -fs_pos + lsb * 0.5)))
    near = int(np.sum(np.abs(x) >= 10 ** (-0.1 / 20)))
    runs, clip_t = [], []
    for c in range(ch):
        v = x[:, c]
        pk = float(np.max(np.abs(v))) if n else 0.0
        fs_idx = np.flatnonzero(np.abs(v) >= fs_pos - lsb * 0.5)
        clip_t.append(fs_idx)
        if n < 8 or pk < 1e-3:
            continue
        dv = np.abs(np.diff(v))
        flat = (dv <= tol + 1e-12) & (np.abs(v[1:]) >= 0.5 * pk)
        if not flat.any():
            continue
        d = np.diff(np.concatenate(([0], flat.view(np.int8), [0])))
        s = np.flatnonzero(d == 1)             # run covers samples s .. e
        e = np.flatnonzero(d == -1)
        length = e - s + 1
        cs = np.concatenate(([0.0], np.cumsum(dv)))
        b0, a1 = np.maximum(s - 3, 0), np.minimum(e + 3, n - 1)
        slope_in = (cs[s] - cs[b0]) / np.maximum(s - b0, 1)
        slope_out = (cs[a1] - cs[e]) / np.maximum(a1 - e, 1)
        val = v[s]
        full = np.abs(val) >= fs_pos - lsb * 0.5
        cand = np.flatnonzero((length >= 3) | ((length >= 2) & full))
        ok = np.zeros(len(s), bool)
        for k in cand:                          # few candidates: plateau range check per run
            seg = v[s[k]:e[k] + 1]
            if full[k] and length[k] >= 2:
                ok[k] = True
                continue
            if np.ptp(seg) > 2 * tol + 1e-12 or np.abs(seg).min() < 0.5 * pk:
                continue
            smooth = 8.0 * tol / length[k]
            ok[k] = min(slope_in[k], slope_out[k]) > 4.0 * smooth
        for a, b, L, vv in zip(s[ok], e[ok], length[ok], val[ok]):
            runs.append((int(a), int(L), float(vv), c))
        if ok.any():
            clip_t.append(s[ok])
    runs.sort()
    lengths = [r[1] for r in runs]
    levels = [abs(r[2]) for r in runs]
    marks = np.unique(np.concatenate(clip_t)) if clip_t else np.zeros(0, int)
    m = {
        "samples_at_full_scale": at_fs,
        "samples_within_0p1dB": near,
        "flat_runs": len(runs),
        "flat_run_longest_samples": int(max(lengths)) if runs else 0,
        "flat_run_total_samples": int(sum(lengths)),
        "flat_run_level_dbfs": rnd(db20(np.median(levels)), 2) if runs else None,
        "flat_run_times_s": [rnd(t0 + r[0] / sr, 4) for r in runs[:12]],
    }
    # A clamp / clipper flattens every overshoot at the SAME level; plateaus
    # found by chance in noisy material land at scattered levels.
    same = 0.0
    if runs:
        lv = db20(np.array(levels))
        same = float(np.mean(np.abs(lv - np.median(lv)) <= 0.05))
    m["flat_run_same_level_fraction"] = rnd(same, 2)
    m["flat_tops_likely"] = bool(m["flat_runs"] and (
        m["flat_run_longest_samples"] >= 8 or (len({r[0] for r in runs}) >= 4 and same >= 0.5)))
    return m, marks


# ----------------------------------------------------------------- loudness

def k_weighting_sos(sr):
    """BS.1770 K-weighting (shelf + RLB high-pass) for any sample rate.

    Coefficients are re-derived from the analogue prototype (the same way
    libebur128 does); at 48 kHz they reproduce the table in the standard.
    """
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi * f0 / sr)
    Vh = 10 ** (G / 20)
    Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    s1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
          1.0, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / sr)
    a0 = 1 + K / Q + K * K
    s2 = [1.0, -2.0, 1.0, 1.0, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    return np.array([s1, s2])


def loudness_metrics(x, sr, t0):
    n, ch = x.shape
    y = signal.sosfilt(k_weighting_sos(sr), x, axis=0)
    p = np.sum(y * y, axis=1)                       # channel weights are 1.0 for L/R
    cs = np.concatenate(([0.0], np.cumsum(p)))

    def series(win_s, hop_s):
        w, h = int(round(win_s * sr)), max(1, int(round(hop_s * sr)))
        if n < w:
            return np.zeros(0), np.zeros(0)
        st = np.arange(0, n - w + 1, h)
        ms = (cs[st + w] - cs[st]) / w
        return t0 + (st + w / 2) / sr, ms

    tm, pm = series(0.4, 0.1)
    ts, ps = series(3.0, 0.1)
    lk = lambda ms: -0.691 + db10(ms)
    approx = False
    if len(pm):
        g = pm[lk(pm) > -70.0]
        if len(g):
            rel = lk(np.mean(g)) - 10.0
            g2 = g[lk(g) > rel]
            integ = float(lk(np.mean(g2))) if len(g2) else float("-inf")
        else:
            integ = float("-inf")
    else:                                           # shorter than one 400 ms block
        integ = float(lk(np.mean(p))) if n else float("-inf")
        approx = True
    lra = None
    if len(ps) >= 5:
        g = ps[lk(ps) > -70.0]
        if len(g):
            g2 = lk(g[lk(g) > lk(np.mean(g)) - 20.0])
            if len(g2) >= 2:
                lra = float(np.percentile(g2, 95) - np.percentile(g2, 10))
    lm, ls = lk(pm), lk(ps)
    m = {
        "integrated_lufs": rnd(integ, 2),
        "integrated_is_ungated_estimate": approx,
        "momentary_max_lufs": rnd(np.max(lm), 2) if len(lm) else None,
        "short_term_max_lufs": rnd(np.max(ls), 2) if len(ls) else None,
        "short_term_min_lufs": rnd(np.min(ls), 2) if len(ls) else None,
        "loudness_range_lu": rnd(lra, 2),
    }
    if ch == 1:
        m["integrated_lufs_as_dual_mono"] = rnd(integ + 3.01, 2)
    return m, {"tm": tm, "lm": lm, "ts": ts, "ls": ls}


# ----------------------------------------------------------------- spectrum

def average_spectrum(x, sr):
    """Welch PSD averaged over channels (power), density scaling."""
    n = len(x)
    nper = int(min(n, 8192))
    nfft = max(8192, nper)
    f, P = signal.welch(x, sr, window="hann", nperseg=nper, noverlap=nper // 2,
                        nfft=nfft, axis=0, detrend="constant")
    return f, np.mean(P, axis=1)


def band_integrate(f, P, lo, hi):
    """Integral of PSD over [lo, hi] using a cumulative sum (energy preserving)."""
    df = f[1] - f[0]
    edges = np.concatenate(([0.0], f + df / 2))
    cum = np.concatenate(([0.0], np.cumsum(P * df)))
    return np.interp(hi, edges, cum) - np.interp(lo, edges, cum)


def smooth_spectrum(f, P, sr, frac=6, fmin=20.0):
    """Energy in sliding 1/frac-octave bands (dBFS per band), log-spaced centres."""
    fmax = min(20000.0, sr / 2 * 0.98)
    fc = np.geomspace(fmin, fmax, 220)
    k = 2 ** (0.5 / frac)
    e = band_integrate(f, P, fc / k, np.minimum(fc * k, sr / 2))
    return fc, db10(e)


def spectrum_metrics(f, P, sr):
    nyq = sr / 2
    total = band_integrate(f, P, 20.0, nyq)
    bands = {}
    for name, lo, hi in BANDS:
        e = band_integrate(f, P, lo, min(hi, nyq)) if lo < nyq else 0.0
        bands[name] = rnd(db10(e / max(total, EPS)), 2) if total > EPS else None
    sel = f >= 20.0
    ps, fs_ = P[sel], f[sel]
    tot = float(np.sum(ps))
    if tot <= EPS:
        return {"bands_db_rel_total": bands, "centroid_hz": None, "rolloff85_hz": None}
    cen = float(np.sum(fs_ * ps) / tot)
    roll = float(fs_[np.searchsorted(np.cumsum(ps), 0.85 * tot)])
    return {"bands_db_rel_total": bands, "centroid_hz": rnd(cen, 1), "rolloff85_hz": rnd(roll, 1)}


def log_band_matrix(edges, nbins, df, sel):
    """bands x bins overlap (Hz) between log bands and FFT bins; zero rows outside sel."""
    lo = np.arange(nbins) * df - df / 2
    hi = lo + df
    W = np.minimum(edges[1:, None], hi[None, :]) - np.maximum(edges[:-1, None], lo[None, :])
    W = np.clip(W, 0.0, None)
    W[~sel, :] = 0.0
    return W.astype(np.float32)


def stft_bands(x1, win, nfft, hop, K, W, sr_eff):
    """Hann STFT -> PSD -> log-band energies. Frames are centred on k*hop."""
    need = (K - 1) * hop + win
    xp = np.zeros(need, dtype=np.float32)
    seg = x1[: max(0, need - win // 2)]
    xp[win // 2: win // 2 + len(seg)] = seg
    w = signal.get_window("hann", win, fftbins=True).astype(np.float32)
    scale = np.float32(2.0 / (sr_eff * float(np.sum(w * w))))
    frames = sliding_window_view(xp, win)[::hop][:K]
    out = np.empty((K, W.shape[0]), dtype=np.float32)
    WT = np.ascontiguousarray(W.T)
    step = max(256, int(4e6 // nfft))
    for a in range(0, K, step):
        X = sfft.rfft(frames[a:a + step] * w, n=nfft, axis=1, workers=-1)
        Pw = (X.real ** 2 + X.imag ** 2) * scale
        out[a:a + step] = Pw @ WT
    return out


def multires_spectrogram(x, sr, hop):
    """Log-frequency spectrogram built from three STFTs sharing one time grid.

    < 300 Hz : signal decimated x16, 256-point window (~93 ms at 44.1 kHz)
    0.3-2.5 k: decimated x4,  256-point window (~23 ms)
    > 2.5 kHz: full rate,     256-point window (~6 ms)
    so bass has frequency resolution and hats/clicks have time resolution.
    Returns band energies (linear power per ~1/24-octave band), band edges, fps.
    """
    n, ch = x.shape
    fmax = min(20000.0, sr / 2 * 0.98)
    nb = int(round(24 * math.log2(fmax / 20.0)))
    edges = np.geomspace(20.0, fmax, nb + 1)
    fc = np.sqrt(edges[1:] * edges[:-1])
    K = n // hop + 1
    S = np.zeros((K, nb), dtype=np.float32)
    plan = [(16, 256, 1024, fc < 300.0),
            (4, 256, 1024, (fc >= 300.0) & (fc < 2500.0)),
            (1, 256, 512, fc >= 2500.0)]
    for c in range(ch):
        xc = x[:, c].astype(np.float32)
        for dec, win, nfft, sel in plan:
            xd = signal.resample_poly(xc, 1, dec) if dec > 1 else xc
            sre = sr / dec
            W = log_band_matrix(edges, nfft // 2 + 1, sre / nfft, sel)
            S += stft_bands(xd, win, nfft, hop // dec, K, W, sre)
    S /= ch
    return S, edges, sr / hop


# ------------------------------------------------------------------- stereo

def stereo_metrics(x, sr, lowband):
    L, R = x[:, 0] - np.mean(x[:, 0]), x[:, 1] - np.mean(x[:, 1])
    eL, eR = float(np.sum(L * L)), float(np.sum(R * R))
    n = len(L)
    m = {}
    silent = eL / max(n, 1) < 1e-12 or eR / max(n, 1) < 1e-12
    m["correlation"] = None if silent else rnd(float(np.sum(L * R)) / math.sqrt(eL * eR), 3)
    M, S = (L + R) / 2, (L - R) / 2
    eM, eS = float(np.sum(M * M)), float(np.sum(S * S))
    m["side_mid_db"] = (None if max(eM, eS) <= EPS else
                        rnd(float(np.clip(db10(max(eS, EPS) / max(eM, EPS)), -99.0, 99.0)), 2))
    m["balance_l_minus_r_db"] = None if silent else rnd(db10(eL / eR), 2)
    # level change when folded to mono: power of (L+R)/2 vs mean power of L and R.
    # 0 dB = dual mono, -3 dB = uncorrelated, very negative = anti-phase cancels.
    m["mono_sum_change_db"] = (None if (eL + eR) <= EPS else
                               rnd(float(np.clip(db10(eM / ((eL + eR) / 2)), -99.0, 6.1)), 2))
    m["one_channel_silent"] = bool(silent and (eL + eR) > 1e-12 * n)
    ll, lr = lowband[:, 0], lowband[:, 1]
    el, er = float(np.sum(ll * ll)), float(np.sum(lr * lr))
    m["low_band_correlation"] = (rnd(float(np.sum(ll * lr)) / math.sqrt(el * er), 3)
                                 if min(el, er) / max(n, 1) > 1e-10 else None)
    lm_ = (ll + lr) / 2
    m["low_band_mono_sum_change_db"] = (None if (el + er) / max(n, 1) <= 1e-10 else
                                        rnd(float(np.clip(db10(np.sum(lm_ * lm_) / ((el + er) / 2)), -99.0, 6.1)), 2))
    # correlation over time (100 ms windows, or shorter for short files)
    w = int(max(64, min(0.1 * sr, n // 4)))
    h = max(1, min(w // 2, max(1, n // 1500)))
    t, r = np.zeros(0), np.zeros(0)
    if n > w:
        c = lambda a: np.concatenate(([0.0], np.cumsum(a)))
        cLR, cLL, cRR = c(L * R), c(L * L), c(R * R)
        st = np.arange(0, n - w + 1, h)
        num = cLR[st + w] - cLR[st]
        den = np.sqrt(np.maximum((cLL[st + w] - cLL[st]) * (cRR[st + w] - cRR[st]), 0))
        loud = den / w > 1e-7                       # ignore windows below ~-70 dBFS
        r = np.where(loud, num / np.maximum(den, EPS), np.nan)
        t = (st + w / 2) / sr
        if np.any(loud):
            m["correlation_min_windowed"] = rnd(np.nanmin(r), 3)
    return m, {"t": t, "r": r}


# ------------------------------------------------------------------- clicks

def detect_clicks(x, sr, t0):
    """Find discontinuities: sample-to-sample jumps the surrounding signal does not explain.

    Heuristic
    ---------
    1. Per channel, d1[i] = x[i+1] - x[i] (the jump) and d2 = diff(d1) (how
       far the jump departs from a straight-line continuation). For both, the
       *local scale* is the mean absolute value in windows of 0.5 ms and 2 ms
       placed before and after the sample (3 guard samples skipped), and the
       LARGEST of those four means is used. A sample is a candidate when
           |d1| > 3 x scale(d1)   and   |d2| > 20 x scale(d2)
       (both above a floor of 8 LSB / -50 dB re peak).
       Why drum transients survive this:
       * taking the larger of "before" and "after" means the first step of a
         snare, hat or kick-click is compared with the fast-moving noise that
         FOLLOWS it, where it is not an outlier. A real click sits between
         two calm stretches. Measured: injected clicks, dropouts and cut-off
         notes score 50..1700x on |d2|; the spiky edges of high-passed naive
         square oscillators in 808-style hats / cowbells score 8..18x, which
         is why the d2 threshold is 20x (an earlier 8x flagged those hats);
       * a sine-phase drum with an instant attack only has a kink (a slope
         change): its first step equals the steps that follow, so the d1 test
         fails, however large d2 is.
    2. Candidates within 3 samples (the up and down stroke of one click, same
       or other channel) merge into one event.
    3. Events with >= 3 similar-sized neighbours (0.4x..2.5x) within 150 ms
       are "recurring edges": the resets of a saw / square / sync / bit-crush
       waveform, i.e. part of the sound. They are counted separately with
       their repetition rate (a square has two edges per cycle).
    4. Remaining events are classified by the RMS 10 ms after vs before (1 ms guard; a
       longer window let a kick 20 ms later turn a real click into an "attack"):
       rise >= 3 dB -> "attack" (a note that starts with a step: normal for
       drums, a bug on a slow pad), fall >= 6 dB -> "cutoff" (note chopped
       mid-cycle), otherwise "isolated". Only isolated + cutoff are clicks.
    Limits: clicks buried in noise (inside a hat or snare), clicks within
    ~10 ms before or a few ms into a drum hit (they read as its "attack"), jumps below ~3x the local slope (0.3 x amplitude on a 1 kHz sine at
    44.1 kHz) and clicks that were low-pass filtered afterwards are not found.
    """
    n, ch = x.shape
    res = {"count": 0, "isolated": 0, "cutoff": 0, "hard_attacks": 0, "recurring_edges": 0,
           "recurring_rate_hz": None, "events": [], "hard_attack_times_s": []}
    if n < 64:
        return res, np.zeros(0)
    peak = float(np.max(np.abs(x)))
    floor = max(8.0 / 32768.0, peak * 10 ** (-50 / 20))
    K1, K2, g = 3.0, 20.0, 3
    wins = [max(8, int(round(w * sr))) for w in (0.0005, 0.002)]

    def local_scale(a):
        N = len(a)
        cs = np.concatenate(([0.0], np.cumsum(a)))
        i = np.arange(N)
        scale = np.zeros(N)
        for w in wins:
            b1, b0 = np.clip(i - g, 0, N), np.clip(i - g - w, 0, N)
            a0, a1 = np.clip(i + g + 1, 0, N), np.clip(i + g + 1 + w, 0, N)
            np.maximum(scale, (cs[b1] - cs[b0]) / w, out=scale)
            np.maximum(scale, (cs[a1] - cs[a0]) / w, out=scale)
        return scale

    gap = 3
    idx_all, jump_all, chan_all = [], [], []
    for c in range(ch):
        d = np.diff(x[:, c])
        a = np.abs(d)
        d2 = np.abs(np.diff(d, prepend=d[0], append=d[-1]))     # d2[i], d2[i+1] straddle d[i]
        s2 = local_scale(d2[:-1])
        big2 = np.maximum(d2[:-1], d2[1:])
        cand = np.flatnonzero((a > K1 * local_scale(a) + floor) & (big2 > K2 * s2 + floor))
        if len(cand) == 0:
            continue
        grp = np.concatenate(([0], np.cumsum(np.diff(cand) > gap)))
        order = np.lexsort((-a[cand], grp))
        first = np.concatenate(([True], np.diff(grp[order]) > 0))
        keep = cand[order][first]
        idx_all.append(keep)
        jump_all.append(d[keep])
        chan_all.append(np.full(len(keep), c))
    if not idx_all:
        return res, np.zeros(0)
    idx = np.concatenate(idx_all)
    jump = np.concatenate(jump_all)
    chan = np.concatenate(chan_all)
    o = np.argsort(idx, kind="stable")
    idx, jump, chan = idx[o], jump[o], chan[o]
    # merge across channels
    grp = np.concatenate(([0], np.cumsum(np.diff(idx) > gap)))
    ng = grp[-1] + 1
    order = np.lexsort((-np.abs(jump), grp))
    first = np.concatenate(([True], np.diff(grp[order]) > 0))
    e_idx, e_jump = idx[order][first], jump[order][first]
    has = np.zeros((ng, 2), bool)
    has[grp, np.minimum(chan, 1)] = True
    o = np.argsort(e_idx)
    e_idx, e_jump, has = e_idx[o], e_jump[o], has[o]
    mag = np.abs(e_jump)
    # recurring edges
    E = len(e_idx)
    rec = np.zeros(E, bool)
    if E >= 4:
        win = 0.15 * sr
        votes = np.zeros(E, int)
        for s in (-3, -2, -1, 1, 2, 3):
            j = np.arange(E) + s
            ok = (j >= 0) & (j < E)
            jj = np.clip(j, 0, E - 1)
            ratio = mag[jj] / np.maximum(mag, EPS)
            votes += ok & (np.abs(e_idx[jj] - e_idx) <= win) & (ratio > 0.4) & (ratio < 2.5)
        rec = votes >= 3
        # a fade-in/out shrinks the edges of a waveform: also accept events that
        # continue the period of two recurring neighbours (within 10 %)
        for _ in range(8):
            grew = False
            for sgn in (1, -1):
                k = np.arange(E)
                k1, k2 = k - sgn, k - 2 * sgn
                ok = (k2 >= 0) & (k2 < E) & (k1 >= 0) & (k1 < E) & ~rec
                k1c, k2c = np.clip(k1, 0, E - 1), np.clip(k2, 0, E - 1)
                per = np.abs(e_idx[k1c] - e_idx[k2c])
                d = np.abs(e_idx - e_idx[k1c])
                new = ok & rec[k1c] & rec[k2c] & (per <= win) & (np.abs(d - per) <= 0.1 * per + 2)
                if new.any():
                    rec |= new
                    grew = True
            if not grew:
                break
    res["recurring_edges"] = int(rec.sum())
    if rec.sum() >= 3:
        iv = np.diff(e_idx[rec])
        iv = iv[iv <= 0.15 * sr]
        if len(iv):
            med = float(np.median(iv))
            res["recurring_rate_hz"] = rnd(sr / float(np.mean(iv[(iv > 0.5 * med) & (iv < 1.5 * med)])), 2)
    cs2 = np.concatenate(([0.0], np.cumsum(np.sum(x * x, axis=1))))
    w, gd = int(0.01 * sr), int(0.001 * sr)
    click_times = []
    for k in np.flatnonzero(~rec)[:2000]:
        i = int(e_idx[k])
        b0, b1 = max(0, i - gd - w), max(0, i - gd)
        a0, a1 = min(n, i + 1 + gd), min(n, i + 1 + gd + w)
        before = (cs2[b1] - cs2[b0]) / max(1, w * ch)       # outside the file counts as silence
        after = (cs2[a1] - cs2[a0]) / max(1, w * ch)
        rise = float(db10(after + 1e-9) - db10(before + 1e-9))
        kind = "attack" if rise >= 3.0 else ("cutoff" if rise <= -6.0 else "isolated")
        t = t0 + (i + 1) / sr
        if kind == "attack":
            res["hard_attacks"] += 1
            if len(res["hard_attack_times_s"]) < 16:
                res["hard_attack_times_s"].append(rnd(t, 4))
            continue
        res[kind] += 1
        click_times.append(t)
        if len(res["events"]) < 40:
            chs = "mono" if ch == 1 else ("L+R" if has[k].all() else ("L" if has[k, 0] else "R"))
            res["events"].append({"t_s": rnd(t, 5), "sample": i + 1, "channel": chs, "kind": kind,
                                  "jump": rnd(e_jump[k], 5), "jump_dbfs": rnd(db20(mag[k]), 1),
                                  "level_change_db": rnd(rise, 1)})
    res["count"] = res["isolated"] + res["cutoff"]
    return res, np.array(click_times)


# ------------------------------------------------------- onsets and rhythm

def refine_onsets(mono, sr, t_coarse, prof, edges):
    """Re-time each onset to ~1 ms with a short-window (256) high-rate spectral flux.

    The coarse detector fires early: ~12 ms for a bright hit, up to 0.36 x the
    low-band window (34 ms at 44.1 kHz) for a pure bass note, because its
    windows are long and the log makes the flux peak at the start of the rise.
    So the search window is placed after the coarse time, further and wider
    the more the onset lives below 300 Hz. Inside it each FFT bin is weighted
    by how much its band contributed to the detection (prof), so a hi-hat is
    timed on its own band and not on the pitch pulses of a bass note sounding
    underneath. The onset is the first frame that reaches 30 % of the largest
    flux in the window, plus a fixed bias calibrated on synthetic sharp
    attacks. Soft attacks read late by a fraction of their attack time.
    """
    win, hop, lag = 256, 16, 4
    fc = np.sqrt(edges[1:] * edges[:-1])
    lf = prof[:, fc < 300.0].sum(axis=1) / np.maximum(prof.sum(axis=1), 1e-9)
    win_low = 256 * 16 / sr
    centre_t = t_coarse + 0.012 + lf ** 2 * (0.36 * win_low - 0.012)
    half_t = 0.022 + 0.012 * lf
    at_start = t_coarse < 0.006                 # frame 0 cannot fire early: the file begins there
    centre_t = np.where(at_start, 0.010, centre_t)
    half_t = np.where(at_start, 0.020, half_t)
    H = int(0.045 * sr)                                   # widest half-window, samples
    w = signal.get_window("hann", win, fftbins=True).astype(np.float32)
    amp = np.float32(2.0 / np.sum(w))
    out = np.empty(len(t_coarse))
    L = 2 * H + win + lag * hop
    off = H + win + lag * hop
    pad = np.concatenate((np.zeros(off, np.float32), mono.astype(np.float32), np.zeros(off, np.float32)))
    fbin = np.arange(win // 2 + 1) * sr / win
    bidx = np.clip(np.searchsorted(edges, fbin) - 1, 0, prof.shape[1] - 1)
    wbin = prof[:, bidx].astype(np.float32)
    wbin /= np.maximum(wbin.max(axis=1, keepdims=True), 1e-9)
    for a in range(0, len(t_coarse), 256):
        c = np.round(centre_t[a:a + 256] * sr).astype(int)
        i0 = c - H - win // 2 - lag * hop                 # first sample of the first frame
        segs = pad[(i0 + off)[:, None] + np.arange(L)[None, :]]
        fr = sliding_window_view(segs, win, axis=1)[:, ::hop]              # [o, frames, win]
        X = sfft.rfft(fr * w, axis=2, workers=-1)
        Y = np.log1p(200.0 * amp * np.abs(X))
        fl = (np.maximum(Y[:, lag:] - Y[:, :-lag], 0) * wbin[a:a + 256, None, :]).sum(axis=2)
        nfr = fl.shape[1]
        ctr = i0[:, None] + (np.arange(nfr)[None, :] + lag) * hop + win // 2   # frame centres
        lo = np.minimum(c - half_t[a:a + 256] * sr, t_coarse[a:a + 256] * sr - 0.003 * sr)
        inside = (ctr >= lo[:, None]) & (ctr <= (c + half_t[a:a + 256] * sr)[:, None])
        fl = np.where(inside, fl, 0.0)
        for j in range(len(c)):
            f = fl[j]
            thr = ONSET_EDGE * float(f.max())
            e = int(np.argmax(f >= thr)) if thr > 0 else nfr // 2
            ef = float(e)
            if e > 0 and f[e] > f[e - 1]:          # sub-frame interpolation of the crossing
                ef = e - 1 + (thr - f[e - 1]) / (f[e] - f[e - 1])
            out[j + a] = (i0[j] + (ef + lag) * hop + win // 2 + ONSET_BIAS_FRAC * win) / sr
    return out


ONSET_RATIO = 1.5           # an onset's flux peak vs the median flux of the 300 ms around it
ONSET_EDGE = 0.3            # leading edge = first fine-flux frame at this fraction of the peak
ONSET_BIAS_FRAC = 0.35      # leading edge of the flux -> true onset, in windows (calibrated)


def detect_onsets(S, fps, mono, sr, t0, cropped, n_low, edges, band_cs):
    """Spectral-flux onset detector on the log-band spectrogram.

    A note-off with a short release splatters energy into neighbouring bands
    and looks like an onset to spectral flux. Two guards: the last ~70 ms of
    the file are ignored, and a candidate is dropped when the total level
    falls by >= 3 dB across it while none of three broad bands (< 150 Hz,
    150 Hz-2 kHz, > 2 kHz) rises by 2 dB. A note-off hidden under other
    sustained sound can still be reported as an onset."""
    K = len(S)
    # Smooth each band over ~40 ms first: with 6-23 ms windows the pitch pulses of
    # a buzzy bass note (40-100 Hz) are resolved in time and would look like a
    # stream of onsets. The exact onset time is recovered later by refine_onsets.
    L = max(3, int(round(0.04 * fps)) | 1)
    kern = np.hanning(L + 2)[1:-1].astype(np.float32)
    Ss = ndimage.convolve1d(S, kern / kern.sum(), axis=0, mode="constant")
    Y = np.log1p(1000.0 * np.sqrt(np.maximum(Ss, 0)))
    lag = max(1, int(round(0.012 * fps)))
    Yp = np.concatenate((np.zeros((lag, Y.shape[1]), np.float32), Y))      # silence before the file
    dY = np.maximum(Y - Yp[:K], 0)
    flux = dY.sum(axis=1).astype(np.float64)
    flux_low = dY[:, :n_low].sum(axis=1).astype(np.float64)       # bands below ~200 Hz (kick/bass)
    if cropped:
        flux[: int(0.05 * fps) + 1] = 0
    guard = int(min(0.5 * 256 * 16 / sr + 0.02, 0.25 * K / fps) * fps)
    flux[K - guard - 1:] = 0
    if K < 4 or flux.max() <= 0:
        return np.zeros(0), np.zeros(0), flux, flux_low
    r = lambda s: max(1, int(round(s * fps)))
    mx = signal.argrelextrema(np.concatenate(([0], flux, [0])), np.greater_equal, order=r(0.03))[0] - 1
    mx = mx[(mx >= 0) & (mx < K)]
    # reference level: running median over +-150 ms (ignores the neighbouring hits)
    local = ndimage.median_filter(flux, size=2 * r(0.15) + 1, mode="constant")[mx]
    delta = 0.03 * np.percentile(flux, 99.5) + 0.008 * flux.max()
    mx = mx[flux[mx] > ONSET_RATIO * local + delta]
    keep, last = [], -1e9                                                  # min spacing 50 ms
    for k in mx:
        if k - last >= r(0.05):
            keep.append(k)
            last = k
        elif flux[k] > flux[keep[-1]]:
            keep[-1] = k
            last = k
    keep = np.array(keep, int)
    if len(keep) == 0:
        return np.zeros(0), np.zeros(0), flux, flux_low
    cd = np.concatenate((np.zeros((1, dY.shape[1]), np.float32), np.cumsum(dY, axis=0)))
    prof = cd[np.minimum(keep + 4, K)] - cd[np.maximum(keep - 3, 0)]
    t = refine_onsets(mono, sr, keep / fps, prof, edges)
    t = np.clip(t, 0, None)
    ok = np.concatenate(([True], np.diff(t) > 0.03))
    # note-off guard (see docstring): energies 25 ms before / after each candidate.
    # A note-off is timed loosely, so the "level falls" test is also tried up to
    # 36 ms later; a band that rises at the candidate itself always keeps it.
    n = band_cs.shape[1] - 1
    i = np.round(t * sr).astype(int)
    w, g0 = int(0.025 * sr), int(0.003 * sr)

    def before_after(shift):
        j = i + int(shift * sr)
        b0, b1 = np.clip(j - g0 - w, 0, n), np.clip(j - g0, 0, n)
        a0, a1 = np.clip(j - g0 // 3, 0, n), np.clip(j - g0 // 3 + w, 0, n)
        return (band_cs[:, b1] - band_cs[:, b0]) / w, (band_cs[:, a1] - band_cs[:, a0]) / w

    Eb, Ea = before_after(0.0)
    rises = ((Ea >= 1.6 * Eb) & (Ea >= 1e-4 * Eb.sum(axis=0)[None, :])).any(axis=0)
    falls = np.zeros(len(t), bool)
    for shift in (0.0, 0.012, 0.024, 0.036):
        Eb, Ea = before_after(shift)
        falls |= Ea.sum(axis=0) < 0.5 * Eb.sum(axis=0)
    ok &= rises | ~falls
    return t[ok] + t0, flux[keep][ok], flux, flux_low


def estimate_tempo(flux, flux_low, fps, onsets, strength, dur):
    """Tempo from the autocorrelation of the onset envelope.

    The full-band and the low-band (< 200 Hz) onset envelopes are both used, so
    the kick decides the beat even when 16th hats are as "loud" as the kick in
    flux terms. A comb over 1-4 beat periods and a log-normal prior around
    124 BPM pick the lag; the result is then refined on the onset times by
    maximising beat/8th/16th phase coherence (resolution ~0.01 BPM)."""
    if dur < 2.5 or len(onsets) < 4:
        return None, None

    def acf(v):
        f = v - np.mean(v)
        nf = 1 << int(np.ceil(np.log2(2 * len(f))))
        a = sfft.irfft(np.abs(sfft.rfft(f, nf)) ** 2)[: len(f)]
        return a / a[0] if a[0] > 0 else None

    ac = acf(flux)
    if ac is None:
        return None, None
    acl = acf(flux_low) if np.max(flux_low) > 0.05 * np.max(flux) else None
    if acl is not None:
        ac = 0.5 * (ac + acl)
    bpms = np.arange(55.0, 215.0, 0.5)
    lags = 60.0 * fps / bpms

    def at(l):
        l = np.asarray(l)
        ok = l < len(ac) - 2
        v = np.interp(np.where(ok, l, 0), np.arange(len(ac)), ac)
        return np.where(ok, v, 0.0)

    score = at(lags) + 0.6 * at(2 * lags) + 0.4 * at(3 * lags) + 0.3 * at(4 * lags)
    score = score * np.exp(-0.5 * (np.log2(bpms / 124.0) / 0.7) ** 2)
    b0 = float(bpms[int(np.argmax(score))])
    conf = float(at(60.0 * fps / b0))
    if conf < 0.1:                                 # no periodicity worth calling a tempo
        return None, None
    w = strength / max(np.sum(strength), EPS)
    cand = np.arange(b0 * 0.98, b0 * 1.02, 0.005)
    ph = 2j * np.pi * (onsets[None, :] - onsets[0]) * (cand[:, None] / 60.0)
    coh = sum(np.abs((w[None, :] * np.exp(h * ph)).sum(axis=1)) for h in (1, 2, 4))
    return float(cand[int(np.argmax(coh))]), conf


def grid_metrics(onsets, bpm, source, dur):
    """Deviation of onsets from the 16th-note grid (grid origin = file time 0).

    offset  = constant shift of the whole pattern (circular mean; with swing:
              of the even 16ths), i.e. latency - harmless.
    swing   = mean deviation of odd 16ths minus even 16ths.
    jitter  = spread of the deviations after removing offset and swing.
    best-fit tempo = tempo within +-3 % of the grid tempo that lines the
              onsets up best; drift is the timing error that the difference
              accumulates per minute (negative = the audio runs fast).
    """
    T = 60.0 / bpm / 4.0
    ph = np.exp(2j * np.pi * onsets / T)
    off = float(np.angle(np.mean(ph)) / (2 * np.pi) * T)          # circular-mean offset
    dev = (onsets - off + T / 2) % T - T / 2
    step = np.round((onsets - off) / T).astype(int)
    odd = step % 2 == 1
    swing = None
    res = dev.copy()
    if odd.sum() >= 3 and (~odd).sum() >= 3:
        me, mo = float(np.mean(dev[~odd])), float(np.mean(dev[odd]))
        swing = mo - me
        res[odd] -= mo
        res[~odd] -= me
        off_rep = off + me
    else:
        res -= np.mean(dev)
        off_rep = off + float(np.mean(dev))
    m = {"bpm": rnd(bpm, 3), "bpm_source": source, "sixteenth_ms": rnd(T * 1e3, 2),
         "n_onsets": int(len(onsets)),
         "offset_ms": rnd(off_rep * 1e3, 2),
         "swing_ms": rnd(swing * 1e3, 2) if swing is not None else None,
         "jitter_sd_ms": rnd(np.std(res) * 1e3, 2),
         "jitter_mad_ms": rnd(np.median(np.abs(res - np.median(res))) * 1e3, 2),
         "max_abs_dev_ms": rnd(np.max(np.abs(res)) * 1e3, 2),
         "within_2ms": int(np.sum(np.abs(res) <= 0.002)),
         "within_5ms": int(np.sum(np.abs(res) <= 0.005)),
         "raw_sd_ms": rnd(np.std(dev) * 1e3, 2),
         "worst_onsets_s": [rnd(v, 4) for v in onsets[np.argsort(-np.abs(res))[:5]]]}
    if len(onsets) >= 8 and onsets[-1] - onsets[0] > 2.0:
        cand = np.arange(bpm * 0.97, bpm * 1.03, bpm * 2e-5)
        z = 2j * np.pi * (onsets[None, :] - onsets[0]) * (cand[:, None] / 15.0)
        coh = np.abs(np.exp(z).mean(axis=1))
        fit = float(cand[int(np.argmax(coh))])
        m["best_fit_bpm"] = rnd(fit, 3)
        m["best_fit_coherence"] = rnd(float(np.max(coh)), 3)
        m["drift_ms_per_min"] = rnd((bpm / fit - 1.0) * 60e3, 1)
        m["sixteenths_slipped_over_file"] = rnd(abs(fit - bpm) / 15.0 * dur, 2)
    return m, res


def band_envelopes(x, sr, hop, K):
    """RMS envelopes (linear) of the < 150 Hz band and of the rest, on the frame grid."""
    n, ch = x.shape
    x = x - np.mean(x, axis=0)
    if n > 64:
        sos = signal.butter(4, LOW_SPLIT_HZ, "lowpass", fs=sr, output="sos")
        low = signal.sosfiltfilt(sos, x, axis=0)
    else:
        low = np.zeros_like(x)
    rest = x - low
    w = max(4, int(0.02 * sr))

    def env(v):
        p = np.mean(v * v, axis=1)
        for _ in range(2):                      # two box passes = ~40 ms triangular window
            cs = np.concatenate(([0.0], np.cumsum(p)))
            i = np.arange(n)
            a, b = np.clip(i - w // 2, 0, n), np.clip(i + w // 2 + 1, 0, n)
            p = (cs[b] - cs[a]) / w
        idx = np.minimum(np.arange(K) * hop, n - 1)
        return np.sqrt(np.maximum(p[idx], 0))

    return low, env(low), env(rest)


def pumping_metrics(env_low, env_rest, fps, bpm, dur):
    """Beat-rate modulation of the low band and beat-folded envelope profiles."""
    m, prof = {}, None
    K = len(env_low)
    t = np.arange(K) / fps
    c0 = float(np.mean(env_low))
    if dur >= 2.0 and c0 > 1e-6:
        e = env_low - c0
        nf = 1 << int(np.ceil(np.log2(8 * K)))
        sp = np.abs(sfft.rfft(e * np.hanning(K), nf)) * 2 / (K * 0.5)
        fr = np.arange(len(sp)) * fps / nf
        sel = (fr >= 0.5) & (fr <= 12.0)
        if sel.any():
            k = int(np.argmax(np.where(sel, sp, 0)))
            m["lf_env_dominant_mod_hz"] = rnd(fr[k], 3)
            m["lf_env_dominant_mod_depth"] = rnd(sp[k] / c0, 3)
    if bpm is None or dur < 3 * 60.0 / bpm or c0 <= 1e-6:
        return m, prof
    T = 60.0 / bpm
    ph = (t / T) % 1.0
    osc = np.exp(-2j * np.pi * t / T)
    m["lf_mod_depth_beat"] = rnd(2 * abs(np.mean(env_low * osc)) / c0, 3)
    cr = float(np.mean(env_rest))
    m["rest_mod_depth_beat"] = rnd(2 * abs(np.mean(env_rest * osc)) / cr, 3) if cr > 1e-6 else None
    nb = 48
    b = np.minimum((ph * nb).astype(int), nb - 1)
    lo_db, re_db = db20(env_low), db20(env_rest)
    pl = np.array([np.median(lo_db[b == i]) if np.any(b == i) else np.nan for i in range(nb)])
    pr = np.array([np.median(re_db[b == i]) if np.any(b == i) else np.nan for i in range(nb)])
    if np.isnan(pl).any():
        return m, prof
    # put the low-band peak (the kick) at ~4 % of the beat; ignore anything > 40 dB down
    sh = int(np.argmax(pl)) - 2
    pl, pr = np.roll(pl, -sh), np.roll(pr, -sh)
    top = max(float(np.max(pl)), float(np.max(pr)))
    pl, pr = np.maximum(pl, top - 40.0), np.maximum(pr, top - 40.0)
    x = (np.arange(nb) + 0.5) / nb
    pre = (x >= 0.80) & (x <= 0.97)                 # just before the next kick
    aft = (x >= 0.08) & (x <= 0.70)                 # after the kick's attack
    # the dip compares lower envelopes (minima), so gaps between drum hits do not count
    m["kick_punch_db"] = rnd(float(np.max(pl)) - float(np.mean(pl[pre])), 2)
    m["lf_duck_dip_db"] = rnd(max(0.0, float(np.min(pl[pre])) - float(np.min(pl[aft]))), 2)
    m["rest_duck_dip_db"] = rnd(max(0.0, float(np.min(pr[pre])) - float(np.min(pr[aft]))), 2)
    m["lf_range_db"] = rnd(float(np.max(pl) - np.min(pl)), 2)
    prof = {"x": x, "low": pl, "rest": pr, "T": T}
    return m, prof


# -------------------------------------------------------------------- pitch

def note_name(f):
    midi = 69 + 12 * math.log2(f / 440.0)
    n = int(round(midi))
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1), (midi - n) * 100.0


def pitch_metrics(mono, sr):
    """Fundamental (normalised autocorrelation + harmonic peak refinement) and an
    inharmonic-energy indicator from a Blackman-Harris spectrum of the steady part."""
    n = len(mono)
    skip = int(min(0.03 * sr, n * 0.1))
    avail = n - skip
    if avail < 512:
        return None, None
    N = 1 << int(math.floor(math.log2(min(avail, 65536))))
    seg = mono[skip:skip + N].astype(np.float64)
    seg = seg - np.mean(seg)
    if np.sqrt(np.mean(seg ** 2)) < 1e-4:
        return None, None
    # --- coarse: McLeod-style normalised square difference function
    Na = min(N, 16384)
    s = seg[:Na]
    r = sfft.irfft(np.abs(sfft.rfft(s, 2 * Na)) ** 2)[:Na]
    c2 = np.concatenate(([0.0], np.cumsum(s * s)))
    lag = np.arange(Na)
    m0 = c2[Na - lag] + (c2[Na] - c2[lag])
    nsdf = 2 * r / np.maximum(m0, EPS)
    lmin, lmax = max(2, int(sr / 5000)), min(int(sr / 20), Na // 2)
    if lmax <= lmin + 2:
        return None, None
    neg = np.flatnonzero(nsdf[:lmax] < 0)
    start = max(lmin, int(neg[0]) if len(neg) else lmin)
    body = nsdf[start:lmax]
    if len(body) < 3:
        return None, None
    pk = signal.argrelextrema(body, np.greater)[0]
    if len(pk) == 0:
        return None, None
    best = float(np.max(body[pk]))
    p = int(pk[np.flatnonzero(body[pk] >= 0.9 * best)[0]]) + start
    conf = float(nsdf[p])
    y0, y1, y2 = nsdf[p - 1], nsdf[p], nsdf[p + 1]
    den = y0 - 2 * y1 + y2
    f0 = sr / (p + (0.5 * (y0 - y2) / den if den != 0 else 0.0))
    # --- refine on the first harmonics of a zero-padded spectrum
    w = signal.get_window("blackmanharris", N, fftbins=True)
    zp = 4
    X = np.abs(sfft.rfft(seg * w, N * zp))
    dfz = sr / (N * zp)
    est, wts = [], []
    for k in range(1, 13):
        fk = k * f0
        if fk > sr / 2 * 0.95:
            break
        a, b = int((fk * 0.97) / dfz), int((fk * 1.03) / dfz) + 2
        b = min(b, len(X) - 1)
        if b - a < 3:
            continue
        j = a + int(np.argmax(X[a:b]))
        if j <= 0 or j >= len(X) - 1 or X[j] <= 0:
            continue
        l0, l1, l2 = np.log(X[j - 1] + EPS), np.log(X[j]), np.log(X[j + 1] + EPS)
        dd = l0 - 2 * l1 + l2
        off = 0.5 * (l0 - l2) / dd if dd != 0 else 0.0
        est.append((j + off) * dfz / k)
        wts.append(X[j])
    if est:
        est, wts = np.array(est), np.array(wts)
        good = wts >= 0.03 * wts.max()
        f0 = float(np.sum(est[good] * wts[good]) / np.sum(wts[good]))
    # --- harmonic vs. non-harmonic energy
    P = np.abs(sfft.rfft(seg * w)) ** 2
    df = sr / N
    f = np.arange(len(P)) * df
    h = np.maximum(np.round(f / f0), 1)
    # Harmonic tolerance: +-5 bins, widened to the measured -30 dB half-width
    # of the fundamental's peak. A decaying or gliding note (any drum) has a
    # broad peak; without this its own skirts would count as "inharmonic".
    j1 = int(round(f0 / df))
    a_, b_ = max(1, int(j1 * 0.9)), min(len(P) - 1, int(j1 * 1.1) + 2)
    j1 = a_ + int(np.argmax(P[a_:b_])) if b_ > a_ else j1
    thr = P[j1] * 1e-3
    lo_ = j1
    while lo_ > 1 and P[lo_ - 1] > thr and (j1 - lo_) * df < 0.25 * f0:
        lo_ -= 1
    hi_ = j1
    while hi_ < len(P) - 2 and P[hi_ + 1] > thr and (hi_ - j1) * df < 0.25 * f0:
        hi_ += 1
    width = max(5 * df, (max(j1 - lo_, hi_ - j1) + 1) * df)
    tol = width + 2e-4 * h * f0
    harm = np.abs(f - h * f0) <= tol
    q = max(1, N // 5)
    decay = db10(np.mean(seg[-q:] ** 2) / max(np.mean(seg[:q] ** 2), EPS))
    valid = f >= max(15.0, min(20.0, f0 * 0.5))
    Eh = float(np.sum(P[harm & valid]))
    En = float(np.sum(P[~harm & valid]))
    E1 = float(np.sum(P[harm & (h == 1)]))
    name, cents = note_name(f0)
    amp = 2 * np.sqrt(P) / np.sum(w)                 # sine amplitude per bin, linear FS
    nh = np.where(~harm & valid, amp, 0)
    js = int(np.argmax(nh))
    blur_ok = width < 0.2 * f0                       # else the harmonic windows cover everything
    m = {"f0_hz": rnd(f0, 3), "note": name, "cents": rnd(cents, 1), "confidence": rnd(conf, 3),
         "thd_db": rnd(db10((Eh - E1) / max(E1, EPS)), 1) if Eh > E1 else -150.0,
         "inharmonic_db_rel_harmonic": rnd(db10(En / max(Eh, EPS)), 1) if blur_ok else None,
         "strongest_stray_hz": rnd(f[js], 1) if blur_ok else None,
         "strongest_stray_dbc": rnd(db20(nh[js] / max(amp.max(), EPS)), 1) if blur_ok else None,
         "analysis_window_s": rnd(N / sr, 3),
         "harmonic_tolerance_hz": rnd(width, 2),
         "level_change_over_window_db": rnd(decay, 1),
         "percussive": bool(decay < -15.0),
         "reliable": bool(conf >= 0.8 and 2 * width < 0.5 * f0)}
    return m, {"f": f, "amp_db": db20(amp), "harm": harm, "f0": f0}


# ----------------------------------------------------------------- analysis

def analyze(path, args, want_plot=True):
    start = getattr(args, "start", 0.0) or 0.0
    dur_arg = getattr(args, "dur", None)
    x, xi, sr, bits, total = read_wav(path, start, dur_arg)
    n, ch = x.shape
    if n < 16:
        raise SystemExit("ears: %s: nothing to analyse (%d samples)" % (path, n))
    dur = n / sr
    t0 = start
    M = {"file": os.path.basename(path), "path": os.path.abspath(path),
         "sample_rate": sr, "channels": ch, "bits": bits,
         "file_duration_s": rnd(total, 4), "analysed_start_s": rnd(t0, 4), "duration_s": rnd(dur, 4)}
    PD = {"x": x, "sr": sr, "t0": t0, "dur": dur}

    # level
    peaks = [float(np.max(np.abs(x[:, c]))) for c in range(ch)]
    tps = true_peak(x)
    rms_c = [float(np.sqrt(np.mean(x[:, c] ** 2))) for c in range(ch)]
    rms = float(np.sqrt(np.mean(x ** 2)))
    dc = [float(np.mean(x[:, c])) for c in range(ch)]
    M["level"] = {
        "sample_peak_dbfs": rnd(db20(max(peaks)), 2), "true_peak_dbtp": rnd(db20(max(tps)), 2),
        "sample_peak_per_channel_dbfs": [rnd(db20(p), 2) for p in peaks],
        "rms_dbfs": rnd(db20(rms), 2), "rms_per_channel_dbfs": [rnd(db20(r), 2) for r in rms_c],
        "crest_factor_db": rnd(db20(max(peaks)) - db20(rms), 2) if rms > 1e-9 else None,
        "dc_offset": [rnd(d, 6) for d in dc],
        "dc_offset_dbfs": [rnd(db20(d), 1) for d in dc]}
    silent = max(peaks) < 1e-5
    M["clipping"], PD["clip_idx"] = clipping_metrics(x, xi, bits, sr, t0)
    M["loudness"], PD["loud"] = loudness_metrics(x, sr, t0)

    # spectrum
    f, P = average_spectrum(x, sr)
    M["spectrum"] = spectrum_metrics(f, P, sr)
    PD["ltas"] = smooth_spectrum(f, P, sr)

    # frame grid shared by spectrogram, onsets, envelopes
    hop = 128 if dur >= 3.0 else (64 if dur >= 1.2 else (32 if dur >= 0.5 else 16))
    S, edges, fps = multires_spectrogram(x, sr, hop)
    K = len(S)
    PD.update(S=S, edges=edges, fps=fps)
    low, env_low, env_rest = band_envelopes(x, sr, hop, K)
    PD.update(env_low=env_low, env_rest=env_rest)

    # stereo
    corr = None
    if ch == 2:
        M["stereo"], PD["corr"] = stereo_metrics(x, sr, low)
        corr = M["stereo"]["correlation"]
        mono = (x[:, 0] - x[:, 1]) / 2 if (corr is not None and corr < -0.2) else np.mean(x, axis=1)
        if M["stereo"]["one_channel_silent"]:
            mono = x[:, int(np.argmax(rms_c))]
    else:
        M["stereo"] = None
        mono = x[:, 0]

    M["clicks"], PD["click_t"] = detect_clicks(x, sr, t0)

    # rhythm
    n_low = int(np.sum(np.sqrt(edges[1:] * edges[:-1]) < 200.0))
    # per-sample power of three broad bands (cumulative), for the note-off guard
    xz = x - np.mean(x, axis=0)
    high = signal.sosfiltfilt(signal.butter(4, min(2000.0, 0.4 * sr), "highpass", fs=sr, output="sos"),
                              xz, axis=0) if n > 64 else np.zeros_like(x)
    band_cs = np.stack([np.concatenate(([0.0], np.cumsum(np.sum(v * v, axis=1))))
                        for v in (low, high, xz - low - high)])
    on_t, on_s, flux, flux_low = (np.zeros(0), np.zeros(0), np.zeros(K), np.zeros(K)) if silent else \
        detect_onsets(S, fps, mono, sr, t0, cropped=start > 0, n_low=n_low, edges=edges, band_cs=band_cs)
    PD["onsets"] = on_t
    tempo, tconf = estimate_tempo(flux, flux_low, fps, on_t, on_s, dur)
    lo_db, re_db = db20(env_low), db20(env_rest)
    r = lambda s: max(1, int(round(s * fps)))
    ons, kick_like = [], 0
    lf_max = float(np.max(lo_db)) if K else -200.0
    for t, s in zip(on_t, on_s):
        k = int(np.clip(round((t - t0) * fps), 0, K - 1))
        a = float(np.max(lo_db[k:k + r(0.08) + 1]))
        b = float(np.max(re_db[k:k + r(0.08) + 1]))
        pre = float(lo_db[max(0, k - r(0.04))]) if k >= r(0.04) else -200.0
        kl = bool(a - pre >= 6.0 and a >= lf_max - 8.0 and a > -50.0 and a >= b - 15.0)
        kick_like += kl
        ons.append({"t_s": rnd(t, 4), "strength": rnd(s / max(on_s.max(), EPS), 3),
                    "low_peak_dbfs": rnd(a, 1), "rest_peak_dbfs": rnd(b, 1), "kick_like": kl})
    M["rhythm"] = {"onset_count": int(len(on_t)), "onsets_per_s": rnd(len(on_t) / dur, 2),
                   "kick_like_onsets": int(kick_like),
                   "tempo_bpm_estimate": rnd(tempo, 2), "tempo_confidence": rnd(tconf, 2),
                   "onset_times_s": [rnd(t, 4) for t in on_t[:400]], "onsets": ons[:400]}
    bpm_arg = getattr(args, "bpm", None)
    bpm_used, src = (bpm_arg, "--bpm") if bpm_arg else ((tempo, "estimated") if tempo else (None, None))
    PD["grid_dev"] = None
    if bpm_used and len(on_t) >= 3:
        M["grid"], PD["grid_dev"] = grid_metrics(on_t, bpm_used, src, dur)
    else:
        M["grid"] = None
    M["pumping"], PD["prof"] = pumping_metrics(env_low, env_rest, fps, bpm_used, dur)
    M["pumping"]["bpm_used"] = rnd(bpm_used, 3)

    # pitch (single notes)
    M["pitch"], PD["pitch"] = None, None
    force = getattr(args, "pitch", False)
    if not silent and (force or dur <= 8.0):
        # analyse one note: from the first onset to the next one (repeated hits
        # would otherwise comb-filter the spectrum and blur the harmonics)
        seg_ = mono
        if len(on_t):
            a0 = int(max(0.0, on_t[0] - t0) * sr)
            nxt = [t for t in on_t[1:] if t - on_t[0] >= 0.1]
            a1 = int((nxt[0] - t0) * sr) - int(0.005 * sr) if nxt else n
            if a1 - a0 >= int(0.1 * sr):
                seg_ = mono[a0:a1]
        pm, pp = pitch_metrics(seg_, sr)
        # a single note: few onsets and periodic, or strongly periodic whatever the onset count
        if pm is not None and (force or pm["confidence"] >= (0.5 if len(on_t) <= 2 else 0.9)):
            M["pitch"], PD["pitch"] = pm, pp

    M["flags"] = make_flags(M)
    return M, PD


def make_flags(M):
    fl = []
    lv, cl, ld, st = M["level"], M["clipping"], M["loudness"], M["stereo"]
    if cl["samples_at_full_scale"] >= 2:
        fl.append("CLIPPING: %d samples at full scale, %d flat-topped runs (longest %d)" % (
            cl["samples_at_full_scale"], cl["flat_runs"], cl["flat_run_longest_samples"]))
    elif cl["flat_tops_likely"]:
        fl.append("FLAT TOPS: %d runs at %.1f dBFS, longest %d samples (clipping before the output gain, a saturator, or a naive square/pulse wave)" % (
            cl["flat_runs"], cl["flat_run_level_dbfs"], cl["flat_run_longest_samples"]))
    elif lv["true_peak_dbtp"] is not None and lv["true_peak_dbtp"] > 0.0:
        fl.append("INTER-SAMPLE OVER: true peak %+.2f dBTP" % lv["true_peak_dbtp"])
    if lv["sample_peak_dbfs"] is not None and lv["sample_peak_dbfs"] < -60:
        fl.append("SILENT: peak %s dBFS" % fnum(lv["sample_peak_dbfs"]))
    if max(abs(d or 0) for d in lv["dc_offset"]) > 0.004:
        fl.append("DC OFFSET: %s" % ", ".join("%+.4f" % d for d in lv["dc_offset"]))
    ck = M["clicks"]
    if ck["count"]:
        fl.append("CLICKS: %d, first at %s s (%d isolated, %d cutoff)%s" % (
            ck["count"], ck["events"][0]["t_s"], ck["isolated"], ck["cutoff"],
            " - with %d recurring edges: maybe the sound's own grain/edges, check --zoom" % ck["recurring_edges"]
            if ck["recurring_edges"] >= 5 * ck["count"] else ""))
    if st:
        if st["one_channel_silent"]:
            fl.append("ONE CHANNEL SILENT")
        if st["correlation"] is not None and st["correlation"] < -0.1:
            fl.append("ANTI-PHASE: L/R correlation %+.2f (mono sum %+.1f dB)" % (
                st["correlation"], st["mono_sum_change_db"]))
        elif st["mono_sum_change_db"] is not None and st["mono_sum_change_db"] < -4.0:
            fl.append("MONO LOSS: %+.1f dB when summed to mono" % st["mono_sum_change_db"])
        elif st["low_band_correlation"] is not None and st["low_band_correlation"] < 0.5:
            fl.append("WIDE BASS: low-band correlation %+.2f" % st["low_band_correlation"])
        b = st["balance_l_minus_r_db"]
        if b is not None and abs(b) > 3.0:
            fl.append("L/R IMBALANCE: %+.1f dB" % b)
    if M["duration_s"] >= 3.0 and ld["integrated_lufs"] is not None:
        if ld["integrated_lufs"] > -6.0:
            fl.append("VERY LOUD: %.1f LUFS" % ld["integrated_lufs"])
        elif ld["integrated_lufs"] < -26.0:
            fl.append("QUIET: %.1f LUFS" % ld["integrated_lufs"])
    if lv["crest_factor_db"] is not None and lv["crest_factor_db"] < 5.0 and M["duration_s"] >= 3.0 \
            and M["rhythm"]["onset_count"] >= 8 and not M["pitch"]:
        fl.append("SQUASHED: crest factor %.1f dB" % lv["crest_factor_db"])
    b = M["spectrum"]["bands_db_rel_total"]
    if M["duration_s"] >= 3.0 and not M["pitch"] and b.get("presence") is not None:
        if max(b["presence"], b["air"] or -99) > -8.0:
            fl.append("HARSH/BRIGHT: presence %.1f dB, air %.1f dB of total" % (b["presence"], b["air"]))
    g = M["grid"]
    if g and g["bpm_source"] == "--bpm" and g["n_onsets"] >= 8:
        if (g.get("sixteenths_slipped_over_file") or 0) > 0.25 and g["best_fit_coherence"] >= 0.6:
            fl.append("TEMPO MISMATCH: onsets fit %.2f BPM, not %.6g (drift %+.0f ms/min)" % (
                g["best_fit_bpm"], g["bpm"], g["drift_ms_per_min"]))
        elif g["jitter_mad_ms"] > 3.0:
            fl.append("LOOSE TIMING: jitter MAD %.1f ms vs %.6g BPM 16ths" % (g["jitter_mad_ms"], g["bpm"]))
    p = M["pitch"]
    if p and p["reliable"] and not p["percussive"] and (p["inharmonic_db_rel_harmonic"] or -999) > -40.0:
        fl.append("INHARMONIC CONTENT: %.1f dB re harmonics (aliasing or noise), strongest stray %.0f Hz" % (
            p["inharmonic_db_rel_harmonic"], p["strongest_stray_hz"]))
    return fl


def summary_text(M):
    L = []
    add = L.append
    lv, cl, ld, sp, st, ck, rh, g, pu, p = (M[k] for k in (
        "level", "clipping", "loudness", "spectrum", "stereo", "clicks", "rhythm", "grid", "pumping", "pitch"))
    chs = {1: "mono", 2: "stereo"}.get(M["channels"], "%d ch" % M["channels"])
    add("== ears: %s ==" % M["file"])
    crop = ""
    if abs(M["file_duration_s"] - M["duration_s"]) > 1e-3:
        crop = "   (analysed %.2f..%.2f s of %.2f s)" % (
            M["analysed_start_s"], M["analysed_start_s"] + M["duration_s"], M["file_duration_s"])
    add("file     : %.3f s  %d Hz  %s  %s-bit%s" % (M["duration_s"], M["sample_rate"], chs,
                                                   M["bits"] or "float", crop))
    pc = "" if M["channels"] == 1 else "   (L %s  R %s)" % tuple(fnum(v, "{:.2f}") for v in lv["sample_peak_per_channel_dbfs"])
    add("peak     : sample %s dBFS   true-peak %s dBTP%s" % (
        fnum(lv["sample_peak_dbfs"], "{:.2f}"), fnum(lv["true_peak_dbtp"], "{:.2f}"), pc))
    s = "clipping : %d samples at full scale, %d within 0.1 dB, %d flat-topped runs" % (
        cl["samples_at_full_scale"], cl["samples_within_0p1dB"], cl["flat_runs"])
    if cl["flat_runs"]:
        s += " (longest %d smp, at %s dBFS, first %s s)" % (
            cl["flat_run_longest_samples"], fnum(cl["flat_run_level_dbfs"]), cl["flat_run_times_s"][0])
    add(s)
    add("level    : RMS %s dBFS   crest %s dB   DC %s" % (
        fnum(lv["rms_dbfs"]), fnum(lv["crest_factor_db"]),
        " / ".join("%+.4f" % d for d in lv["dc_offset"])))
    s = "loudness : integrated %s LUFS%s" % (fnum(ld["integrated_lufs"]),
                                            " (ungated, < 0.4 s)" if ld["integrated_is_ungated_estimate"] else "")
    if M["channels"] == 1:
        s += " (mono; as dual-mono %s)" % fnum(ld["integrated_lufs_as_dual_mono"])
    if ld["short_term_max_lufs"] is not None:
        s += "   short-term min/max %s/%s" % (fnum(ld["short_term_min_lufs"]), fnum(ld["short_term_max_lufs"]))
    if ld["momentary_max_lufs"] is not None:
        s += "   momentary max %s" % fnum(ld["momentary_max_lufs"])
    if ld["loudness_range_lu"] is not None:
        s += "   LRA %s LU" % fnum(ld["loudness_range_lu"])
    add(s)
    b = sp["bands_db_rel_total"]
    add("bands dB : " + " | ".join("%s %s" % (k, fnum(b[k])) for k, _, _ in BANDS) + "   (re total)")
    add("spectrum : centroid %s Hz   rolloff(85%%) %s Hz" % (fnum(sp["centroid_hz"], "{:.0f}"),
                                                           fnum(sp["rolloff85_hz"], "{:.0f}")))
    if st:
        add("stereo   : corr %s (min %s)   low-band corr %s   side/mid %s dB   balance L-R %s dB" % (
            fnum(st["correlation"], "{:+.2f}"), fnum(st.get("correlation_min_windowed"), "{:+.2f}"),
            fnum(st["low_band_correlation"], "{:+.2f}"), fnum(st["side_mid_db"]),
            fnum(st["balance_l_minus_r_db"], "{:+.2f}")))
        add("mono sum : level change %s dB (low band %s dB)" % (
            fnum(st["mono_sum_change_db"], "{:+.1f}"), fnum(st["low_band_mono_sum_change_db"], "{:+.1f}")))
    else:
        add("stereo   : mono file")
    s = "clicks   : %d (%d isolated, %d cutoff) | hard attacks %d | recurring edges %d" % (
        ck["count"], ck["isolated"], ck["cutoff"], ck["hard_attacks"], ck["recurring_edges"])
    if ck["recurring_rate_hz"]:
        s += " at ~%s Hz" % fnum(ck["recurring_rate_hz"])
    add(s)
    for e in ck["events"][:4]:
        add("           %.4f s  %s  jump %s dBFS  %s (level %+.1f dB after)" % (
            e["t_s"], e["channel"], fnum(e["jump_dbfs"]), e["kind"], e["level_change_db"]))
    if len(ck["events"]) > 4:
        add("           ... %d more in the JSON" % (ck["count"] - 4))
    s = "onsets   : %d (%.2f/s), %d kick-like" % (rh["onset_count"], rh["onsets_per_s"], rh["kick_like_onsets"])
    if rh["tempo_bpm_estimate"]:
        s += "   tempo estimate %.2f BPM (confidence %.2f)" % (rh["tempo_bpm_estimate"], rh["tempo_confidence"])
    elif rh["onset_count"] and rh["onset_count"] <= 6:
        s += "   at " + ", ".join("%.3f" % t for t in rh["onset_times_s"]) + " s"
    add(s)
    if g:
        s = "grid     : vs %.6g BPM 16ths (%s): offset %+.1f ms" % (g["bpm"], g["bpm_source"], g["offset_ms"])
        if g.get("swing_ms") is not None:
            s += ", swing %+.1f ms" % g["swing_ms"]
        s += ", jitter sd %.2f / MAD %.2f ms, max %.1f ms, %d/%d within 2 ms" % (
            g["jitter_sd_ms"], g["jitter_mad_ms"], g["max_abs_dev_ms"], g["within_2ms"], g["n_onsets"])
        if g.get("best_fit_bpm") is not None and g["bpm_source"] == "--bpm":
            s += ", best fit %.2f BPM (drift %+.0f ms/min)" % (g["best_fit_bpm"], g["drift_ms_per_min"])
        add(s)
    if pu.get("lf_mod_depth_beat") is not None:
        s = "pumping  : LF(<150 Hz) mod depth at beat rate %.2f" % pu["lf_mod_depth_beat"]
        if pu.get("kick_punch_db") is not None:
            s += " | kick punch %+.1f dB | duck dip: low %.1f dB, rest %.1f dB" % (
                pu["kick_punch_db"], pu["lf_duck_dip_db"], pu["rest_duck_dip_db"])
        add(s)
    elif pu.get("lf_env_dominant_mod_hz") is not None:
        add("pumping  : no tempo; strongest LF envelope modulation %.2f Hz, depth %.2f" % (
            pu["lf_env_dominant_mod_hz"], pu["lf_env_dominant_mod_depth"]))
    if p:
        add("pitch    : %.2f Hz = %s %+.1f cents (confidence %.2f%s%s) | THD %s dB | inharmonic %s dB re harmonics | strongest stray %s Hz at %s dBc" % (
            p["f0_hz"], p["note"], p["cents"], p["confidence"], "" if p["reliable"] else ", UNRELIABLE",
            ", percussive" if p["percussive"] else "",
            fnum(p["thd_db"]), fnum(p["inharmonic_db_rel_harmonic"]),
            fnum(p["strongest_stray_hz"], "{:.0f}"), fnum(p["strongest_stray_dbc"])))
    if M["flags"]:
        add("flags    : " + M["flags"][0])
        for f_ in M["flags"][1:]:
            add("           " + f_)
    else:
        add("flags    : none")
    return "\n".join(L)


# ----------------------------------------------------------------- plotting

def _mpl():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({
        "font.size": 11, "font.family": "DejaVu Sans", "axes.titlesize": 11.5,
        "axes.labelsize": 11, "xtick.labelsize": 10.5, "ytick.labelsize": 10.5,
        "axes.edgecolor": "#8a8984", "axes.linewidth": 0.8, "axes.facecolor": C_SURF,
        "figure.facecolor": C_SURF, "savefig.facecolor": C_SURF, "text.color": C_INK,
        "axes.labelcolor": C_INK2, "xtick.color": C_INK2, "ytick.color": C_INK2,
        "grid.color": C_GRID, "grid.linewidth": 0.7, "axes.titleweight": "bold",
        "axes.titlelocation": "left", "legend.frameon": False, "legend.fontsize": 10,
        "axes.spines.top": False, "axes.spines.right": False})
    return plt


FREQ_TICKS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]


def _flabel(f):
    return ("%gk" % (f / 1000.0)) if f >= 1000 else "%g" % f


def _wave(ax, v, sr, t0, color, cols=1500):
    n = len(v)
    if n <= 3 * cols:
        ax.plot(t0 + np.arange(n) / sr, v, color=color, lw=0.9)
    else:
        m = n // cols
        r = v[: m * cols].reshape(cols, m)
        t = t0 + (np.arange(cols) + 0.5) * m / sr
        ax.fill_between(t, r.min(axis=1), r.max(axis=1), color=color, lw=0.4, step="mid")


def _spec_image(ax, S, edges, t0, t1, dyn=80.0, cols=1500, cmap="magma"):
    D = 10 * np.log10(np.maximum(S, 1e-14))
    K = len(D)
    if K > cols:
        m = int(math.ceil(K / cols))
        pad = (-K) % m
        if pad:
            D = np.concatenate((D, np.repeat(D[-1:], pad, axis=0)))
        D = D.reshape(-1, m, D.shape[1]).max(axis=1)
    vmax = float(np.max(D))
    vmax = vmax if vmax > -130 else -60.0
    im = ax.imshow(D.T, origin="lower", aspect="auto", cmap=cmap, vmin=vmax - dyn, vmax=vmax,
                   extent=[t0, t1, 0, len(edges) - 1], interpolation="nearest")
    lo, hi = edges[0], edges[-1]
    ticks = [f for f in FREQ_TICKS if lo <= f <= hi * 1.001]
    pos = [(len(edges) - 1) * math.log(f / lo) / math.log(hi / lo) for f in ticks]
    ax.set_yticks(pos)
    ax.set_yticklabels([_flabel(f) for f in ticks])
    ax.set_ylabel("Hz")
    return im


def _key_lines(M):
    lv, cl, ld, sp, st, ck, rh, g, pu, p = (M[k] for k in (
        "level", "clipping", "loudness", "spectrum", "stereo", "clicks", "rhythm", "grid", "pumping", "pitch"))
    b = sp["bands_db_rel_total"]
    f0 = lambda v: fnum(v, "{:.0f}")
    L = ["Peak   %s dBFS   true peak %s dBTP" % (fnum(lv["sample_peak_dbfs"]), fnum(lv["true_peak_dbtp"])),
         "Clip   %d at FS   %d flat runs" % (cl["samples_at_full_scale"], cl["flat_runs"]),
         "RMS    %s dBFS   crest %s dB" % (fnum(lv["rms_dbfs"]), fnum(lv["crest_factor_db"])),
         "Loud   %s LUFS int   LRA %s LU" % (fnum(ld["integrated_lufs"]), fnum(ld["loudness_range_lu"])),
         "DC     " + " / ".join("%+.4f" % d for d in lv["dc_offset"]),
         "Bands  sub %s  bass %s  lo-mid %s" % (f0(b["sub"]), f0(b["bass"]), f0(b["low-mid"])),
         "  dB   mid %s  pres %s  air %s" % (f0(b["mid"]), f0(b["presence"]), f0(b["air"])),
         "Spec   centroid %s Hz  rolloff %s Hz" % (f0(sp["centroid_hz"]), f0(sp["rolloff85_hz"]))]
    if st:
        L.append("Stereo corr %s  low %s  S/M %s dB" % (fnum(st["correlation"], "{:+.2f}"),
                                                         fnum(st["low_band_correlation"], "{:+.2f}"),
                                                         f0(st["side_mid_db"])))
        L.append("Mono   sum %s dB  bal L-R %s dB" % (fnum(st["mono_sum_change_db"], "{:+.1f}"),
                                                     fnum(st["balance_l_minus_r_db"], "{:+.1f}")))
    else:
        L.append("Stereo mono file")
    L.append("Clicks %d   hard attacks %d   edges %d" % (ck["count"], ck["hard_attacks"], ck["recurring_edges"]))
    s = "Onsets %d (%d kick-like)" % (rh["onset_count"], rh["kick_like_onsets"])
    if rh["tempo_bpm_estimate"]:
        s += "  ~%.1f BPM" % rh["tempo_bpm_estimate"]
    L.append(s)
    if g:
        L.append("Grid   %.5g BPM: jitter %.1f ms sd, off %+.1f" % (g["bpm"], g["jitter_sd_ms"], g["offset_ms"]))
    if pu.get("kick_punch_db") is not None:
        L.append("Pump   LF depth %.2f  punch %+.0f dB" % (pu["lf_mod_depth_beat"], pu["kick_punch_db"]))
        L.append("       duck dip low %.1f / rest %.1f dB" % (pu["lf_duck_dip_db"], pu["rest_duck_dip_db"]))
    if p:
        L.append("Pitch  %s %+.0f ct (%.2f Hz)" % (p["note"], p["cents"], p["f0_hz"]))
        L.append("       THD %s dB   inharmonic %s dB" % (f0(p["thd_db"]), f0(p["inharmonic_db_rel_harmonic"])))
    return L


def _band_marks(ax, M, ymax, with_values=True):
    b = M["spectrum"]["bands_db_rel_total"] if M else None
    nyq_top = ax.get_xlim()[1]
    for name, lo, hi in BANDS:
        if lo > 20:
            ax.axvline(lo, color="#8a8984", lw=0.9, ls=(0, (4, 3)), zorder=1)
        hi2 = min(hi, nyq_top)
        if hi2 <= lo:
            continue
        xc = math.sqrt(lo * hi2)
        short = {"low-mid": "lo-mid", "presence": "pres"}.get(name, name)
        ax.text(xc, ymax - 1.0, short, ha="center", va="top", fontsize=10, color=C_INK2)
        if with_values and b and b.get(name) is not None:
            ax.text(xc, ymax - 6.5, "%.0f" % b[name], ha="center", va="top", fontsize=11,
                    color=C_INK, weight="bold")


def _freq_axis(ax, sr):
    ax.set_xscale("log")
    top = min(20000.0, sr / 2)
    ax.set_xlim(20, top)
    ticks = [f for f in FREQ_TICKS if f <= top * 1.001]
    ax.set_xticks(ticks)
    ax.set_xticklabels([_flabel(f) for f in ticks])
    ax.minorticks_off()
    ax.set_xlabel("Hz")


def render_main(M, PD, out_png, title):
    plt = _mpl()
    x, sr, t0, dur = PD["x"], PD["sr"], PD["t0"], PD["dur"]
    n, ch = x.shape
    t1 = t0 + dur
    stereo = ch == 2
    fig = plt.figure(figsize=(16, 12), dpi=100)
    hr = [0.85, 0.85, 3.0, 1.45, 1.35] + ([0.75] if stereo else [])
    if not stereo:
        hr[0] = 1.7
    gl = fig.add_gridspec(len(hr) - (0 if stereo else 1), 1, left=0.05, right=0.628, top=0.895, bottom=0.05,
                          height_ratios=hr if stereo else [hr[0]] + hr[2:], hspace=0.30)
    fig.text(0.012, 0.975, title, fontsize=18, weight="bold", va="top")
    chs = "stereo" if stereo else "mono"
    fig.text(0.012, 0.945, "%s   %.3f s   %d Hz   %s   %s-bit" % (M["file"], dur, sr, chs, M["bits"] or "float"),
             fontsize=11.5, color=C_INK2, va="top")

    # ---- waveform
    row = 0
    wax = []
    if stereo:
        sub = gl[0:2].subgridspec(2, 1, hspace=0.08)
        wax = [fig.add_subplot(sub[0]), fig.add_subplot(sub[1])]
        row = 2
    else:
        wax = [fig.add_subplot(gl[0])]
        row = 1
    clip_t = t0 + PD["clip_idx"] / sr
    for c, ax in enumerate(wax):
        _wave(ax, x[:, c], sr, t0, C_L if c == 0 else C_R)
        ax.set_ylim(-1.08, 1.08)
        ax.set_xlim(t0, t1)
        ax.set_yticks([-1, 0, 1])
        ax.axhline(1, color=C_GRID, lw=0.7)
        ax.axhline(-1, color=C_GRID, lw=0.7)
        ax.text(0.004, 0.86, ("L" if c == 0 else "R") if stereo else "mono", transform=ax.transAxes,
                fontsize=11, weight="bold", color=C_INK, va="top")
        if len(PD["clip_idx"]):
            idx = PD["clip_idx"]
            vals = x[idx, c]
            lim = 0.7 * np.max(np.abs(x[:, c])) if n else 1
            sel = np.abs(vals) >= max(lim, 1e-4)
            if sel.any():
                tt = np.unique(np.round(clip_t[sel] / dur * 1500)) * dur / 1500
                ax.plot(tt, np.full(len(tt), 1.0), "|", color=C_BAD, ms=9, mew=1.6, clip_on=False, zorder=5)
        if len(PD["click_t"]):
            ax.plot(PD["click_t"], np.full(len(PD["click_t"]), -1.0), "^", color=C_INK, ms=8,
                    clip_on=False, zorder=6)
        if ax is not wax[-1]:
            ax.set_xticklabels([])
    cl, ck = M["clipping"], M["clicks"]
    wax[0].set_title("Waveform (full scale = +-1)   red | = clipped (%d at FS, %d flat runs)   black ^ = click (%d)" % (
        cl["samples_at_full_scale"], cl["flat_runs"], ck["count"]))
    wax[-1].set_xticklabels([])

    # ---- spectrogram
    ax = fig.add_subplot(gl[row]); row += 1
    im = _spec_image(ax, PD["S"], PD["edges"], t0, t1)
    ax.set_xticklabels([])
    ax.set_title("Spectrogram, dB (window 93 ms < 300 Hz, 23 ms < 2.5 kHz, 6 ms above)"
                 + ("; white ticks = onsets" if 0 < len(PD["onsets"]) <= 600 else ""))
    if len(PD["onsets"]) and len(PD["onsets"]) <= 600:
        nbv = len(PD["edges"]) - 1
        ax.plot(PD["onsets"], np.full(len(PD["onsets"]), nbv * 0.985), "|", color="white", ms=7, mew=1.2)
    cax = fig.add_axes([0.634, ax.get_position().y0, 0.008, ax.get_position().height])
    cb = fig.colorbar(im, cax=cax)
    cb.ax.tick_params(labelsize=9.5)
    cb.outline.set_visible(False)
    spec_ax = ax

    # ---- low band vs rest envelope
    ax = fig.add_subplot(gl[row]); row += 1
    K = len(PD["env_low"])
    tt = t0 + np.arange(K) / PD["fps"]
    lo_db, re_db = db20(PD["env_low"]), db20(PD["env_rest"])
    top = max(float(np.max(lo_db)), float(np.max(re_db)), -80.0)
    ax.plot(tt, re_db, color=C_REST, lw=1.1, label="rest (> 150 Hz)")
    ax.plot(tt, lo_db, color=C_LOW, lw=1.3, label="low band (< 150 Hz)")
    ax.set_ylim(top - 48, top + 4)
    ax.set_xlim(t0, t1)
    ax.grid(True, axis="y")
    ax.set_ylabel("dBFS")
    ax.set_xticklabels([])
    ax.set_title("Band envelopes, RMS 40 ms:  purple = low < 150 Hz,  green = rest.  Ducking = dip after each kick")

    # ---- loudness
    ax = fig.add_subplot(gl[row]); row += 1
    ld = PD["loud"]
    integ = M["loudness"]["integrated_lufs"]
    vals = []
    if len(ld["tm"]):
        ax.plot(ld["tm"], ld["lm"], color="#86b6ef", lw=1.2, label="momentary (0.4 s)")
        vals.append(ld["lm"])
    if len(ld["ts"]):
        ax.plot(ld["ts"], ld["ls"], color="#184f95", lw=2.2, label="short-term (3 s)")
        vals.append(ld["ls"])
    if integ is not None:
        ax.axhline(integ, color=C_INK, lw=1.1, ls=(0, (5, 3)))
        ax.text(0.995, 0.93, "integrated %.1f LUFS" % integ, transform=ax.transAxes,
                ha="right", va="top", fontsize=10.5, color=C_INK, weight="bold")
        hi = max([integ] + [float(np.max(v)) for v in vals])
        lo = min([integ] + [float(np.percentile(v, 2)) for v in vals])
        ax.set_ylim(max(lo, hi - 30) - 3, hi + 4)
    if not vals:
        ax.text(0.5, 0.5, "file shorter than one 0.4 s loudness block", transform=ax.transAxes,
                ha="center", va="center", color=C_INK2)
        ax.set_title("Loudness (BS.1770)")
    else:
        ax.set_title("Loudness (BS.1770):  light = momentary 0.4 s,  dark = short-term 3 s,  dashed = integrated")
    ax.set_xlim(t0, t1)
    ax.grid(True, axis="y")
    ax.set_ylabel("LUFS")
    last = ax

    # ---- correlation strip
    if stereo:
        ax.set_xticklabels([])
        ax = fig.add_subplot(gl[row]); row += 1
        c = PD["corr"]
        if len(c["t"]):
            tc, r = t0 + c["t"], c["r"]
            ax.fill_between(tc, 0, np.where(np.nan_to_num(r) > 0, r, 0), color=C_L, alpha=0.55, lw=0)
            ax.fill_between(tc, 0, np.where(np.nan_to_num(r) < 0, r, 0), color=C_BAD, alpha=0.75, lw=0)
            ax.plot(tc, r, color=C_INK, lw=0.8)
        ax.axhline(0, color="#8a8984", lw=0.8)
        ax.set_ylim(-1.05, 1.05)
        ax.set_yticks([-1, 0, 1])
        ax.set_xlim(t0, t1)
        ax.set_title("L/R correlation over time (+1 mono, 0 wide, red below 0 = out of phase)")
        last = ax
    last.set_xlabel("time (s)")

    # ---- right column: key numbers
    gr = fig.add_gridspec(3, 2, left=0.728, right=0.985, top=0.905, bottom=0.05,
                          height_ratios=[3.0, 3.1, 2.4], hspace=0.36, wspace=0.42)
    ax = fig.add_subplot(gr[0, :])
    ax.axis("off")
    lines = _key_lines(M)
    y = 1.0
    import textwrap
    flags = []
    for fl in M["flags"][:5]:                  # short form: drop the explanation in brackets / after " - "
        short = fl.split(" (")[0].split(" - ")[0]
        flags += textwrap.wrap("! " + short, 44, subsequent_indent="  ")[:2]
    dy = min(0.062, 1.0 / (len(lines) + max(1, len(flags)) + 0.6))
    for s in lines:
        ax.text(-0.07, y, s, transform=ax.transAxes, family="DejaVu Sans Mono", fontsize=10.8, va="top")
        y -= dy
    y -= 0.012
    if flags:
        for fl in flags:
            ax.text(-0.07, y, fl, transform=ax.transAxes,
                    family="DejaVu Sans Mono", fontsize=10.8, va="top", color=C_BAD, weight="bold")
            y -= dy
    else:
        ax.text(-0.07, y, "Flags  none", transform=ax.transAxes, family="DejaVu Sans Mono",
                fontsize=10.8, va="top", color=C_INK2)

    # ---- average spectrum / harmonic spectrum
    ax = fig.add_subplot(gr[1, :])
    pp = PD["pitch"]
    if pp is not None and M["pitch"]:
        f, a, harm = pp["f"], pp["amp_db"], pp["harm"]
        top = float(np.max(a))
        ax.plot(f[1:], a[1:], color=C_R, lw=0.7)
        ah = np.where(harm, a, np.nan)
        ax.plot(f[1:], ah[1:], color=C_L, lw=1.1)
        _freq_axis(ax, sr)
        ymax = min(6.0, top + 14)
        ax.set_ylim(max(top - 110, -135), ymax)
        ax.set_title("Note spectrum (blue = harmonics of f0)")
        ax.set_ylabel("dBFS")
        _band_marks(ax, None, ymax, with_values=False)
    else:
        fc, e = PD["ltas"]
        top = float(np.max(e))
        ax.plot(fc, e, color=C_INK, lw=1.8)
        ax.fill_between(fc, e, top - 200, color="#cde2fb", lw=0)
        _freq_axis(ax, sr)
        ymax = top + 14
        ax.set_ylim(top - 62, ymax)
        ax.set_title("Average spectrum; bold = band dB re total")
        ax.set_ylabel("dBFS per 1/6 oct")
        _band_marks(ax, M, ymax)
    ax.grid(True, axis="y")

    # ---- bottom right: goniometer (stereo) or timing (mono) + beat-folded envelope
    axa = fig.add_subplot(gr[2, 0])
    if stereo:
        m = max(1, n // 150000)
        Lc, Rc = x[::m, 0], x[::m, 1]
        gx, gy = (Rc - Lc) / math.sqrt(2), (Lc + Rc) / math.sqrt(2)
        lim = max(float(np.max(np.abs(x))) * math.sqrt(2), 1e-3) * 1.05
        # odd bin count: a dual-mono file lands in the centre column. pcolormesh
        # draws every cell, so a one-bin-wide line cannot vanish when downscaled.
        H, be, _ = np.histogram2d(gx, gy, bins=81, range=[[-lim, lim], [-lim, lim]])
        Hl = np.log1p(H.T)
        axa.pcolormesh(be, be, np.ma.masked_where(Hl <= 0, Hl), cmap="Blues",
                       vmin=-0.5 * max(float(Hl.max()), 1e-9), vmax=float(Hl.max()),
                       shading="flat", rasterized=True, edgecolors="none")
        axa.set_xlim(-lim, lim); axa.set_ylim(-lim, lim); axa.set_aspect("equal")
        for a_, b_ in (((-lim, lim), (-lim, lim)), ((-lim, lim), (lim, -lim))):
            axa.plot(a_, b_, color=C_GRID, lw=0.8, zorder=0)
        axa.axhline(0, color=C_GRID, lw=0.8, zorder=0)
        axa.axvline(0, color=C_GRID, lw=0.8, zorder=0)
        axa.set_xticks([]); axa.set_yticks([])
        axa.text(0.03, 0.97, "L", transform=axa.transAxes, va="top", fontsize=11, weight="bold")
        axa.text(0.97, 0.97, "R", transform=axa.transAxes, va="top", ha="right", fontsize=11, weight="bold")
        axa.set_xlabel("side (anti-phase)")
        axa.set_ylabel("mid (mono)")
        axa.set_title("Goniometer")
        for s_ in ("top", "right"):
            axa.spines[s_].set_visible(True)
    else:
        dev = PD["grid_dev"]
        if dev is not None and M["grid"]:
            axa.axhline(0, color="#8a8984", lw=0.8)
            axa.plot(PD["onsets"], (dev) * 1e3, "o", color=C_L, ms=5, mec=C_SURF, mew=0.6)
            lim = max(5.0, float(np.max(np.abs(dev)) * 1e3) * 1.2)
            axa.set_ylim(-lim, lim)
            axa.set_xlabel("time (s)")
            axa.set_ylabel("ms")
            axa.grid(True, axis="y")
            axa.set_title("Onset - 16th grid")
        else:
            axa.axis("off")
            axa.text(0.5, 0.5, "no tempo:\nno grid stats", ha="center", va="center",
                     transform=axa.transAxes, color=C_INK2)
    axb = fig.add_subplot(gr[2, 1])
    prof = PD["prof"]
    if prof is not None:
        xs = prof["x"] * prof["T"] * 1e3
        axb.plot(xs, prof["rest"], color=C_REST, lw=1.6)
        axb.plot(xs, prof["low"], color=C_LOW, lw=2.0)
        top = max(float(np.max(prof["low"])), float(np.max(prof["rest"])))
        bot = min(float(np.min(prof["low"])), float(np.min(prof["rest"])))
        axb.set_ylim(max(bot, top - 45) - 3, top + 3)
        axb.set_xlim(0, prof["T"] * 1e3)
        axb.grid(True, axis="y")
        axb.set_xlabel("ms within beat")
        axb.set_ylabel("dBFS")
        axb.set_title("One beat, folded")
        axb.text(0.0, 1.0, "low", color=C_LOW, transform=axb.transAxes, ha="left", va="bottom",
                 fontsize=10.5, weight="bold")
        axb.text(1.0, 1.0, "rest", color=C_REST, transform=axb.transAxes, ha="right", va="bottom",
                 fontsize=10.5, weight="bold")
        axb.set_title("One beat, folded", pad=18)
    else:
        axb.axis("off")
        axb.text(0.5, 0.5, "no tempo:\nno beat-folded\nenvelope", ha="center", va="center",
                 transform=axb.transAxes, color=C_INK2)
    fig.savefig(out_png, dpi=100)
    plt.close(fig)


def render_zoom(path, args, out_png, title):
    """Detailed view of T0..T1: waveform (sample dots when short), sample-to-sample delta, spectrogram."""
    plt = _mpl()
    z0, z1 = args.zoom
    x, xi, sr, bits, total = read_wav(path)
    z0, z1 = max(0.0, min(z0, z1)), min(total, max(z0, z1))
    if z1 - z0 < 4.0 / sr:
        raise SystemExit("ears: zoom window is empty (file is %.3f s long)" % total)
    n, ch = x.shape
    i0, i1 = int(round(z0 * sr)), int(round(z1 * sr))
    seg = x[i0:i1]
    m = len(seg)
    ck, click_t = detect_clicks(x, sr, 0.0)
    _, clip_idx = clipping_metrics(x, xi, bits, sr, 0.0)
    fig = plt.figure(figsize=(16, 10), dpi=100)
    gs = fig.add_gridspec(2 + ch, 1, left=0.055, right=0.94, top=0.895, bottom=0.065,
                          height_ratios=[1.5] * ch + [1.0, 3.2], hspace=0.34)
    pk = float(np.max(np.abs(seg)))
    rms = float(np.sqrt(np.mean(seg ** 2)))
    dmax = float(np.max(np.abs(np.diff(seg, axis=0)))) if m > 1 else 0.0
    inwin = [e for e in ck["events"] if z0 <= e["t_s"] <= z1]
    fig.text(0.012, 0.975, "%s   zoom %.4f .. %.4f s" % (title, z0, z1), fontsize=18, weight="bold", va="top")
    fig.text(0.012, 0.94, "%d samples (%.2f ms) at %d Hz   peak %.2f dBFS   RMS %.1f dBFS   largest step %.4f (%.1f dBFS)   clicks in window: %d" % (
        m, (z1 - z0) * 1e3, sr, db20(pk), db20(rms), dmax, db20(dmax), len(inwin)),
        fontsize=11.5, color=C_INK2, va="top")
    t = z0 + np.arange(m) / sr
    ms_unit = (z1 - z0) < 0.2
    tx = (lambda v: (np.asarray(v) - z0) * 1e3) if ms_unit else (lambda v: np.asarray(v))
    xl = (0, (z1 - z0) * 1e3) if ms_unit else (z0, z1)
    yl = max(pk * 1.15, 1e-3)
    cidx = clip_idx[(clip_idx >= i0) & (clip_idx < i1)]
    ctin = click_t[(click_t >= z0) & (click_t <= z1)]
    for c in range(ch):
        ax = fig.add_subplot(gs[c])
        col = C_L if c == 0 else C_R
        if m <= 4500:
            ax.plot(tx(t), seg[:, c], color=col, lw=1.0, marker="o" if m <= 500 else None, ms=3.2)
        else:
            cols = 1500
            k = m // cols
            r = seg[: k * cols, c].reshape(cols, k)
            ax.fill_between(tx(z0 + (np.arange(cols) + 0.5) * k / sr), r.min(axis=1), r.max(axis=1),
                            color=col, lw=0.4, step="mid")
        ax.axhline(0, color="#8a8984", lw=0.7)
        if yl > 0.85:
            ax.axhline(1, color=C_GRID, lw=0.8)
            ax.axhline(-1, color=C_GRID, lw=0.8)
        ax.set_ylim(-yl, yl)
        ax.set_xlim(*xl)
        ax.grid(True, axis="y")
        lab = ("L" if c == 0 else "R") if ch == 2 else "mono"
        ax.text(0.004, 0.9, lab, transform=ax.transAxes, fontsize=11, weight="bold", va="top")
        if len(cidx):
            sel = np.abs(x[cidx, c]) >= 0.7 * max(float(np.max(np.abs(x[:, c]))), 1e-9)
            if sel.any():
                ax.plot(tx(cidx[sel] / sr), x[cidx[sel], c], "s", color=C_BAD, ms=4.5, zorder=5)
        for tc in ctin:
            ax.axvline(tx(tc), color=C_INK, lw=1.0, ls=(0, (2, 2)), zorder=4)
        if c == 0:
            ax.set_title("Waveform%s   red squares = clipped samples   dotted line = detected click" % (
                " (dots = samples)" if m <= 500 else ""))
        ax.set_xticklabels([])
    ax = fig.add_subplot(gs[ch])
    d = np.abs(np.diff(seg, axis=0)).max(axis=1) if m > 1 else np.zeros(1)
    dd = db20(np.maximum(d, 1e-6))
    td = tx(t[1:]) if m > 1 else tx(t)
    if len(dd) > 4500:
        cols = 1500
        k = len(dd) // cols
        dd2 = dd[: k * cols].reshape(cols, k).max(axis=1)
        ax.plot(tx(z0 + (np.arange(cols) + 0.5) * k / sr), dd2, color=C_INK, lw=0.9)
    else:
        ax.plot(td, dd, color=C_INK, lw=0.9)
    ax.set_xlim(*xl)
    ax.set_ylim(max(float(np.max(dd)) - 70, -110), float(np.max(dd)) + 5)
    ax.grid(True, axis="y")
    ax.set_ylabel("dBFS")
    ax.set_title("Sample-to-sample step |x[n] - x[n-1]| (a click is one spike far above its surroundings)")
    ax.set_xticklabels([])
    # spectrogram with a window sized to the zoom length
    win = int(2 ** np.clip(round(math.log2(max(m / 12.0, 1))), 6, 11))
    hop = max(1, m // 1200)
    nfft = max(2048, win * 4)
    padl = win // 2
    a0 = i0 - padl
    need = (m // hop) * hop + win
    buf = np.zeros((need, ch), np.float32)
    s0, s1 = max(a0, 0), min(n, a0 + need)
    buf[s0 - a0: s1 - a0] = x[s0:s1]
    K = m // hop + 1
    fmax = min(20000.0, sr / 2 * 0.98)
    fmin = 20.0
    nb = int(round(18 * math.log2(fmax / fmin)))
    edges = np.geomspace(fmin, fmax, nb + 1)
    W = log_band_matrix(edges, nfft // 2 + 1, sr / nfft, np.ones(nb, bool))
    S = np.zeros((K, nb), np.float32)
    for c in range(ch):
        xp = np.concatenate((buf[:, c], np.zeros(win, np.float32)))
        w = signal.get_window("hann", win, fftbins=True).astype(np.float32)
        fr = sliding_window_view(xp, win)[::hop][:K]
        X = sfft.rfft(fr * w, n=nfft, axis=1)
        S += ((X.real ** 2 + X.imag ** 2) * (2.0 / (sr * float(np.sum(w * w))))) @ W.T
    S /= ch
    ax = fig.add_subplot(gs[ch + 1])
    im = _spec_image(ax, S, edges, xl[0], xl[1], dyn=85.0)
    ax.set_xlabel("time (ms from %.4f s)" % z0 if ms_unit else "time (s)")
    ax.set_title("Spectrogram, log frequency, dB (window %d samples = %.1f ms; nothing is resolved below ~%.0f Hz)" % (
        win, win / sr * 1e3, 2.0 * sr / win))
    cax = fig.add_axes([0.95, ax.get_position().y0, 0.008, ax.get_position().height])
    cb = fig.colorbar(im, cax=cax)
    cb.outline.set_visible(False)
    fig.savefig(out_png, dpi=100)
    plt.close(fig)
    print("== ears zoom: %s  %.4f..%.4f s ==" % (os.path.basename(path), z0, z1))
    print("window   : %d samples, peak %.2f dBFS, RMS %.1f dBFS, largest step %.4f (%.1f dBFS)" % (
        m, db20(pk), db20(rms), dmax, db20(dmax)))
    print("clipped  : %d marked samples in window" % len(cidx))
    print("clicks   : %d in window" % len(inwin))
    for e in inwin[:8]:
        print("           %.5f s  %s  jump %s dBFS  %s" % (e["t_s"], e["channel"], fnum(e["jump_dbfs"]), e["kind"]))
    print("wrote    : %s" % out_png)


# ------------------------------------------------------------------ compare

def _get(M, *keys):
    v = M
    for k in keys:
        if v is None:
            return None
        v = v.get(k) if isinstance(v, dict) else None
    return v


COMPARE_ROWS = [
    ("duration s", ("duration_s",), "{:.3f}"),
    ("sample peak dBFS", ("level", "sample_peak_dbfs"), "{:.2f}"),
    ("true peak dBTP", ("level", "true_peak_dbtp"), "{:.2f}"),
    ("samples at FS", ("clipping", "samples_at_full_scale"), "{:.0f}"),
    ("flat-topped runs", ("clipping", "flat_runs"), "{:.0f}"),
    ("RMS dBFS", ("level", "rms_dbfs"), "{:.2f}"),
    ("crest factor dB", ("level", "crest_factor_db"), "{:.2f}"),
    ("integrated LUFS", ("loudness", "integrated_lufs"), "{:.2f}"),
    ("short-term max LUFS", ("loudness", "short_term_max_lufs"), "{:.2f}"),
    ("loudness range LU", ("loudness", "loudness_range_lu"), "{:.2f}"),
    ("band sub dB", ("spectrum", "bands_db_rel_total", "sub"), "{:.1f}"),
    ("band bass dB", ("spectrum", "bands_db_rel_total", "bass"), "{:.1f}"),
    ("band low-mid dB", ("spectrum", "bands_db_rel_total", "low-mid"), "{:.1f}"),
    ("band mid dB", ("spectrum", "bands_db_rel_total", "mid"), "{:.1f}"),
    ("band presence dB", ("spectrum", "bands_db_rel_total", "presence"), "{:.1f}"),
    ("band air dB", ("spectrum", "bands_db_rel_total", "air"), "{:.1f}"),
    ("centroid Hz", ("spectrum", "centroid_hz"), "{:.0f}"),
    ("rolloff 85% Hz", ("spectrum", "rolloff85_hz"), "{:.0f}"),
    ("stereo correlation", ("stereo", "correlation"), "{:+.3f}"),
    ("low-band correlation", ("stereo", "low_band_correlation"), "{:+.3f}"),
    ("side/mid dB", ("stereo", "side_mid_db"), "{:.1f}"),
    ("balance L-R dB", ("stereo", "balance_l_minus_r_db"), "{:+.2f}"),
    ("mono-sum change dB", ("stereo", "mono_sum_change_db"), "{:+.1f}"),
    ("clicks", ("clicks", "count"), "{:.0f}"),
    ("hard attacks", ("clicks", "hard_attacks"), "{:.0f}"),
    ("recurring edges", ("clicks", "recurring_edges"), "{:.0f}"),
    ("onsets", ("rhythm", "onset_count"), "{:.0f}"),
    ("tempo estimate BPM", ("rhythm", "tempo_bpm_estimate"), "{:.2f}"),
    ("grid jitter sd ms", ("grid", "jitter_sd_ms"), "{:.2f}"),
    ("grid offset ms", ("grid", "offset_ms"), "{:+.2f}"),
    ("LF mod depth @beat", ("pumping", "lf_mod_depth_beat"), "{:.2f}"),
    ("kick punch dB", ("pumping", "kick_punch_db"), "{:.1f}"),
    ("duck dip low dB", ("pumping", "lf_duck_dip_db"), "{:.1f}"),
    ("duck dip rest dB", ("pumping", "rest_duck_dip_db"), "{:.1f}"),
    ("f0 Hz", ("pitch", "f0_hz"), "{:.2f}"),
    ("cents off note", ("pitch", "cents"), "{:+.1f}"),
    ("THD dB", ("pitch", "thd_db"), "{:.1f}"),
    ("inharmonic dB", ("pitch", "inharmonic_db_rel_harmonic"), "{:.1f}"),
]


def compare_rows(MA, MB):
    rows = []
    for label, keys, fmt in COMPARE_ROWS:
        a, b = _get(MA, *keys), _get(MB, *keys)
        if a is None and b is None:
            continue
        d = (b - a) if (a is not None and b is not None) else None
        dfmt = fmt if fmt.startswith("{:+") else "{:+" + fmt[2:]
        rows.append((label, fnum(a, fmt, "-"), fnum(b, fmt, "-"), fnum(d, dfmt, ""), a, b, d))
    return rows


def render_compare(MA, PA, MB, PB, rows, out_png, title):
    plt = _mpl()
    fig = plt.figure(figsize=(16, 10), dpi=100)
    fig.text(0.012, 0.975, title, fontsize=18, weight="bold", va="top")
    fig.text(0.012, 0.94, "A = %s      B = %s" % (MA["file"], MB["file"]), fontsize=11.5, color=C_INK2, va="top")
    gs = fig.add_gridspec(2, 1, left=0.055, right=0.60, top=0.895, bottom=0.07, height_ratios=[2.4, 1], hspace=0.2)
    ax = fig.add_subplot(gs[0])
    fa, ea = PA["ltas"]
    fb, eb = PB["ltas"]
    sr = min(PA["sr"], PB["sr"])
    ax.plot(fa, ea, color=C_L, lw=2.0, label="A  " + MA["file"])
    ax.plot(fb, eb, color=C_R, lw=2.0, label="B  " + MB["file"])
    _freq_axis(ax, sr)
    top = max(float(np.max(ea)), float(np.max(eb)))
    ymax = top + 12
    ax.set_ylim(top - 75, ymax)
    ax.grid(True, axis="y")
    ax.set_ylabel("dBFS per 1/6 oct")
    ax.set_xlabel("")
    ax.set_xticklabels([])
    ax.set_title("Average spectra (1/6-octave smoothed)")
    ax.legend(loc="lower left")
    _band_marks(ax, None, ymax, with_values=False)
    ax2 = fig.add_subplot(gs[1])
    fcom = np.geomspace(20, min(20000.0, sr / 2 * 0.98), 220)
    d = np.interp(np.log(fcom), np.log(fb), eb) - np.interp(np.log(fcom), np.log(fa), ea)
    quiet = np.maximum(np.interp(np.log(fcom), np.log(fb), eb), np.interp(np.log(fcom), np.log(fa), ea)) < top - 75
    d = np.where(quiet, np.nan, d)
    ax2.axhline(0, color="#8a8984", lw=0.9)
    ax2.fill_between(fcom, 0, np.where(np.nan_to_num(d) > 0, d, 0), color=C_R, alpha=0.5, lw=0)
    ax2.fill_between(fcom, 0, np.where(np.nan_to_num(d) < 0, d, 0), color=C_L, alpha=0.5, lw=0)
    ax2.plot(fcom, d, color=C_INK, lw=1.4)
    _freq_axis(ax2, sr)
    lim = float(np.nanmax(np.abs(d))) if np.isfinite(d).any() else 1.0
    lim = min(max(6.0, lim * 1.15), 60.0)
    ax2.set_ylim(-lim, lim)
    ax2.grid(True, axis="y")
    ax2.set_ylabel("dB")
    ax2.set_title("Difference B - A (orange: B has more, blue: A has more; hidden where both are > 75 dB down)")
    for name, lo, hi in BANDS[1:]:
        ax2.axvline(lo, color="#8a8984", lw=0.9, ls=(0, (4, 3)), zorder=1)
    axt = fig.add_axes([0.635, 0.05, 0.36, 0.86])
    axt.axis("off")
    y = 1.0
    step = min(0.034, 0.96 / (len(rows) + 2))
    fs = 10.6
    axt.text(0, y, "%-20s %9s %9s %9s" % ("metric", "A", "B", "B-A"), family="DejaVu Sans Mono",
             fontsize=fs, weight="bold", va="top", transform=axt.transAxes)
    y -= step * 1.2
    for label, a, b, dlt, *_ in rows:
        axt.text(0, y, "%-20s %9s %9s %9s" % (label, a, b, dlt), family="DejaVu Sans Mono", fontsize=fs,
                 va="top", transform=axt.transAxes)
        y -= step
    fig.savefig(out_png, dpi=100)
    plt.close(fig)


# --------------------------------------------------------------------- main

def base_name(path):
    return os.path.splitext(os.path.basename(path))[0]


def out_dir(args, path):
    d = args.out or os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    return d


def main(argv=None):
    ap = argparse.ArgumentParser(prog="ears.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="?", help="WAV file to analyse")
    ap.add_argument("--out", metavar="DIR", help="output directory (default: next to the WAV)")
    ap.add_argument("--bpm", type=float, metavar="N", help="known tempo: report onset deviation from its 16th grid")
    ap.add_argument("--start", type=float, default=0.0, metavar="S", help="analyse from S seconds")
    ap.add_argument("--dur", type=float, default=None, metavar="S", help="analyse only S seconds")
    ap.add_argument("--title", metavar="T", help="figure title (default: file name)")
    ap.add_argument("--zoom", nargs=2, type=float, metavar=("T0", "T1"),
                    help="render only a detailed view of T0..T1 seconds")
    ap.add_argument("--compare", nargs=2, metavar=("A.wav", "B.wav"), help="compare two files")
    ap.add_argument("--pitch", action="store_true", help="force single-note pitch/inharmonicity analysis")
    ap.add_argument("--no-png", action="store_true", help="skip the figure (JSON + text only)")
    args = ap.parse_args(argv)

    if args.compare:
        a, b = args.compare
        MA, PA = analyze(a, args)
        MB, PB = analyze(b, args)
        rows = compare_rows(MA, MB)
        d = out_dir(args, a)
        stem = os.path.join(d, "%s_vs_%s" % (base_name(a), base_name(b)))
        print("== ears compare: A = %s   B = %s ==" % (MA["file"], MB["file"]))
        print("%-22s %10s %10s %10s" % ("metric", "A", "B", "B-A"))
        for label, sa, sb, sd, *_ in rows:
            print("%-22s %10s %10s %10s" % (label, sa, sb, sd))
        for tag, M in (("A", MA), ("B", MB)):
            print("flags %s  : %s" % (tag, "; ".join(M["flags"]) if M["flags"] else "none"))
        with open(stem + ".json", "w") as fh:
            json.dump(clean({"a": MA, "b": MB,
                             "diff_b_minus_a": {r[0]: {"a": r[4], "b": r[5], "diff": r[6]} for r in rows}}),
                      fh, indent=1)
        outs = [stem + ".json"]
        if not args.no_png:
            render_compare(MA, PA, MB, PB, rows, stem + ".png",
                           args.title or "%s vs %s" % (base_name(a), base_name(b)))
            outs.append(stem + ".png")
        print("wrote     : " + ", ".join(outs))
        return 0

    if not args.wav:
        ap.error("give a WAV file (or --compare A.wav B.wav)")
    d = out_dir(args, args.wav)
    stem = os.path.join(d, base_name(args.wav))
    title = args.title or os.path.basename(args.wav)
    if args.zoom:
        z0, z1 = sorted(args.zoom)
        render_zoom(args.wav, args, "%s.zoom_%g-%g.png" % (stem, z0, z1), title)
        return 0
    M, PD = analyze(args.wav, args)
    with open(stem + ".json", "w") as fh:
        json.dump(clean(M), fh, indent=1)
    print(summary_text(M))
    outs = [stem + ".json"]
    if not args.no_png:
        render_main(M, PD, stem + ".png", title)
        outs.append(stem + ".png")
    print("wrote    : " + ", ".join(outs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
