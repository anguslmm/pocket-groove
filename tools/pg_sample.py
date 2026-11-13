"""Audio file -> a Pocket Groove sample pool slot's audio: mono int16 at 22,050 Hz,
trimmed and normalised. The Python twin of host/common/sample_convert.h (the
desktop app's converter); tools/test_sample_convert.py checks that the two give
the same samples (within 1 LSB) on shared test vectors. Keep the two in step:
the algorithm is spelled out in sample_convert.h.

    python3 tools/pg_sample.py IN.wav OUT.raw     # raw int16 little-endian
    python3 tools/pg_sample.py IN.wav OUT.wav     # a mono 16-bit WAV at 22,050 Hz

Reads WAV itself (PCM 8/16/24/32-bit, float 32/64, any channels and rate). Any
other file on macOS goes through `afconvert` first (when it exists). Uses numpy
when it is installed (fast), plain Python otherwise (a few seconds per file).
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

RATE = 22050
SLOT_MAX_SAMPLES = 137216     # pg/sampler/sample_format.h kSlotMaxSamples
MIN_SAMPLES = 64
NAME_LEN = 8

try:  # optional, only for speed
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


class ConvertError(Exception):
    pass


def parse_wav(data):
    """(rate, channels, mono samples as floats) from a WAV file's bytes."""
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ConvertError("not a WAV file")
    fmt = None
    body = None
    p = 12
    while p + 8 <= len(data):
        cid = data[p:p + 4]
        ln = struct.unpack_from("<I", data, p + 4)[0]
        start = p + 8
        avail = len(data) - start
        if cid == b"fmt " and ln >= 16 and avail >= 16:
            tag, ch, rate, _, block, bits = struct.unpack_from("<HHIIHH", data, start)
            if tag == 0xFFFE and ln >= 40 and avail >= 26:
                tag = struct.unpack_from("<H", data, start + 24)[0]
            fmt = (tag, ch, rate, block, bits)
        elif cid == b"data":
            body = data[start:start + min(ln, avail)]
        p = start + ln + (ln & 1)
        if ln > len(data):
            break
    if fmt is None or body is None:
        raise ConvertError("no fmt / data chunk")
    tag, ch, rate, block, bits = fmt
    pcm = tag == 1 and bits in (8, 16, 24, 32)
    flt = tag == 3 and bits in (32, 64)
    if not (pcm or flt):
        raise ConvertError("unsupported WAV encoding (format %d, %d bits)" % (tag, bits))
    if not ch or rate < 1000 or rate > 384000 or ch > 32 or block < ch * bits // 8:
        raise ConvertError("implausible WAV header")
    frames = len(body) // block
    nb = bits // 8
    vals = []
    if np is not None:
        raw = np.frombuffer(body[:frames * block], dtype=np.uint8).reshape(frames, block)[:, :ch * nb]
        raw = np.ascontiguousarray(raw)
        if flt:
            x = raw.view("<f4" if bits == 32 else "<f8").astype(np.float64)
        elif bits == 8:
            x = (raw.astype(np.float64) - 128.0) / 128.0
        elif bits == 16:
            x = raw.view("<i2").astype(np.float64) / 32768.0
        elif bits == 24:
            r = raw.reshape(frames, ch, 3).astype(np.int32)
            v = (r[:, :, 0] << 8) | (r[:, :, 1] << 16) | (r[:, :, 2] << 24)
            x = (v >> 8).astype(np.float64) / 8388608.0
        else:
            x = raw.view("<i4").astype(np.float64) / 2147483648.0
        x = x.reshape(frames, ch)
        x[~np.isfinite(x)] = 0.0
        vals = list(x.sum(axis=1) / ch) if ch > 1 else list(x[:, 0])
        return rate, ch, vals
    for i in range(frames):
        s = 0.0
        for c in range(ch):
            o = i * block + c * nb
            if flt:
                v = struct.unpack_from("<f" if bits == 32 else "<d", body, o)[0]
            elif bits == 8:
                v = (body[o] - 128) / 128.0
            elif bits == 16:
                v = struct.unpack_from("<h", body, o)[0] / 32768.0
            elif bits == 24:
                v = int.from_bytes(body[o:o + 3], "little", signed=True) / 8388608.0
            else:
                v = struct.unpack_from("<i", body, o)[0] / 2147483648.0
            if not math.isfinite(v):
                v = 0.0
            s += v
        vals.append(s / ch)
    return rate, ch, vals


def _blackman(u):
    if u <= -1.0 or u >= 1.0:
        return 0.0
    return 0.42 + 0.5 * math.cos(math.pi * u) + 0.08 * math.cos(2.0 * math.pi * u)


def _sinc(x):
    if x == 0.0:
        return 1.0
    return math.sin(math.pi * x) / (math.pi * x)


