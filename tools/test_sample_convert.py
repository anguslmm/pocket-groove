"""The two sample converters agree (ctest test_sample_convert).

    python3 tools/test_sample_convert.py build/host/sample_convert

Writes shared test vectors (WAV files in a temporary folder: 16 / 24 / 8 / 32-bit
PCM, float32, WAVE_FORMAT_EXTENSIBLE, mono / stereo, 8 to 48 kHz, leading silence,
one longer than a slot) and converts each with the C++ converter
(host/common/sample_convert.h, through host/tools/sample_convert) and with the
Python one (tools/pg_sample.py, with numpy when installed and in plain Python):
the samples must match within 1 LSB, the lengths exactly, and each result must
have the expected length and a -1 dBFS peak.
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pg_sample  # noqa: E402


def lcg(seed):
    s = seed
    while True:
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        yield (s / 2147483648.0) - 1.0


def write(path, rate, frames, tag, bits, ext=False):
    ch = len(frames[0])
    nb = bits // 8
    out = bytearray()
    for fr in frames:
        for v in fr:
            v = max(-1.0, min(1.0, v))
            if tag == 3:
                out += struct.pack("<f", v)
            elif bits == 8:
                out.append(int(round(v * 127.0)) + 128)
            elif bits == 16:
                out += struct.pack("<h", int(round(v * 32767.0)))
            elif bits == 24:
                out += int(round(v * 8388607.0)).to_bytes(3, "little", signed=True)
            else:
                out += struct.pack("<i", int(round(v * 2147483647.0)))
    if ext:
        fmt = struct.pack("<HHIIHHHHIH14s", 0xFFFE, ch, rate, rate * ch * nb, ch * nb, bits, 22, bits, 0, tag,
                          b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71")
    else:
        fmt = struct.pack("<HHIIHH", tag, ch, rate, rate * ch * nb, ch * nb, bits)
    data = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"LIST" + struct.pack("<I", 4) + b"INFO"
    data += b"data" + struct.pack("<I", len(out)) + bytes(out)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(data)) + b"WAVE" + data)


def vectors():
    v = []
    r = 44100
    fr = [(0.0, 0.0)] * int(0.2 * r)
    fr += [(0.5 * math.sin(2 * math.pi * 440 * i / r), 0.3 * math.sin(2 * math.pi * 660 * i / r)) for i in range(r)]
    v.append(("st16_44k", r, fr, 1, 16, False, None))
    r = 48000
    fr = [(0.7 * math.sin(2 * math.pi * (200 + 3000 * i / (0.8 * r)) * i / r),) for i in range(int(0.8 * r))]
    v.append(("mono24_48k", r, fr, 1, 24, False, None))
    r = 32000
    g = lcg(7)
    fr = [(0.4 * next(g), 0.4 * next(g)) for _ in range(int(0.5 * r))]
    v.append(("float32_32k", r, fr, 3, 32, False, None))
    r = 11025
    fr = [(0.6 if (i // 25) % 2 else -0.6,) for i in range(int(0.6 * r))]
    v.append(("u8_11k", r, fr, 1, 8, False, None))
    r = 22050
    fr = [(0.25 * math.sin(2 * math.pi * 1000 * i / r), 0.25 * math.sin(2 * math.pi * 1500 * i / r)) for i in range(int(0.3 * r))]
    v.append(("ext16_22k", r, fr, 1, 16, True, None))
    r = 8000
    g = lcg(11)
    fr = [(0.5 * math.sin(2 * math.pi * 300 * i / r) + 0.1 * next(g),) for i in range(7 * r)]
    v.append(("long32_8k", r, fr, 1, 32, False, pg_sample.SLOT_MAX_SAMPLES))
    return v


def main(argv):
    if len(argv) != 2:
        print("usage: test_sample_convert.py PATH/TO/sample_convert", file=sys.stderr)
        return 2
    tool = argv[1]
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        for name, rate, frames, tag, bits, ext, want_len in vectors():
            path = os.path.join(d, name + ".wav")
            write(path, rate, frames, tag, bits, ext)
            raw = os.path.join(d, name + ".raw")
            r = subprocess.run([tool, path, raw], capture_output=True, text=True)
            if r.returncode != 0:
                print("FAIL %s: C++ converter: %s" % (name, r.stderr.strip()))
                fails += 1
                continue
            with open(raw, "rb") as f:
                b = f.read()
            cpp = list(struct.unpack("<%dh" % (len(b) // 2), b))
            modes = [("numpy", pg_sample.np)] if pg_sample.np is not None else []
            if want_len is None:
                modes.append(("python", None))
            saved = pg_sample.np
            for mode, npmod in modes:
                pg_sample.np = npmod
                try:
                    py, _ = pg_sample.convert_file(path)
                finally:
                    pg_sample.np = saved
                worst = max((abs(a - c) for a, c in zip(py, cpp)), default=0)
                ok = len(py) == len(cpp) and worst <= 1
                print("%s %-12s %-6s %6d samples, largest difference %d LSB" % ("ok  " if ok else "FAIL", name, mode,
                                                                               len(cpp), worst))
                fails += 0 if ok else 1
            peak = max(abs(x) for x in cpp)
            exp_len = want_len if want_len else None
            ok = 29190 <= peak <= 29200 and (exp_len is None or len(cpp) == exp_len)
            if name == "st16_44k":   # 0.2 s of silence trimmed to 5 ms before the tone (the file ends with it)
                ok = ok and abs(len(cpp) - (22050 + 110)) <= 2
            if not ok:
                print("FAIL %s: peak %d, length %d" % (name, peak, len(cpp)))
                fails += 1
    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
