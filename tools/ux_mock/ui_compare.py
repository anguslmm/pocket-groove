#!/usr/bin/env python3
"""Compare UI screenshots (host/tools/ui_shot) with the mockups in docs/ux/.

    python3 tools/ux_mock/ui_compare.py [shots_dir] [diff_dir]

For each <name>.png in shots_dir with a docs/ux/<name>.png, prints the share of
differing pixels (status bar y 0..10 separately, since meters show live data),
and writes diff_dir/<name>.png: mockup | screenshot | difference (red), at 3x.
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.path.join(HERE, "..", "..", "docs", "ux")


def load_mock(path):
    im = np.asarray(Image.open(path).convert("RGB"))
    return im[::4, ::4]          # mockups are 4x nearest-neighbour


def main(argv):
    shots = argv[0] if argv else "build/ui/shots"
    out = argv[1] if len(argv) > 1 else "build/ui/diff"
    os.makedirs(out, exist_ok=True)
    total = 0
    for name in sorted(os.listdir(shots)):
        if not name.endswith(".png") or name.endswith("_4x.png"):
            continue
        mp = os.path.join(MOCK, name)
        if not os.path.exists(mp):
            continue
        a = load_mock(mp)
        b = np.asarray(Image.open(os.path.join(shots, name)).convert("RGB"))
        d = np.any(a != b, axis=2)
        st, body = d[:11].mean() * 100, d[11:].mean() * 100
        ys, xs = np.nonzero(d[11:])
        box = "" if len(ys) == 0 else " body diff box x%d..%d y%d..%d" % (xs.min(), xs.max(), ys.min() + 11, ys.max() + 11)
        print("%-24s status %5.2f%%  body %5.2f%%%s" % (name, st, body, box))
        total += 1
        vis = np.concatenate([a, b, np.where(d[..., None], np.array([255, 0, 0], np.uint8), (b // 3))], axis=1)
        Image.fromarray(vis.astype(np.uint8)).resize((vis.shape[1] * 3, vis.shape[0] * 3), Image.NEAREST).save(
            os.path.join(out, name))
    print("%d screens compared" % total)


if __name__ == "__main__":
    main(sys.argv[1:])
