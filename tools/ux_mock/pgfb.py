"""Pocket Groove UX mockups: framebuffer, palette and drawing primitives.

Draws every screen described in docs/UX.md into a 240x135 8-bit indexed
framebuffer (same palette indices, same bitmap font, same pixel coordinates as
the spec) and writes 4x nearest-neighbour PNGs to docs/ux/.  Also draws one
keyboard-map diagram per mode (docs/ux/keys-*.png).

Entry point is render.py.
"""
import math
import os
import sys

from PIL import Image


import pgfont

W, H = 240, 135
SCALE = 4
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "ux")

# --------------------------------------------------------------------------
# Palette (index -> RGB888).  UX.md "Palette" is generated from this table.
# --------------------------------------------------------------------------
BG, PANEL, RAISED, LINE, DIM, TEXT, WHITE, AMBER, RED, GREEN, CYAN = range(11)
AMBER_D, RED_D, GREEN_D, CYAN_D, BLACK = 11, 12, 13, 14, 15
TRACK0 = 16  # 16 + 4*track + {0 bright, 1 mid, 2 dim, 3 tint}

BASE = {
    BG: (0x08, 0x09, 0x0C), PANEL: (0x15, 0x18, 0x1F), RAISED: (0x23, 0x27, 0x31),
    LINE: (0x3A, 0x41, 0x50), DIM: (0x7A, 0x83, 0x96), TEXT: (0xCF, 0xD6, 0xE2),
    WHITE: (0xFF, 0xFF, 0xFF), AMBER: (0xFF, 0xB4, 0x00), RED: (0xFF, 0x38, 0x48),
    GREEN: (0x40, 0xE0, 0x70), CYAN: (0x30, 0xD0, 0xFF), AMBER_D: (0x5C, 0x40, 0x00),
    RED_D: (0x60, 0x14, 0x1C), GREEN_D: (0x14, 0x50, 0x28), CYAN_D: (0x10, 0x4C, 0x60),
    BLACK: (0, 0, 0),
}
TRACK_RGB = [
    (0xFF, 0x50, 0x3C),  # 0 KICK  red
    (0xFF, 0x8C, 0x20),  # 1 SNAR  orange
    (0xFF, 0xD4, 0x38),  # 2 CLAP  yellow
    (0x98, 0xE4, 0x30),  # 3 CHH   lime
    (0x30, 0xE4, 0xA8),  # 4 OHH   mint
    (0x38, 0xB4, 0xFF),  # 5 PERC  sky
    (0x94, 0x80, 0xFF),  # 6 BASS  violet
    (0xFF, 0x60, 0xC8),  # 7 LEAD  pink
]
TRACK_NAMES = ["KICK", "SNAR", "CLAP", "CHH", "OHH", "PERC", "BASS", "LEAD"]
PAD_KEYS = "ASDFGHJK"
NOTE_KEYS = "ZXCVBNM,"
STEP_KEYS = "12345678QWERTYUI"


def _mix(c, bg, k):
    return tuple(int(round(bg[i] + (c[i] - bg[i]) * k)) for i in range(3))


def build_palette():
    pal = dict(BASE)
    for t, rgb in enumerate(TRACK_RGB):
        pal[TRACK0 + 4 * t + 0] = rgb
        pal[TRACK0 + 4 * t + 1] = _mix(rgb, BASE[BG], 0.62)
        pal[TRACK0 + 4 * t + 2] = _mix(rgb, BASE[BG], 0.36)
        pal[TRACK0 + 4 * t + 3] = _mix(rgb, BASE[BG], 0.17)
    return pal


PAL = build_palette()


def rgb565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def tc(track, shade=0):
    """Palette index of a track colour: shade 0 bright, 1 mid, 2 dim, 3 tint."""
    return TRACK0 + 4 * track + shade


# --------------------------------------------------------------------------
# Framebuffer + primitives
# --------------------------------------------------------------------------
class FB:
    def __init__(self, w=W, h=H, bg=BG):
        self.w, self.h = w, h
        self.px = bytearray([bg]) * (w * h)

    def pixel(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = c

    def rect(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.h, y + h)):
            for xx in range(max(0, x), min(self.w, x + w)):
                self.px[yy * self.w + xx] = c

    def frame(self, x, y, w, h, c):
        self.rect(x, y, w, 1, c)
        self.rect(x, y + h - 1, w, 1, c)
        self.rect(x, y, 1, h, c)
        self.rect(x + w - 1, y, 1, h, c)

    def rrect(self, x, y, w, h, c):
        """Filled rect with the four corner pixels knocked out."""
        self.rect(x + 1, y, w - 2, h, c)
        self.rect(x, y + 1, w, h - 2, c)

    def rframe(self, x, y, w, h, c):
        self.rect(x + 1, y, w - 2, 1, c)
        self.rect(x + 1, y + h - 1, w - 2, 1, c)
        self.rect(x, y + 1, 1, h - 2, c)
        self.rect(x + w - 1, y + 1, 1, h - 2, c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.pixel(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def text(self, x, y, s, c, scale=1):
        """Draw s with FONT_S (scale 1), FONT_L (2) or FONT_XL (3). Returns end x."""
        for ch in s:
            cols = pgfont.glyph_columns(ch)
            for cx in range(5):
                b = cols[cx]
                for cy in range(8):
                    if b & (1 << cy):
                        self.rect(x + cx * scale, y + cy * scale, scale, scale, c)
            x += 6 * scale
        return x

    def text_c(self, cx, y, s, c, scale=1):
        """Centre s on column cx."""
        w = len(s) * 6 * scale - scale
        self.text(cx - w // 2, y, s, c, scale)

    def text_r(self, xr, y, s, c, scale=1):
        """Right-align s so its last pixel column is xr."""
        w = len(s) * 6 * scale - scale
        self.text(xr - w + 1, y, s, c, scale)

    def blit(self, other, x, y):
        for yy in range(other.h):
            for xx in range(other.w):
                self.pixel(x + xx, y + yy, other.px[yy * other.w + xx])

    def save(self, name, scale=SCALE):
        img = Image.new("P", (self.w, self.h))
        flat = []
        for i in range(256):
            flat.extend(from565(rgb565(PAL.get(i, (255, 0, 255)))))
        img.putpalette(flat)
        img.putdata(bytes(self.px))
        img = img.resize((self.w * scale, self.h * scale), Image.NEAREST)
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, name)
        img.convert("RGB").save(path)
        return path


def I(name):
    return pgfont.icon_code(name)


def tw(s, scale=1):
    return len(s) * 6 * scale - scale