def resample(x, rate):
    if rate == RATE:
        return list(x)
    ratio = RATE / rate
    s = min(ratio, 1.0)
    fc = 0.5 * 0.95 * s
    W = int(math.ceil(16.0 / s))
    n = len(x) * RATE // rate
    kernels = {}

    def kernel(ph):
        k = kernels.get(ph)
        if k is None:
            frac = ph / RATE
            k = []
            for j in range(2 * W):
                t = (j - W + 1) - frac
                k.append(2.0 * fc * _sinc(2.0 * fc * t) * _blackman(t / W))
            kernels[ph] = k
        return k

    out = [0.0] * n
    if np is not None:
        xa = np.concatenate([np.zeros(W), np.asarray(x, dtype=np.float64), np.zeros(W)])
        inside = np.concatenate([np.zeros(W), np.ones(len(x)), np.zeros(W)])
        idx = np.arange(n, dtype=np.int64)
        num = idx * rate
        base = num // RATE
        ph = num % RATE
        phases = np.unique(ph)
        table = np.array([kernel(int(p)) for p in phases])
        row = np.searchsorted(phases, ph)
        offs = np.arange(2 * W)
        for a in range(0, n, 4096):
            b = min(n, a + 4096)
            pos = (base[a:b] - W + 1)[:, None] + offs[None, :] + W   # into the padded arrays
            k = table[row[a:b]]
            m = inside[pos]
            w = k * m
            acc = (w * xa[pos]).sum(axis=1)
            ws = w.sum(axis=1)
            res = np.where(ws != 0.0, acc / np.where(ws != 0.0, ws, 1.0), 0.0)
            out[a:b] = res.tolist()
        return out
    L = len(x)
    for i in range(n):
        num = i * rate
        base, ph = divmod(num, RATE)
        k = kernel(ph)
        acc = 0.0
        ws = 0.0
        start = base - W + 1
        for j in range(2 * W):
            idx = start + j
            if 0 <= idx < L:
                acc += k[j] * x[idx]
                ws += k[j]
        out[i] = acc / ws if ws != 0.0 else 0.0
    return out


def to_slot(mono, rate):
    """Steps 3-7 of sample_convert.h: a list of int16 values."""
    x = resample(mono, rate)
    peak = max((abs(v) for v in x), default=0.0)
    if not peak > 0.0:
        raise ConvertError("silent")
    thr = peak * 10.0 ** (-48.0 / 20.0)
    first = 0
    while first < len(x) and not abs(x[first]) > thr:
        first += 1
    last = len(x) - 1
    while last > first and not abs(x[last]) > thr:
        last -= 1
    a = max(0, first - 110)
    b = min(len(x), last + 1 + 441)
    y = x[a:b]
    if len(y) > SLOT_MAX_SAMPLES:
        y = y[:SLOT_MAX_SAMPLES]
        L = len(y)
        for j in range(110):
            y[L - 110 + j] *= (110 - 1 - j) / 110.0
    p2 = max((abs(v) for v in y), default=0.0)
    if not p2 > 0.0 or len(y) < MIN_SAMPLES:
        raise ConvertError("too short or silent")
    gain = min(0.891 / p2, 100.0)
    out = []
    for v in y:
        q = math.floor(v * gain * 32767.0 + 0.5)
        out.append(32767 if q > 32767 else (-32768 if q < -32768 else int(q)))
    return out


def clean_name(stem):
    """The 8-character name the device keeps (sample_format.h cleanName)."""
    n = "".join(c for c in stem.upper() if c.isascii() and (c.isalnum() or c in "-_"))
    return n[:NAME_LEN]


def convert_file(path):
    """(int16 list, cleaned name) for any audio file: WAV here, others through afconvert."""
    with open(path, "rb") as f:
        data = f.read()
    tmp = None
    try:
        if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
            tool = shutil.which("afconvert")
            if not tool:
                raise ConvertError("not a WAV file (other formats need macOS's afconvert)")
            fd, tmp = tempfile.mkstemp(suffix=".wav")
            os.close(fd)
            r = subprocess.run([tool, "-f", "WAVE", "-d", "LEI24", path, tmp], capture_output=True)
            if r.returncode != 0:
                raise ConvertError("afconvert could not read it: " + r.stderr.decode(errors="replace").strip())
            with open(tmp, "rb") as f:
                data = f.read()
        rate, _, mono = parse_wav(data)
        return to_slot(mono, rate), clean_name(os.path.splitext(os.path.basename(path))[0])
    finally:
        if tmp:
            os.unlink(tmp)


def write_wav(path, samples, rate=RATE):
    with open(path, "wb") as f:
        n = len(samples)
        f.write(b"RIFF" + struct.pack("<I", 36 + 2 * n) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, 2 * rate, 2, 16))
        f.write(b"data" + struct.pack("<I", 2 * n) + struct.pack("<%dh" % n, *samples))


def main(argv):
    if len(argv) != 3:
        print("usage: pg_sample.py IN OUT.raw|OUT.wav", file=sys.stderr)
        return 2
    try:
        s, name = convert_file(argv[1])
    except (ConvertError, OSError) as e:
        print("pg_sample: %s: %s" % (argv[1], e), file=sys.stderr)
        return 1
    if argv[2].endswith(".wav"):
        write_wav(argv[2], s)
    else:
        with open(argv[2], "wb") as f:
            f.write(struct.pack("<%dh" % len(s), *s))
    print("%s: %d samples (%.2f s), peak %d, name %s" % (argv[2], len(s), len(s) / RATE, max(abs(v) for v in s), name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
