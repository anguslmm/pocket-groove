#!/usr/bin/env python3
"""Pocket Groove UX mockups (entry point).

Draws every screen described in docs/UX.md into a 240x135 8-bit indexed
framebuffer (same palette indices, same bitmap font, same pixel coordinates as
the spec) and writes 4x nearest-neighbour PNGs to docs/ux/, plus one
keyboard-map diagram per mode (docs/ux/keys-*.png).

    pip install pillow
    python3 tools/ux_mock/render.py            # writes docs/ux/*.png
    python3 tools/ux_mock/render.py --palette  # prints the palette table (markdown)
    python3 tools/ux_mock/render.py --font     # writes docs/ux/font.png only

Files: pgfont.py (glyph table), pgfb.py (framebuffer, palette, primitives),
screens.py (the 240x135 screens), keymaps.py (keyboard diagrams).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pgfb      # noqa: E402
import screens   # noqa: E402
import keymaps   # noqa: E402


def print_palette():
    names = {0: "BG", 1: "PANEL", 2: "RAISED", 3: "LINE", 4: "DIM", 5: "TEXT", 6: "WHITE",
             7: "AMBER", 8: "RED", 9: "GREEN", 10: "CYAN", 11: "AMBER_D", 12: "RED_D",
             13: "GREEN_D", 14: "CYAN_D", 15: "BLACK"}
    shades = ["bright", "mid", "dim", "tint"]
    print("| idx | name | RGB888 | RGB565 |")
    print("|----:|------|--------|--------|")
    for i in sorted(pgfb.PAL):
        if i < 16:
            n = names[i]
        else:
            t, s = divmod(i - 16, 4)
            n = "T%d_%s (%s %s)" % (t, shades[s].upper(), pgfb.TRACK_NAMES[t], shades[s])
        r, g, b = pgfb.PAL[i]
        print("| %d | %s | #%02X%02X%02X | 0x%04X |" % (i, n, r, g, b, pgfb.rgb565((r, g, b))))


SHEET = [
    ("boot.png", "POWER ON: DEMO LOADED, SPACE PLAYS"),
    ("groove-drum.png", "GROOVE: KICK SELECTED"),
    ("groove-bass.png", "GROOVE: BASS SELECTED"),
    ("groove-lead.png", "GROOVE: LEAD CHORD STABS"),
    ("step-edit.png", "HOLD A STEP: STEP EDIT (BASS)"),
    ("step-edit-drum.png", "STEP EDIT: HAT, 2X RATCHET"),
    ("step-edit-lead.png", "STEP EDIT: LEAD CHORD"),
    ("groove-stepwrite.png", "REC WHILE STOPPED: STEP-WRITE"),
    ("groove-live.png", "LIVE REC, 2 MUTED, P05 QUEUED"),
    ("groove-page2.png", "PAGE 2 OF A 32-STEP PATTERN"),
    ("toast-value.png", "VALUE POP-UP: DJ FILTER"),
    ("toast-undo.png", "DESTRUCTIVE ACTION + UNDO HINT"),
    ("sound.png", "SOUND: BASS PAGE 1"),
    ("sound-lock.png", "SOUND + HELD STEP = P-LOCK"),
    ("sound-kick.png", "SOUND: KICK"),
    ("mix.png", "MIX: LEVELS"),
    ("mix-duck.png", "MIX: DUCK AMOUNT PER TRACK"),
    ("mix-pan.png", "MIX: PAN (MONO OUT)"),
    ("fx-duck.png", "FX DUCK: SIDECHAIN PUMP"),
    ("fx-duck-track.png", "FX DUCK: PAD = TRACK AMOUNT"),
    ("fx-space.png", "FX SPACE: DELAY + REVERB"),
    ("fx-master.png", "FX MASTER: DJ FILTER, SCALE"),
    ("pattern-overlay.png", "OPT HELD: QUEUE PATTERNS"),
    ("mute-overlay.png", "FN HELD: MUTE / SOLO"),
    ("roll-overlay.png", "ALT HELD: ROLL"),
    ("tempo-overlay.png", "TAB HELD: TEMPO + SWING"),
    ("song.png", "SONG: PATTERN CHAIN"),
    ("project-load.png", "PROJECT: LOAD"),
    ("project-save.png", "PROJECT: SAVE AS"),
    ("confirm.png", "CONFIRM DIALOG"),
    ("help.png", "HOLD /: KEY MAP OF THIS VIEW"),
    ("help-explain.png", "TAP /: KEYS EXPLAIN THEMSELVES"),
]


def contact_sheet():
    """docs/ux/overview.png: every screen at 2x with a caption, 4 per row."""
    from PIL import Image
    cols, tw_, th_, cap, gap = 4, 480, 270, 26, 16
    rows = (len(SHEET) + cols - 1) // cols
    head = 60
    sheet_w = cols * tw_ + (cols + 1) * gap
    sheet_h = head + rows * (th_ + cap + gap) + gap
    fbh = pgfb.FB(sheet_w // 2, head // 2)
    fbh.text(gap // 2, 6, "POCKET GROOVE", pgfb.WHITE, 2)
    fbh.text(gap // 2 + 170, 13, "UX MOCKUPS  240x135 SHOWN AT 2X", pgfb.DIM)
    out = Image.new("RGB", (sheet_w, sheet_h), pgfb.PAL[pgfb.BG])
    hdr = os.path.join(pgfb.OUT, "_hdr.png")
    fbh.save("_hdr.png", scale=2)
    out.paste(Image.open(hdr), (0, 0))
    os.remove(hdr)
    for i, (name, caption) in enumerate(SHEET):
        r, c = divmod(i, cols)
        x = gap + c * (tw_ + gap)
        y = head + r * (th_ + cap + gap)
        img = Image.open(os.path.join(pgfb.OUT, name)).resize((tw_, th_), Image.NEAREST)
        out.paste(Image.new("RGB", (tw_ + 4, th_ + 4), pgfb.PAL[pgfb.LINE]), (x - 2, y - 2))
        out.paste(img, (x, y))
        fbc = pgfb.FB(tw_ // 2, cap // 2, bg=pgfb.BG)
        fbc.text(0, 3, "%02d %s" % (i + 1, caption), pgfb.TEXT)
        tmp = "_cap.png"
        fbc.save(tmp, scale=2)
        out.paste(Image.open(os.path.join(pgfb.OUT, tmp)), (x, y + th_ + 4))
        os.remove(os.path.join(pgfb.OUT, tmp))
    path = os.path.join(pgfb.OUT, "overview.png")
    out.save(path)
    return path


def main(argv):
    if "--palette" in argv:
        print_palette()
        return
    if "--font" in argv:
        print(screens.font_specimen())
        return
    for p in screens.render_all() + keymaps.render_all():
        print(os.path.relpath(p))
    print(os.path.relpath(contact_sheet()))


if __name__ == "__main__":
    main(sys.argv[1:])
