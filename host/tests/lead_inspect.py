#!/usr/bin/env python3
"""Look at the lead-voice audition renders without ears.

  python3 host/tests/lead_inspect.py [build/renders/lead] [name-filter]

For every WAV: a PNG (log-frequency spectrogram of L+R, level envelope of L and
R, short-time L/R correlation) next to it in png/, and one line of statistics.
Needs numpy, scipy, matplotlib.
"""
import glob, os, sys
import numpy as np
from scipy.io import wavfile
from scipy import signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

def db(x): return 20 * np.log10(np.maximum(x, 1e-9))

def inspect(path, outdir):
    fs, d = wavfile.read(path)
    x = d.astype(np.float64) / 32768.0
    L, R = x[:, 0], x[:, 1]
    M, S = 0.5 * (L + R), 0.5 * (L - R)
    name = os.path.splitext(os.path.basename(path))[0]
    f, t, Z = signal.stft(M, fs, nperseg=4096, noverlap=4096 - 512, window="hann")
    P = np.abs(Z)
    spec = np.mean(P ** 2, axis=1)
    centroid = np.sum(f * spec) / max(np.sum(spec), 1e-20)
    hop = int(fs * 0.005)
    n = len(L) // hop
    envL = np.abs(L[: n * hop]).reshape(n, hop).max(axis=1)
    envR = np.abs(R[: n * hop]).reshape(n, hop).max(axis=1)
    w = int(fs * 0.05)
    k = len(L) // w
    corr = []
    for i in range(k):
        a, b = L[i * w:(i + 1) * w], R[i * w:(i + 1) * w]
        den = np.sqrt(np.sum(a * a) * np.sum(b * b))
        corr.append(np.sum(a * b) / den if den > 1e-9 else np.nan)
    corr = np.array(corr)
    loud = np.abs(M) > 10 ** (-50 / 20)
    stats = dict(
        peak=db(np.max(np.abs(x))), rmsL=db(np.sqrt(np.mean(L ** 2))), rmsR=db(np.sqrt(np.mean(R ** 2))),
        crest=db(np.max(np.abs(x))) - db(np.sqrt(np.mean(x[loud] ** 2))) if loud.any() else 0,
        dc=np.mean(M), centroid=centroid, corr=np.nanmean(corr),
        side=db(np.sqrt(np.mean(S ** 2))) - db(np.sqrt(np.mean(M ** 2))),
        maxstep=np.max(np.abs(np.diff(L))),
    )
    print(f"{name:28s} peak {stats['peak']:6.1f}  rmsL {stats['rmsL']:6.1f} rmsR {stats['rmsR']:6.1f}  "
          f"crest {stats['crest']:4.1f}  centroid {stats['centroid']:6.0f} Hz  L/R corr {stats['corr']:5.2f}  "
          f"S/M {stats['side']:5.1f} dB  DC {stats['dc']:+.5f}  maxstep {stats['maxstep']:.3f}")
    fig, ax = plt.subplots(3, 1, figsize=(14, 9), gridspec_kw={"height_ratios": [3, 1.3, 0.8]}, sharex=True)
    ax[0].pcolormesh(t, f[1:], db(P[1:]), vmin=-110, vmax=-20, shading="auto", cmap="magma")
    ax[0].set_yscale("log"); ax[0].set_ylim(40, fs / 2); ax[0].set_ylabel("Hz"); ax[0].set_title(name)
    te = np.arange(n) * hop / fs
    ax[1].plot(te, db(envL), lw=0.7, label="L"); ax[1].plot(te, db(envR), lw=0.7, label="R")
    ax[1].set_ylim(-70, 0); ax[1].set_ylabel("peak dBFS"); ax[1].grid(alpha=0.3); ax[1].legend(loc="upper right")
    ax[2].plot(np.arange(k) * w / fs, corr, lw=0.8); ax[2].set_ylim(-1, 1.05); ax[2].set_ylabel("L/R corr")
    ax[2].grid(alpha=0.3); ax[2].set_xlabel("s")
    fig.tight_layout()
    os.makedirs(outdir, exist_ok=True)
    fig.savefig(os.path.join(outdir, name + ".png"), dpi=70)
    plt.close(fig)

if __name__ == "__main__":
    d = sys.argv[1] if len(sys.argv) > 1 else "build/renders/lead"
    flt = sys.argv[2] if len(sys.argv) > 2 else ""
    for p in sorted(glob.glob(os.path.join(d, "*.wav"))):
        if flt in p:
            inspect(p, os.path.join(d, "png"))
