"""Pocket Groove UX mockups: the 240x135 screens.  Coordinates here are the
coordinates quoted in docs/UX.md; if you change one, change the other."""
import math

import pgparams as PP
from pgfb import (FB, I, tw, tc, W, H, BG, PANEL, RAISED, LINE, DIM, TEXT, WHITE, AMBER, RED,
                  GREEN, CYAN, AMBER_D, RED_D, GREEN_D, CYAN_D, BLACK, TRACK_NAMES, PAD_KEYS,
                  NOTE_KEYS, STEP_KEYS)

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
CHORDS = ["-", "OCT", "5TH", "MAJ", "MIN", "MAJ7", "MIN7", "DOM7", "SUS4", "MIN9"]
CHORD_SHORT = ["", "8", "5", "M", "m", "M7", "m7", "7", "s4", "m9"]


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


# --------------------------------------------------------------------------
# Demo data (the factory demo pattern, see UX.md "Power-on and the demo")
# --------------------------------------------------------------------------
def S(on=1, note=60, acc=0, slide=0, vel=100, gate=2, prob=100, ratch=1, chord=0, lock=None):
    return dict(on=on, note=note, acc=acc, slide=slide, vel=vel, gate=gate, prob=prob,
                ratch=ratch, chord=chord, lock=lock)


def demo_pattern():
    off = lambda: S(on=0)
    t = [[off() for _ in range(16)] for _ in range(8)]
    for i in (0, 4, 8, 12):
        t[0][i] = S(acc=1 if i == 0 else 0, vel=110)
    t[1][12] = S(vel=96)
    t[1][15] = S(vel=60, prob=50)
    t[1][7] = S(vel=50, prob=40)
    for i in (4, 12):
        t[2][i] = S(vel=105)
    for i in range(16):
        t[3][i] = S(vel=(100, 55, 80, 55)[i % 4])
    t[3][15] = S(vel=80, ratch=2)
    for i in (2, 6, 10, 14):
        t[4][i] = S(vel=95)
    t[5][3] = S(vel=80)
    t[5][9] = S(vel=70, prob=60)
    t[5][14] = S(vel=85, note=63)
    A1 = 33
    bass = {0: S(note=A1, acc=1), 2: S(note=A1), 3: S(note=A1 + 12, slide=1), 5: S(note=A1 + 3),
            6: S(note=A1), 8: S(note=A1, acc=1), 10: S(note=A1 - 2), 11: S(note=A1, slide=1),
            13: S(note=A1 + 7, acc=1, lock=("CUT", 110)), 14: S(note=A1 + 5)}
    for i, s in bass.items():
        t[6][i] = s
    t[7][2] = S(note=57, chord=6, gate=3)
    t[7][7] = S(note=57, chord=6, gate=2)
    t[7][10] = S(note=53, chord=5, gate=6)
    return t


# --------------------------------------------------------------------------
# Shared chrome
# --------------------------------------------------------------------------
def status_bar(fb, playing=True, rec=False, pat=1, queued=None, song=None, tempo=124, beat=True,
               focus=(6, "CUT", "58"), peak=(0.66, 0.58), clip=False, duck=0.45, batt=0.8):
    """y 0..10.  focus = (track index | 'FX', param short name, value text)."""
    fb.rect(0, 0, W, 11, PANEL)
    fb.text(3, 2, I("play") if playing else I("stop"), GREEN if playing else DIM)
    fb.text(11, 2, I("rec"), RED if rec else RAISED)
    x = fb.text(20, 2, "P%02d" % pat, WHITE)
    if queued is not None:
        fb.text(x, 2, ">%02d" % queued, AMBER)
    elif song is not None:
        fb.text(x + 1, 2, I("chain"), GREEN)
        fb.text(x + 7, 2, "%02d" % song, GREEN)
    fb.text(64, 2, "%3d" % tempo, TEXT)
    fb.rect(84, 4, 3, 3, WHITE if beat else LINE)
    # focus chip x 92..165
    if focus:
        trk, name, val = focus
        if trk == "FX":
            tag, col = "FX", CYAN
        else:
            tag, col = TRACK_NAMES[trk], tc(trk)
        w = len(tag) * 6 + 3
        fb.rrect(92, 1, w, 9, col)
        fb.text(94, 2, tag, BG)
        fb.text(92 + w + 3, 2, name, TEXT)
        fb.text_r(165, 2, val, WHITE)
    # peak meter (mono output: one bar, max of L/R) x 171..204, y 3..7
    p = max(peak)
    fb.rect(171, 3, 34, 5, RAISED)
    n = int(round(min(1.0, p) * 34))
    fb.rect(171, 3, min(n, 26), 5, GREEN)
    if n > 26:
        fb.rect(197, 3, n - 26, 5, AMBER)
    if clip:
        fb.rect(203, 2, 2, 7, RED)
    # ducker gain-reduction bar x 208..211, y 1..9 (fills downward as gain falls)
    fb.rect(208, 1, 4, 9, RAISED)
    fb.rect(208, 1, 4, int(round((1.0 - duck) * 9)), CYAN)
    # battery x 217..234
    bc = GREEN if batt > 0.3 else (AMBER if batt > 0.1 else RED)
    fb.frame(217, 2, 16, 7, DIM)
    fb.rect(233, 4, 2, 3, DIM)
    fb.rect(219, 4, int(round(12 * batt)), 3, bc)


def footer(fb, parts, right=None):
    """y 127..133.  parts = [(text, colour)], drawn left to right from x=4."""
    x = 4
    for s, c in parts:
        x = fb.text(x, 127, s, c)
    if right:
        s, c = right
        fb.text_r(235, 127, s, c)


def page_indicator(fb, x, y, pages, shown, playing_page=None):
    """4 segments of 7x5 px with 2 px gaps: pages in use, shown page, play page."""
    for p in range(4):
        px = x + p * 9
        if p >= pages:
            fb.frame(px, y, 7, 5, RAISED)
        elif p == shown:
            fb.rect(px, y, 7, 5, WHITE)
        else:
            fb.rect(px, y, 7, 5, LINE)
        if playing_page == p and p < pages:
            fb.rect(px + 2, y + 1, 3, 3, GREEN)
    return x + 36


# --------------------------------------------------------------------------
# GROOVE view
# --------------------------------------------------------------------------
OV_Y0, OV_PITCH = 13, 9          # overview rows: y = 13 + 9*i, content rows y+1..y+7
OV_X0 = 42                       # first mini step


def ov_x(i):
    return OV_X0 + i * 11 + (i // 4) * 2


def overview(fb, pat, sel, play, mutes=(), flash=(), lengths=None):
    for t in range(8):
        y = OV_Y0 + t * OV_PITCH
        muted = t in mutes
        col = tc(t)
        if t == sel:
            fb.rect(0, y, W, 9, tc(t, 3))
        # pad-key tag x 1..7
        if muted:
            fb.frame(1, y + 1, 7, 7, DIM)
            fb.rect(3, y + 3, 3, 3, DIM)
        else:
            fb.rect(1, y, 7, 9, col)
            fb.text(2, y + 1, PAD_KEYS[t], BG)
        fb.text(11, y + 1, TRACK_NAMES[t], DIM if muted else (WHITE if t == sel else tc(t, 1)))
        n = lengths[t] if lengths else 16
        lo = min(s["note"] for s in pat[t] if s["on"]) if any(s["on"] for s in pat[t]) else 60
        hi = max(s["note"] for s in pat[t] if s["on"]) if any(s["on"] for s in pat[t]) else 60
        for i in range(16):
            x = ov_x(i)
            if i >= n:
                continue
            s = pat[t][i]
            here = (play is not None and play % n == i)
            if not s["on"]:
                fb.rect(x, y + 1, 10, 7, LINE if here else (RAISED if i % 4 == 0 else PANEL))
                continue
            if muted:
                c = LINE
            elif here:
                c = WHITE
            elif s["prob"] < 100:
                c = tc(t, 1)
            else:
                c = col
            if t >= 6:   # synth tracks: 3 px bar, height by pitch (mini piano roll)
                fb.rect(x, y + 1, 10, 7, tc(t, 3) if not muted else PANEL)
                lvl = 0 if hi == lo else int(round((s["note"] - lo) * 4.0 / (hi - lo)))
                w = 13 if s["slide"] else 10
                fb.rect(x, y + 5 - lvl, w, 3, c)
            else:
                fb.rect(x, y + 1, 10, 7, c)
                if s["acc"] and not here and not muted:
                    fb.rect(x, y + 1, 10, 1, WHITE)
        # activity lamp x 228..235
        lamp = WHITE if t in flash else (tc(t, 2) if not muted else PANEL)
        fb.rect(228, y + 2, 8, 5, lamp)
        if muted:
            fb.text(229, y + 1, "M", DIM)


PAD_W, PAD_H = 27, 18
PAD_Y = (88, 108)


def pad_x(i):
    return 4 + (i % 8) * 29 + ((i % 8) // 4) * 2


def pad_xy(i):
    return pad_x(i), PAD_Y[i // 8]


def step_pad(fb, i, s, track, play=False, held=False, cursor=False, beyond=False, base=0):
    x, y = pad_xy(i)
    key = STEP_KEYS[i]
    synth = track >= 6
    if beyond:
        fb.rframe(x, y, PAD_W, PAD_H, RAISED)
        return
    if not s["on"]:
        fill = RAISED if i % 4 == 0 else PANEL
        if play:
            fill = LINE
        fb.rrect(x, y, PAD_W, PAD_H, fill)
        fb.text(x + 11, y + 6, key, DIM if play else LINE)
    else:
        fill = WHITE if play else (tc(track, 1) if s["prob"] < 100 else tc(track))
        fb.rrect(x, y, PAD_W, PAD_H, fill)
        ink = BG
        fb.text(x + 3, y + 2, key, tc(track, 2) if not play else DIM)
        if synth:
            fb.text_r(x + 23, y + 9, note_name(s["note"]), ink)
        else:
            # velocity bar, 3 px tall, up to 21 px wide
            fb.rect(x + 3, y + 12, max(2, int(round(s["vel"] * 21 / 127.0))), 3, ink)
        # markers on the top row, right to left: chord, lock, ratchet, probability.
        # A marker is skipped when it would reach left of x+9 (the key legend).
        mx = x + 23
        if s["chord"]:
            lab = CHORD_SHORT[s["chord"]]
            fb.text_r(mx, y + 1, lab, tc(track, 2) if not play else DIM)
            mx -= len(lab) * 6 + 1
        if s["lock"] and mx - 4 >= x + 9:
            fb.text(mx - 4, y + 1, I("lock"), ink)
            mx -= 7
        if s["ratch"] > 1 and mx - s["ratch"] * 2 + 1 >= x + 9:
            for k in range(s["ratch"]):
                fb.rect(mx - k * 2, y + 3, 1, 5, ink)
            mx -= s["ratch"] * 2 + 2
        if s["prob"] < 100 and mx - 4 >= x + 9:
            fb.text(mx - 4, y + 2, "%", ink)
        if s["acc"]:
            fb.rect(x + 1, y, PAD_W - 2, 2, WHITE if not play else AMBER)
        if s["slide"] and synth:
            # slide: bar bridging the gap into the next pad
            gx = x + PAD_W
            gw = pad_x(i + 1) - gx if (i % 8) < 7 else 2
            fb.rect(gx - 2, y + 12, gw + 4, 3, WHITE)
    if held:
        fb.rframe(x - 1, y - 1, PAD_W + 2, PAD_H + 2, WHITE)
        fb.rframe(x, y, PAD_W, PAD_H, WHITE)
    if cursor:
        fb.rect(x + 2, y + PAD_H, PAD_W - 4, 1, AMBER)
        fb.rframe(x - 1, y - 1, PAD_W + 2, PAD_H + 2, AMBER)


def pad_grid(fb, pat, track, play=None, held=None, cursor=None, length=16):
    for i in range(16):
        step_pad(fb, i, pat[track][i], track, play=(play == i), held=(held == i),
                 cursor=(cursor == i), beyond=(i >= length))


def groove(fb, pat, sel, play=6, rec=False, playing=True, cursor=None, focus=None, mutes=(),
           flash=(), queued=None, footer_extra=None, length=16, pages=1, shown=0, duck=0.45,
           total=16, play_page=0, held=None):
    if focus is None:
        focus = (sel, "TUNE", "60") if sel < 6 else (sel, "CUT", "58" if sel == 6 else "70")
    status_bar(fb, playing=playing, rec=rec, focus=focus, queued=queued, duck=duck,
               peak=(0.66, 0.58) if playing else (0.0, 0.0))
    overview(fb, pat, sel, play if playing else None, mutes=mutes, flash=flash)
    pad_grid(fb, pat, sel, play=play if playing else None, cursor=cursor, length=length,
             held=held)
    x = page_indicator(fb, 4, 128, pages, shown, play_page if playing else None)
    xx = fb.text(x + 6, 127, "%d-%d" % (shown * 16 + 1, min(total, shown * 16 + 16)), TEXT)
    xx = fb.text(xx, 127, "/%d" % total, DIM)
    if footer_extra is None:      # default context: what the note row does
        if sel < 6:
            footer_extra = [("NOTES=VEL", tc(sel, 1))]
        elif sel == 6:
            footer_extra = [("A MIN", tc(sel)), (" OCT1", TEXT)]
        else:
            footer_extra = [("A MIN", tc(sel)), (" OCT3 ", TEXT), ("AUTO", tc(sel))]
    xx = 94
    for s, c in footer_extra:
        xx = fb.text(xx, 127, s, c)
    assert xx <= 200, "footer too long"
    fb.text_r(235, 127, "/HELP", DIM)




def chip(fb, x, y, s, col, ink=BG):
    """Rounded label, 9 px tall, text inset 2 px.  Returns x after the chip."""
    w = len(s) * 6 + 3
    fb.rrect(x, y, w, 9, col)
    fb.text(x + 2, y + 1, s, ink)
    return x + w


def toast_value(fb, title, value, frac, col, bipolar=False, sub=None):
    """Value pop-up: x 48..191, y 34..66."""
    x, y, w, h = 48, 34, 144, 33
    fb.rect(x - 1, y - 1, w + 2, h + 2, BG)
    fb.rrect(x, y, w, h, RAISED)
    fb.rframe(x, y, w, h, col)
    fb.text(x + 6, y + 5, title, TEXT)
    if sub:
        fb.text(x + 6, y + 15, sub, col)
    fb.text_r(x + w - 7, y + 4, value, WHITE, 2)
    bx, bw = x + 6, w - 12
    fb.rect(bx, y + 25, bw, 4, BG)
    if bipolar:
        mid = bx + bw // 2
        n = int(round((frac - 0.5) * bw))
        if n < 0:
            fb.rect(mid + n, y + 25, -n, 4, col)
        else:
            fb.rect(mid, y + 25, n, 4, col)
        fb.rect(mid, y + 24, 1, 6, WHITE)
    else:
        fb.rect(bx, y + 25, int(round(frac * bw)), 4, col)


def toast_msg(fb, s, col=TEXT, border=LINE):
    """Message pop-up, centred, y 44..60."""
    w = tw(s) + 16
    x = (W - w) // 2
    fb.rect(x - 1, 43, w + 2, 19, BG)
    fb.rrect(x, 44, w, 17, RAISED)
    fb.rframe(x, 44, w, 17, border)
    fb.text(x + 8, 49, s, col)


def dialog(fb, title, lines, col=AMBER):
    """Modal dialog, x 30..209, y 34..100."""
    x, y, w, h = 30, 34, 180, 67
    fb.rect(x - 2, y - 2, w + 4, h + 4, BG)
    fb.rrect(x, y, w, h, PANEL)
    fb.rframe(x, y, w, h, col)
    fb.rect(x + 1, y + 1, w - 2, 11, col)
    fb.text(x + 6, y + 3, title, BG)
    for i, (s, c) in enumerate(lines):
        fb.text(x + 8, y + 18 + i * 11, s, c)


# --------------------------------------------------------------------------
# STEP EDIT (GROOVE view while a step key is held)
# --------------------------------------------------------------------------
# Step fields on the pad row (a s d f g h j k) while a step is held.  None = no field
# for this track type (drawn as an empty dark slot; the key does nothing).
FIELDS_DRUM = ["VEL", "PTCH", "PROB", "RTCH", "ACC", None, None, "LOCK"]
FIELDS_BASS = ["VEL", "GATE", "PROB", "RTCH", "ACC", "SLD", None, "LOCK"]
FIELDS_LEAD = ["VEL", "GATE", "PROB", "RTCH", "ACC", "SLD", "CHRD", "LOCK"]
VEL_ROW = [16, 32, 48, 64, 80, 96, 112, 127]          # drum note row = velocity levels


def fields_for(track):
    return FIELDS_DRUM if track < 6 else (FIELDS_BASS if track == 6 else FIELDS_LEAD)


def gate_text(q):
    return ("%g" % (q / 4.0))


def step_field_value(track, f, s):
    """(text, bar fraction or None, special) for one field of step s."""
    if f == "VEL":
        return "%d" % s["vel"], s["vel"] / 127.0
    if f == "PTCH":
        d = s["note"] - 60
        return ("0" if d == 0 else "%+d" % d), (d + 24) / 48.0
    if f == "GATE":
        return gate_text(s["gate"]), min(1.0, s["gate"] / 16.0)
    if f == "PROB":
        return "%d%%" % s["prob"], s["prob"] / 100.0
    if f == "RTCH":
        return "x%d" % s["ratch"], None
    if f == "ACC":
        return ("ON" if s["acc"] else "OFF"), None
    if f == "SLD":
        return ("ON" if s["slide"] else "OFF"), None
    if f == "CHRD":
        return (CHORDS[s["chord"]] if s["chord"] else "OFF"), None
    if f == "LOCK":
        return (s["lock"][0] if s["lock"] else "OFF"), None
    return "", None


def step_edit_panel(fb, track, idx, s, target, scale_notes):
    """Drawn over the overview (y 12..85) while step `idx` is held in GROOVE."""
    col = tc(track)
    drum = track < 6
    fb.rect(0, 12, W, 74, PANEL)
    fb.rect(0, 12, W, 1, col)
    x = fb.text(4, 16, "STEP %d" % (idx + 1), WHITE)
    chip(fb, x + 4, 15, TRACK_NAMES[track], col)
    if drum:
        fb.text_r(235, 16, "Z-, VELOCITY", DIM)
        fb.text(4, 28, "%d" % s["vel"], WHITE, 2)
        cells = VEL_ROW
        cur_i = VEL_ROW.index(s["vel"]) if s["vel"] in VEL_ROW else -1
        lab = lambda v: "%d" % v
    else:
        fb.text_r(235, 16, I("up") + I("down") + " SEMI  SHIFT OCT", DIM)
        fb.text(4, 28, note_name(s["note"]), WHITE, 2)
        cells = scale_notes
        cur_i = scale_notes.index(s["note"]) if s["note"] in scale_notes else -1
        lab = note_name
    for k in range(8):
        x = 50 + k * 23
        cur = (k == cur_i)
        fb.rrect(x, 27, 22, 17, col if cur else RAISED)
        fb.text(x + 2, 28, NOTE_KEYS[k], BG if cur else DIM)
        fb.text_r(x + 19, 36, lab(cells[k]), BG if cur else TEXT)
    names = fields_for(track)
    for k in range(8):
        x = pad_x(k)
        f = names[k]
        if f is None:
            fb.rframe(x, 48, 27, 37, RAISED)
            fb.text(x + 3, 50, PAD_KEYS[k], LINE)
            continue
        sel = (k == target)
        fb.rrect(x, 48, 27, 37, RAISED if sel else BG)
        if sel:
            fb.rframe(x, 48, 27, 37, WHITE)
        fb.text(x + 3, 50, PAD_KEYS[k], WHITE if sel else DIM)
        fb.text_c(x + 14, 59, f, col if sel else DIM)
        txt, fr = step_field_value(track, f, s)
        on = txt != "OFF"
        if f == "LOCK" and s["lock"]:
            fb.text_c(x + 14, 68, txt, AMBER)
            fb.text_c(x + 14, 77, "%d" % s["lock"][1], AMBER)
            continue
        fb.text_c(x + 14, 68, txt, WHITE if on else LINE)
        if f == "PTCH":
            fb.rect(x + 3, 79, 21, 3, LINE)
            fb.rect(x + 13, 78, 1, 5, WHITE)
        elif fr is not None:
            fb.rect(x + 3, 79, 21, 3, LINE)
            fb.rect(x + 3, 79, int(round(fr * 21)), 3, col)
        elif f in ("ACC", "SLD") and on:
            fb.rect(x + 3, 79, 21, 3, WHITE)
        elif f == "RTCH":
            for r in range(4):
                fb.rect(x + 3 + r * 6, 79, 4, 3, col if r < s["ratch"] else LINE)


def step_held_footer(fb):
    fb.rect(0, 125, W, 10, BG)
    footer(fb, [("TAP STEP", WHITE), (" COPY HERE  ", DIM), ("DEL", WHITE), (" NO LOCK", DIM)],
           ("/ HELP", DIM))


# --------------------------------------------------------------------------
# Parameter columns (SOUND, FX SPACE, FX MASTER) and the mini step strip
# --------------------------------------------------------------------------
def P(name, val, text=None, kind="uni", n=0, long=None, lock=None):
    return dict(name=name, val=val, text=text if text is not None else "%d" % val, kind=kind,
                n=n, long=long or name, lock=lock)


def PI(info, v=None, lock=None):
    """P() column built from a real ParamInfo dict (pgparams) and a raw value."""
    v = info["d"] if v is None else v
    k = PP.kind(info)
    n = info["max"] - info["min"] + 1
    if k == "enum":
        val = v - info["min"]
    else:
        val = int(round(PP.frac(info, v) * 127))
    lk = None if lock is None else int(round(PP.frac(info, lock) * 127))
    return dict(name=info["name"], val=val, text=PP.fmt(info, v), kind=k, n=n,
                long=info["long"].upper(), lock=lk, locktext=None if lock is None else PP.fmt(info, lock))


def param_cols(fb, params, focus, bright, mid, y0=32, bar_h=52, pitch=30, keys=NOTE_KEYS):
    """Column c occupies x c*pitch .. c*pitch+pitch-1.  y0 = name row."""
    by = y0 + 9
    for c, p in enumerate(params):
        cx = c * pitch + pitch // 2
        f = (c == focus)
        if p is None:
            fb.text_c(cx, y0, "-", LINE)
            continue
        if f:
            w = max(len(p["name"]), 3) * 6 + 3
            fb.rrect(cx - w // 2, y0 - 1, w, 9, bright)
            fb.text_c(cx, y0, p["name"], BG)
        else:
            fb.text_c(cx, y0, p["name"], TEXT)
        bx, bw = cx - 7, 14
        fb.rect(bx, by, bw, bar_h, PANEL if not f else RAISED)
        fill = bright if f else mid
        if p["lock"] is not None:
            fill = AMBER
        v = p["lock"] if p["lock"] is not None else p["val"]
        if p["kind"] == "enum" and p["n"] > 8:
            hpx = int(round((v + 1) / float(p["n"]) * bar_h))
            fb.rect(bx, by + bar_h - hpx, bw, hpx, fill)
            for k in range(1, p["n"]):
                fb.rect(bx, by + bar_h - int(round(k / float(p["n"]) * bar_h)), bw, 1, PANEL if not f else RAISED)
        elif p["kind"] == "enum":
            n = p["n"]
            sh = bar_h // n
            for k in range(n):
                yy = by + bar_h - (k + 1) * sh
                fb.rect(bx + 2, yy + 1, bw - 4, sh - 2, fill if k == v else LINE)
        elif p["kind"] == "bi":
            midy = by + bar_h // 2
            d = int(round((v - 64) / 64.0 * (bar_h // 2)))
            if d >= 0:
                fb.rect(bx, midy - d, bw, d + 1, fill)
            else:
                fb.rect(bx, midy, bw, -d, fill)
            fb.rect(bx - 2, midy, bw + 4, 1, DIM)
        else:
            hpx = int(round(v / 127.0 * bar_h))
            fb.rect(bx, by + bar_h - hpx, bw, hpx, fill)
            if p["lock"] is not None:   # base value tick while showing a lock
                ty = by + bar_h - int(round(p["val"] / 127.0 * bar_h))
                fb.rect(bx - 2, ty, bw + 4, 1, WHITE)
        txt = p["text"] if p["lock"] is None else p.get("locktext") or "%d" % p["lock"]
        fb.text_c(cx, by + bar_h + 3, txt, AMBER if p["lock"] is not None else (WHITE if f else TEXT))
        fb.text_c(cx, by + bar_h + 12, keys[c], WHITE if f else DIM)


def strip_x(i):
    return 4 + i * 14 + (i // 4) * 2


def mini_strip(fb, y, steps, track, play=None, held=None, length=16):
    """16 cells of 13x8 at y; lock = amber top edge; held = white frame."""
    for i in range(16):
        x = strip_x(i)
        if i >= length:
            continue
        s = steps[i]
        if s["on"]:
            c = WHITE if play == i else tc(track)
        else:
            c = LINE if play == i else (RAISED if i % 4 == 0 else PANEL)
        fb.rect(x, y, 13, 8, c)
        if s["on"] and s["lock"]:
            fb.rect(x, y, 13, 2, AMBER)
        if held == i:
            fb.frame(x - 1, y - 1, 15, 10, WHITE)


def fx_tabs(fb, active):
    """y 14..22: the three FX pages as tabs from x=4; the active one is a CYAN chip."""
    x = 4
    for i, t in enumerate(FX_TABS):
        if i == active:
            x = chip(fb, x, 14, t, CYAN) + 4
        else:
            fb.text(x + 2, 15, t, DIM)
            x += len(t) * 6 + 3 + 4
    return x


def big_value(fb, long, val, col=WHITE):
    """Focused parameter read-out, right side of the header (y 13..28)."""
    long = long.upper()
    fb.text_r(235, 13, val, col, 2)
    fb.text_r(235 - tw(val, 2) - 6, 19, long, DIM)


# Demo sound settings (UX.md "The demo project"): raw values, keyed by param name.
DEMO_SOUND = {6: dict(CUT=58, RES=96, ENV=84, DCY=40, ACC=100, DRV=52, WAVE=0, SUB=70, FOLD=24),
              7: dict(CUT=70, RES=24, WAVE=3, DET=64, REL=60)}


def page_params(track, page, locks=None):
    """The 8 P() columns of SOUND page `page` for `track` (None for unused slots)."""
    tbl = PP.TRACKS[track]
    vals = DEMO_SOUND.get(track, {})
    out = []
    for i in range(page * 8, page * 8 + 8):
        if i >= len(tbl):
            out.append(None)
            continue
        info = tbl[i]
        lk = (locks or {}).get(info["name"])
        out.append(PI(info, vals.get(info["name"]), lock=lk))
    return out


def n_pages(track):
    return (len(PP.TRACKS[track]) + 7) // 8


def sound(fb, pat, track=6, focus=0, play=6, held=None, params=None, page=0, flash=()):
    params = params or page_params(track, page)
    col = tc(track)
    p = params[focus]
    val = p["text"] if p["lock"] is None else p["locktext"]
    status_bar(fb, focus=(track, p["name"], val))
    x = chip(fb, 4, 14, TRACK_NAMES[track], col)
    if held is None:
        x = fb.text(x + 5, 15, "SOUND", TEXT)
        for i in range(n_pages(track)):
            if i == page:
                x = chip(fb, x + 5, 14, "%d" % (i + 1), WHITE)
            else:
                fb.text(x + 7, 15, "%d" % (i + 1), DIM)
                x += 14
        big_value(fb, p["long"], val)
    else:
        x = fb.text(x + 5, 15, "STEP %d" % (held + 1), WHITE)
        chip(fb, x + 3, 14, I("lock") + "LOCK", AMBER)
        big_value(fb, p["long"], val, AMBER)
    param_cols(fb, params, focus, col, tc(track, 1), y0=32, bar_h=52)
    mini_strip(fb, 114, pat[track], track, play=play, held=held)
    if held is None:
        footer(fb, [("HOLD STEP + -/= ", DIM), (I("lock") + "LOCK", AMBER)], ("/ HELP", DIM))
    else:
        footer(fb, [("-/= LOCK VALUE   DEL REMOVE LOCK", DIM)], ("/ HELP", DIM))


# --------------------------------------------------------------------------
# MIX view
# --------------------------------------------------------------------------
MIX_ROWS = ["LEVEL", "DUCK", "DELAY", "REVERB", "PAN"]
MIX_SHORT = ["LVL", "DUCK", "DLY", "REV", "PAN"]       # = ParamInfo names (globals.cpp)
MIX_INDEX = [24, 28, 26, 27, 25]                       # track param index of each row


def mix(fb, row=0, sel=6, values=None, mutes=(), flash=(), pat=None, play=6):
    values = values or PP.MIX_DEF[MIX_SHORT[row]]
    txt = PP.fmt(PP.MIX[[m["name"] for m in PP.MIX].index(MIX_SHORT[row])], values[sel])
    status_bar(fb, focus=(sel, MIX_SHORT[row], txt))
    x = 4
    for i, t in enumerate(MIX_ROWS):
        w = len(t) * 6 + 12
        if i == row:
            fb.rrect(x, 14, w, 9, WHITE)
            fb.text(x + 2, 15, NOTE_KEYS[i], DIM)
            fb.text(x + 10, 15, t, BG)
        else:
            fb.text(x + 2, 15, NOTE_KEYS[i], LINE)
            fb.text(x + 10, 15, t, DIM if t != "PAN" else LINE)  # PAN dimmed: mono output
        x += w + 5
    for t in range(8):
        x0 = t * 30
        cx = x0 + 15
        muted = t in mutes
        col = tc(t) if not muted else LINE
        if t == sel:
            fb.rrect(x0 + 1, 26, 28, 98, tc(t, 3))
            fb.rframe(x0 + 1, 26, 28, 98, tc(t, 1))
        # trig lamp
        fb.rect(cx - 5, 29, 10, 2, WHITE if t in flash else RAISED)
        # fader well x cx-5..cx+4, y 34..89 (56 px)
        fb.rect(cx - 5, 34, 10, 56, PANEL if t != sel else BG)
        v = values[t]
        if MIX_SHORT[row] == "PAN":
            midy = 34 + 28
            d = int(round((v - 64) / 64.0 * 28))
            if d >= 0:
                fb.rect(cx - 5, midy - d, 10, d + 1, col)
            else:
                fb.rect(cx - 5, midy, 10, -d, col)
            fb.rect(cx - 7, midy, 14, 1, DIM)
        else:
            hpx = int(round(v / 127.0 * 56))
            fb.rect(cx - 5, 90 - hpx, 10, hpx, col if t == sel else (tc(t, 1) if not muted else LINE))
            fb.rect(cx - 7, 90 - hpx, 14, 2, WHITE if t == sel else TEXT)
        s = PP.fmt(PP.MIX[[m["name"] for m in PP.MIX].index(MIX_SHORT[row])], v)
        if MIX_SHORT[row] == "PAN" and v == 64:
            s = "C"
        fb.text_c(cx, 93, s, WHITE if t == sel else TEXT)
        if muted:
            fb.rframe(x0 + 2, 103, 26, 9, DIM)
            fb.text_c(cx, 104, "MUTE", DIM)
        else:
            w = len(TRACK_NAMES[t]) * 6 + 3
            fb.rrect(cx - w // 2, 103, w, 9, col)
            fb.text_c(cx, 104, TRACK_NAMES[t], BG)
        fb.text_c(cx, 115, PAD_KEYS[t], WHITE if t == sel else DIM)
    footer(fb, [("A-K", TEXT), (" STRIP  ", DIM), ("-/=", TEXT), (" MOVE  ", DIM), ("Z-B", TEXT), (" ROW", DIM)]
           if MIX_SHORT[row] != "PAN" else [("PAN: HEADPHONE OUT IS MONO", AMBER)])


# --------------------------------------------------------------------------
# FX view
# --------------------------------------------------------------------------
FX_TABS = ["DUCK", "SPACE", "MASTER"]


def duck_curve(t, depth, hold, rel, curve):
    """Schematic gain 0..1 at time t (in beats) after the trigger."""
    if t < hold:
        return 1.0 - depth
    u = min(1.0, (t - hold) / max(1e-3, rel))
    shaped = u ** (2.0 ** ((0.5 - curve) * 3.0))
    return 1.0 - depth * (1.0 - shaped)


DEMO_GLOBALS = dict(DDEP=100, DHLD=30, DREL=70, DCRV=80, DFX=64, DFB=58, RSIZ=70, FILT=38, DRIV=20,
                    ROOT=9, SCAL=2, SWNG=8)


def gval(name):
    return DEMO_GLOBALS.get(name, PP.GLOBALS[PP.G[name]]["d"])


def gtext(name):
    return PP.fmt(PP.GLOBALS[PP.G[name]], gval(name))


def fx_duck(fb, pat, focus=1, pos=0.38, track_focus=None):
    """track_focus: a pad was pressed, so VALUE -/+ edits that track's DUCK amount."""
    depth, hold, rel, curve = gval("DDEP") / 127.0, 0.10, 0.55, 0.62
    g = duck_curve(pos, depth, hold, rel, curve)
    names = PP.FX_PAGES[0][1]
    amounts = PP.MIX_DEF["DUCK"]
    if track_focus is None:
        status_bar(fb, focus=("FX", names[focus], gtext(names[focus])), duck=g)
    else:
        status_bar(fb, focus=(track_focus, "DUCK", PP.fmt(PP.MIX[4], amounts[track_focus])), duck=g)
    fx_tabs(fb, 0)
    fb.text_r(235, 15, "GAIN %d%%" % int(round(g * 100)), CYAN)
    # graph frame x 4..151, y 27..76; plot area x 6..149 (144 px = 1 beat), y 30..74 (gain 1..0)
    gx, gy, gw, gh = 6, 30, 144, 45
    fb.rect(4, 27, 148, 50, PANEL)
    for q in range(1, 4):
        for yy in range(gy, gy + gh, 3):
            fb.pixel(gx + q * gw // 4, yy, LINE)
    for xx in range(gx, gx + gw, 3):
        fb.pixel(xx, gy, LINE)
    prev = None
    for i in range(gw):
        gg = duck_curve(i / float(gw), depth, hold, rel, curve)
        y = gy + int(round((1.0 - gg) * (gh - 1)))
        fb.rect(gx + i, y, 1, gy + gh - y, CYAN_D)
        if prev is not None and abs(y - prev) > 1:
            fb.rect(gx + i, min(y, prev), 1, abs(y - prev) + 1, CYAN)
        fb.pixel(gx + i, y, CYAN)
        fb.pixel(gx + i, y + 1, CYAN)
        prev = y
    # trigger marker and live dot
    fb.rect(gx, gy, 1, gh, tc(0))
    fb.text(gx + 3, gy + gh - 8, "KICK", tc(0))
    lx = gx + int(pos * gw)
    ly = gy + int(round((1.0 - g) * (gh - 1)))
    fb.rect(lx, gy, 1, gh, LINE)
    fb.rect(lx - 2, ly - 2, 5, 5, WHITE)
    fb.text_r(149, gy + gh - 8, "1 BEAT", DIM)
    # per-track duck amounts x 160..236
    if track_focus is None:
        fb.text(160, 27, "DUCKED", DIM)
    else:
        fb.text(160, 27, TRACK_NAMES[track_focus], tc(track_focus))
        fb.text_r(236, 27, PP.fmt(PP.MIX[4], amounts[track_focus]), WHITE)
    for t in range(8):
        x = 160 + t * 10
        f = (t == track_focus)
        fb.rect(x, 38, 7, 28, PANEL)
        hpx = int(round(amounts[t] / 127.0 * 28))
        fb.rect(x, 66 - hpx, 7, hpx, tc(t) if (track_focus is None or f) else tc(t, 2))
        if f:
            fb.frame(x - 1, 37, 9, 30, WHITE)
        fb.text(x + 1, 69, PAD_KEYS[t], WHITE if f else tc(t, 1))
    # six parameters, 39 px pitch from x 4
    ps = []
    for nm in names:
        info = PP.GLOBALS[PP.G[nm]]
        ps.append((nm, gtext(nm), None if info["fmt"] == PP.EN else PP.frac(info, gval(nm))))
    for c, (name, txt, frac) in enumerate(ps):
        x = 4 + c * 39
        f = (c == focus)
        if f:
            fb.rrect(x, 80, 37, 32, RAISED)
            fb.rframe(x, 80, 37, 32, CYAN)
        fb.text(x + 3, 83, NOTE_KEYS[c], WHITE if f else DIM)
        fb.text_r(x + 33, 83, name, CYAN if f else TEXT)
        fb.text_r(x + 33, 93, txt, WHITE if f else TEXT)
        fb.rect(x + 3, 104, 31, 4, PANEL if not f else BG)
        if frac is not None:
            fb.rect(x + 3, 104, int(round(frac * 31)), 4, CYAN if f else CYAN_D)
        else:
            for k in range(3):
                fb.rect(x + 3 + k * 11, 104, 9, 4, CYAN_D if k != 1 else (CYAN if f else DIM))
    mini_strip(fb, 116, pat[0], 0, play=6)
    footer(fb, [("A-K", TEXT), (" EDIT THAT TRACK'S DUCK AMOUNT", DIM)])


def fx_page(fb, pat, tab, focus):
    names = PP.FX_PAGES[tab][1]
    ps = [PI(PP.GLOBALS[PP.G[n]], gval(n)) for n in names] + [None] * (8 - len(names))
    status_bar(fb, focus=("FX", names[focus], gtext(names[focus])))
    fx_tabs(fb, tab)
    big_value(fb, ps[focus]["long"], ps[focus]["text"])
    param_cols(fb, ps, focus, CYAN, CYAN_D, y0=32, bar_h=52)
    mini_strip(fb, 114, pat[0], 0, play=6)
    if tab == 2:
        footer(fb, [("[ ]", CYAN), (" DJ FILTER FROM ANY VIEW", DIM)])
    else:
        footer(fb, [("MIX VIEW SETS EACH TRACK'S SENDS", DIM)])


def fx_master(fb, pat, focus=3):
    fx_page(fb, pat, 2, focus)


def fx_space(fb, pat, focus=1):
    fx_page(fb, pat, 1, focus)


# --------------------------------------------------------------------------
# Pattern pads (SONG view and the OPT overlay)
# --------------------------------------------------------------------------
def pattern_pad(fb, x, y, w, h, n, state, used=True):
    """state: None | 'play' | 'queued' | 'cursor'."""
    if state == "play":
        fb.rrect(x, y, w, h, GREEN)
        ink = BG
    elif used:
        fb.rrect(x, y, w, h, RAISED)
        ink = TEXT
    else:
        fb.rrect(x, y, w, h, PANEL)
        ink = LINE
    if state == "queued":
        fb.rframe(x, y, w, h, AMBER)
        fb.rframe(x + 1, y + 1, w - 2, h - 2, AMBER)
        ink = AMBER
    fb.text_c(x + w // 2, y + (h - 7) // 2, "%02d" % n, ink)


def pattern_overlay(fb, playing=1, queued=5, used=(1, 2, 3, 5, 9)):
    """OPT held: panel y 72..134 over any view, pads at the step-pad positions."""
    fb.rect(0, 72, W, 63, BG)
    fb.rect(0, 73, W, 62, PANEL)
    fb.rect(0, 73, W, 1, AMBER)
    x = chip(fb, 4, 76, "PATTERN", AMBER)
    x = fb.text(x + 6, 77, I("play") + "%02d" % playing, GREEN)
    if queued:
        fb.text(x + 6, 77, "NEXT %02d" % queued, AMBER)
    fb.text_r(235, 77, "+SHIFT = COPY TO", DIM)
    for i in range(16):
        x, y = pad_xy(i)
        st = "play" if i + 1 == playing else ("queued" if i + 1 == queued else None)
        pattern_pad(fb, x, y, PAD_W, PAD_H, i + 1, st, used=(i + 1) in used)


def mute_overlay(fb, mutes=(1, 5), solo=None):
    """FN held: track strip y 84..134 over any view.  Pad row = mute, note row = solo."""
    fb.rect(0, 83, W, 52, BG)
    fb.rect(0, 84, W, 51, PANEL)
    fb.rect(0, 84, W, 1, WHITE)
    chip(fb, 4, 87, "MUTE", WHITE)
    fb.text_r(235, 88, "A-K MUTE   Z-, SOLO", DIM)
    for t in range(8):
        x = 4 + t * 29
        m = t in mutes
        if m:
            fb.rrect(x, 99, 27, 20, BG)
            fb.rframe(x, 99, 27, 20, LINE)
            fb.text(x + 3, 101, PAD_KEYS[t], DIM)
            fb.text_c(x + 14, 110, "MUTE", DIM)
        else:
            fb.rrect(x, 99, 27, 20, tc(t))
            fb.text(x + 3, 101, PAD_KEYS[t], tc(t, 2))
            fb.text_c(x + 14, 110, TRACK_NAMES[t], BG)
        if solo == t:
            fb.rrect(x, 121, 27, 11, WHITE)
            fb.text(x + 3, 123, NOTE_KEYS[t], DIM)
            fb.text(x + 13, 123, "SO", BG)
        else:
            fb.rrect(x, 121, 27, 11, RAISED)
            fb.text(x + 3, 123, NOTE_KEYS[t], LINE)
            fb.text(x + 13, 123, "SO", DIM)


def roll_overlay(fb, rolling=(1,), rate="1/16"):
    """ALT held: strip y 96..134.  Pads roll while held; up/down sets the rate."""
    fb.rect(0, 95, W, 40, BG)
    fb.rect(0, 96, W, 39, PANEL)
    fb.rect(0, 96, W, 1, WHITE)
    x = chip(fb, 4, 99, "ROLL", WHITE)
    fb.text(x + 6, 100, rate, AMBER)
    fb.text_r(235, 100, I("up") + I("down") + " RATE", DIM)
    for t in range(8):
        x = 4 + t * 29
        on = t in rolling
        fb.rrect(x, 111, 27, 20, WHITE if on else tc(t))
        fb.text(x + 3, 113, PAD_KEYS[t], DIM if on else tc(t, 2))
        fb.text_c(x + 14, 122, TRACK_NAMES[t], BG)


def tempo_overlay(fb, bpm=124, swing=54):
    x, y, w, h = 44, 26, 152, 84
    fb.rect(x - 2, y - 2, w + 4, h + 4, BG)
    fb.rrect(x, y, w, h, PANEL)
    fb.rframe(x, y, w, h, WHITE)
    fb.text(x + 8, y + 7, "TEMPO", DIM)
    fb.text(x + 30, y + 20, "%3d" % bpm, WHITE, 3)
    fb.text(x + 90, y + 34, "BPM", DIM)
    for k in range(4):
        fb.rect(x + 112 + k * 8, y + 8, 5, 5, WHITE if k == 1 else LINE)
    fb.text(x + 8, y + 50, "SWING", DIM)
    fb.text_r(x + 68, y + 50, "%d%%" % swing, WHITE)
    fb.rect(x + 76, y + 52, 66, 3, RAISED)
    fb.rect(x + 76, y + 52, int((swing - 50) / 25.0 * 66), 3, WHITE)
    fb.rect(x + 1, y + 62, w - 2, 1, LINE)
    fb.text(x + 8, y + 66, "-/= BPM", TEXT)
    fb.text_r(x + w - 9, y + 66, I("up") + I("down") + " SWING", TEXT)
    fb.text_c(x + w // 2, y + 75, "TAP TAB x3 = TAP TEMPO", DIM)


# --------------------------------------------------------------------------
# SONG view
# --------------------------------------------------------------------------
def song(fb, chain=None, cursor=3, playing_slot=1, song_on=True, playing=3, used=(1, 2, 3, 5, 9)):
    chain = chain or [(1, 4), (2, 4), (3, 2), (5, 2), (3, 4), (9, 8)]
    status_bar(fb, pat=playing, song=playing_slot + 1 if song_on else None, focus=None)
    xx = chip(fb, 92, 1, "SLOT %d" % (cursor + 1), WHITE)
    fb.text(xx + 3, 2, "P%02d" % chain[cursor][0], TEXT)
    fb.text_r(165, 2, "x%d" % chain[cursor][1], AMBER)
    x = chip(fb, 4, 14, "SONG", TEXT)
    if song_on:
        chip(fb, x + 6, 14, I("chain") + "CHAIN ON", GREEN)
    else:
        fb.text(x + 8, 15, "CHAIN OFF", DIM)
    # pattern grid: 8 x 2 cells of 17x17, pitch 19, x 4.., y 28 / 47
    for i in range(16):
        x = 4 + (i % 8) * 19
        y = 28 + (i // 8) * 19
        st = "play" if i + 1 == playing else None
        pattern_pad(fb, x, y, 17, 17, i + 1, st, used=(i + 1) in used)
    helps = [("1-8 Q-I", "PUT PATTERN"), ("SHIFT+", "INSERT PATTERN"), (I("up") + I("down"), "MOVE CURSOR"),
             ("-/=", "REPEATS"), ("ENTER", "CHAIN ON/OFF")]
    for i, (k, d) in enumerate(helps):
        fb.text(4, 71 + i * 10, k, TEXT)
        fb.text(52, 71 + i * 10, d, DIM)
    # chain list x 160..236
    fb.rect(158, 13, 82, 112, PANEL)
    fb.text(162, 16, "CHAIN", DIM)
    fb.text_r(235, 16, "%d/32" % len(chain), DIM)
    rows = chain + [None]
    for i, sl in enumerate(rows[:10]):
        y = 27 + i * 10
        cur = (i == cursor)
        if cur:
            fb.rrect(160, y - 1, 78, 9, WHITE)
        ink = BG if cur else TEXT
        if sl is None:
            fb.text(162, y, "   END", BG if cur else DIM)
            continue
        if song_on and i == playing_slot:
            fb.text(162, y, I("play"), GREEN_D if cur else GREEN)
        fb.text(170, y, "%2d" % (i + 1), DIM if not cur else LINE)
        fb.text(188, y, "P%02d" % sl[0], ink)
        fb.text(212, y, "x%d" % sl[1], AMBER_D if cur else AMBER)
    footer(fb, [("DEL", TEXT), (" REMOVE SLOT   ", DIM), ("OPT+STEP", TEXT), (" QUEUE", DIM)])


# --------------------------------------------------------------------------
# PROJECT view
# --------------------------------------------------------------------------
def project(fb, mode="LOAD", cursor=2):
    status_bar(fb, playing=False, peak=(0, 0), duck=1.0, focus=None)
    fb.text(92, 2, "ACIDJAM*", TEXT)
    x = chip(fb, 4, 14, "PROJECT", TEXT)
    for i, t in enumerate(["LOAD", "SAVE AS"]):
        if t == mode:
            x = chip(fb, x + 6, 14, t, WHITE)
        else:
            fb.text(x + 8, 15, t, DIM)
            x += len(t) * 6 + 9
    fb.text_r(235, 15, "SD " + I("check"), CYAN)
    files = [("ROM", "DEMO", "124"), ("INT", "ACIDJAM", "126"), ("INT", "WAREHOUS", "130"),
             ("SD", "BASSHAUS", "124"), ("SD", "LIVESET1", "122"), ("SD", "SKETCH07", "118")]
    if mode == "SAVE AS":
        files = [("", "+ NEW NAME...", "")] + files
    for i, (mnt, name, size) in enumerate(files[:9]):
        y = 28 + i * 10
        cur = (i == cursor)
        if cur:
            fb.rrect(2, y - 1, 236, 9, WHITE)
        if mnt:
            c = {"SD": CYAN, "INT": AMBER, "ROM": GREEN}[mnt]
            if cur:
                c = {"SD": CYAN_D, "INT": AMBER_D, "ROM": GREEN_D}[mnt]
            fb.text(6, y, mnt, c)
        fb.text(30, y, name, BG if cur else TEXT)
        if size:
            fb.text_r(214, y, size, LINE if cur else DIM)
            fb.text_r(232, y, "BPM", LINE if cur else RAISED)
    if mode == "LOAD":
        footer(fb, [("ENTER", TEXT), (" LOAD  ", DIM), ("DEL", TEXT), (" DELETE  ", DIM),
                    ("\\", TEXT), (" SAVE TAB", DIM)])
    else:
        footer(fb, [("ENTER", TEXT), (" SAVE / OVERWRITE  ", DIM), ("\\", TEXT), (" LOAD TAB", DIM)])


def name_dialog(fb, name="NIGHTBUS", mount="SD"):
    x, y, w, h = 30, 34, 180, 67
    dialog(fb, "SAVE AS", [])
    fb.rect(x + 8, y + 18, w - 16, 20, BG)
    xx = fb.text(x + 13, y + 21, name, WHITE, 2)
    fb.rect(xx, y + 21, 10, 14, AMBER)
    fb.text(x + 8, y + 43, "TAB", TEXT)
    fb.text(x + 30, y + 43, "SAVE TO", DIM)
    for i, m in enumerate(["INT", "SD"]):
        if m == mount:
            chip(fb, x + 78 + i * 26, y + 42, m, CYAN if m == "SD" else AMBER)
        else:
            fb.text(x + 80 + i * 26, y + 43, m, DIM)
    fb.text(x + 8, y + 55, "ENTER", TEXT)
    fb.text(x + 44, y + 55, "SAVE", DIM)
    fb.text(x + 86, y + 55, "ESC", TEXT)
    fb.text(x + 110, y + 55, "CANCEL", DIM)


# --------------------------------------------------------------------------
# HELP overlay
# --------------------------------------------------------------------------
# (label, ink colour, fill colour) for each of the 56 keys in GROOVE view.
def help_keys_groove(sel=6):
    T = lambda t, s: (s, BG, tc(t))
    stepc = lambda s: (s, WHITE, RAISED)
    note = lambda s: (s, BG, tc(sel, 1))
    view = lambda s: (s, BG, TEXT)
    mod = lambda s: (s, TEXT, LINE)
    return [
        [("ES", BG, DIM)] + [stepc("%d" % n) for n in range(1, 9)] + [view("SN"), view("MX"),
         ("-", BG, AMBER), ("+", BG, AMBER), ("ER", TEXT, RAISED)],
        [("BP", BG, WHITE)] + [stepc("%d" % n) for n in range(9, 17)] + [view("FX"), view("SG"),
         ("LP", BG, CYAN), ("HP", BG, CYAN), view("PJ")],
        [mod("MU"), mod("SH"), T(0, "KK"), T(1, "SN"), T(2, "CP"), T(3, "CH"), T(4, "OH"),
         T(5, "PC"), T(6, "BS"), T(7, "LD"), ("CD", BG, tc(7, 1)), (I("up"), BG, tc(sel, 1)),
         ("PG", WHITE, RAISED), (I("rec"), WHITE, RED)],
        [mod("CT"), mod("PT"), mod("RL")] + [note("%d" % n) for n in range(1, 9)] +
        [(I("down"), BG, tc(sel, 1)), ("?", BG, WHITE), (I("play"), BG, GREEN)],
    ]


def help_overlay(fb, keys, title="KEYS", sub="GROOVE", legend=None):
    fb.rect(0, 0, W, H, BG)
    fb.rect(0, 0, W, 11, WHITE)
    x = fb.text(3, 2, title, BG)
    fb.text(x + 4, 2, sub, LINE)
    fb.text_r(236, 2, "FN/SFT/CTL/OPT/ALT/TAB", LINE)
    for r in range(4):
        for c in range(14):
            lab, ink, fill = keys[r][c]
            x, y = 1 + c * 17, 14 + r * 19
            fb.rrect(x, y, 16, 17, fill)
            fb.text_c(x + 8, y + 5, lab, ink)
    y = 92
    for ln in legend or []:
        x = 3
        for s, c in ln:
            x = fb.text(x, y, s, c)
        y += 9


GROOVE_LEGEND = [
    [("1-16", WHITE), (" STEP: TAP=ON/OFF HOLD=EDIT", DIM)],
    [("KK-LD", tc(0)), (" PADS ", DIM), ("1-8", tc(6, 1)), (" NOTES ", DIM), ("-+", AMBER), (" VALUE ", DIM),
     ("LP HP", CYAN), (" DJ", DIM)],
    [("SN MX FX SG PJ", TEXT), (" VIEWS ", DIM), ("ES", DIM), (" HOME ", DIM), ("BP", WHITE), (" TEMPO", DIM)],
    [("MU", TEXT), ("+PAD MUTE ", DIM), ("PT", TEXT), ("+STEP PATTERN ", DIM), ("ER", TEXT), (" ERASE", DIM)],
    [("TAP / : EVERY KEY EXPLAINS ITSELF", AMBER)],
]


def help_explain(fb, keys):
    help_overlay(fb, keys, "KEYS", "GROOVE", [])
    # pressed key highlighted + description box
    x, y = 1 + 9 * 17, 14 + 3 * 19
    fb.rframe(x - 1, y - 1, 18, 19, WHITE)
    fb.rrect(2, 93, 236, 40, PANEL)
    fb.rframe(2, 93, 236, 40, AMBER)
    xx = chip(fb, 7, 97, "M", WHITE)
    fb.text(xx + 5, 98, "NOTE 7: G2 ON BASS", WHITE)
    fb.text(7, 109, "PLAYS THE SELECTED TRACK.", TEXT)
    fb.text(7, 118, "HOLD A STEP + KEY: SET ITS NOTE", TEXT)
    fb.text_r(233, 98, "/ EXIT", AMBER)


def boot_banner(fb):
    """First-run hint, drawn over the overview until the first key press."""
    x, y, w, h = 30, 30, 180, 44
    fb.rect(x - 2, y - 2, w + 4, h + 4, BG)
    fb.rrect(x, y, w, h, PANEL)
    fb.rframe(x, y, w, h, GREEN)
    fb.text_c(x + w // 2, y + 6, "POCKET GROOVE", WHITE, 2)
    fb.text(x + 16, y + 26, "SPACE", GREEN)
    fb.text(x + 52, y + 26, "PLAY", TEXT)
    fb.text(x + 100, y + 26, "/", GREEN)
    fb.text(x + 112, y + 26, "ALL KEYS", TEXT)
    fb.text_c(x + w // 2, y + 35, "1-8 Q-I STEPS   A-K PADS", DIM)


# --------------------------------------------------------------------------
# Font specimen
# --------------------------------------------------------------------------
def font_specimen():
    fb = FB()
    fb.text(2, 2, "FONT_S 5x7 in 6x8 cell", AMBER)
    lines = [" !\"#$%&'()*+,-./0123456789:;<=>?",
             "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_",
             "`abcdefghijklmnopqrstuvwxyz{|}~",
             "".join(chr(0x80 + i) for i in range(17))]
    for i, ln in enumerate(lines):
        fb.text(2, 13 + i * 10, ln, WHITE)
    fb.text(2, 56, "FONT_L 2x", AMBER)
    fb.text(2, 66, "0123456789 A#2 +12", WHITE, 2)
    fb.text(2, 86, "FONT_XL 3x", AMBER)
    fb.text(2, 96, "124 BPM", WHITE, 3)
    fb.text(140, 96, "Sphinx of", TEXT)
    fb.text(140, 106, "black quartz", TEXT)
    fb.text(140, 116, "judge my vow", TEXT)
    return fb.save("font.png")


A_MIN_OCT1 = [33, 35, 36, 38, 40, 41, 43, 45]


def render_all():
    out = [font_specimen()]
    pat = demo_pattern()

    def shot(name, fn):
        fb = FB()
        fn(fb)
        out.append(fb.save(name))

    shot("groove-drum.png", lambda fb: groove(
        fb, pat, 0, play=6, flash=(3,)))
    shot("groove-bass.png", lambda fb: groove(
        fb, pat, 6, play=6, flash=(3, 6)))

    def lead(fb):
        groove(fb, pat, 7, play=10, flash=(0, 3, 7), focus=(7, "CUT", "70"))
    shot("groove-lead.png", lead)

    def step_edit(fb):
        groove(fb, pat, 6, play=6, held=13, focus=(6, "CUT", "58"))
        step_edit_panel(fb, 6, 13, pat[6][13], 0, A_MIN_OCT1)
        step_held_footer(fb)
    shot("step-edit.png", step_edit)

    def step_edit_drum(fb):
        groove(fb, pat, 3, play=6, held=15, focus=(3, "DCY", "58"))
        step_edit_panel(fb, 3, 15, pat[3][15], 3, None)
        step_held_footer(fb)
    shot("step-edit-drum.png", step_edit_drum)

    def step_edit_lead(fb):
        groove(fb, pat, 7, play=6, held=10, focus=(7, "CUT", "70"))
        step_edit_panel(fb, 7, 10, pat[7][10], 6, [57, 59, 60, 62, 64, 65, 67, 69])
        step_held_footer(fb)
    shot("step-edit-lead.png", step_edit_lead)

    def stepwrite(fb):
        groove(fb, pat, 6, playing=False, rec=True, cursor=4,
               footer_extra=[("STEP WRITE", RED), (" OCT1", TEXT)])
    shot("groove-stepwrite.png", stepwrite)

    def live(fb):
        groove(fb, pat, 6, play=14, rec=True, mutes=(1, 5), queued=5, flash=(0, 6))
    shot("groove-live.png", live)

    def toast(fb):
        groove(fb, pat, 6, play=6)
        toast_value(fb, "DJ FILTER", "LP26", 38 / 127.0, CYAN, bipolar=True, sub="MASTER")
    shot("toast-value.png", toast)

    def boot(fb):
        groove(fb, pat, 0, playing=False)
        boot_banner(fb)
    shot("boot.png", boot)

    def page2(fb):
        p2 = demo_pattern()
        p2[6][4] = S(note=45, acc=1)
        p2[6][15] = S(note=31, ratch=3)
        p2[1][13] = S(vel=70)
        p2[1][14] = S(vel=90, ratch=2)
        groove(fb, p2, 6, play=None, pages=2, shown=1, total=32, play_page=0)
    shot("groove-page2.png", page2)

    shot("sound.png", lambda fb: sound(fb, pat))

    def sound_lock(fb):
        sound(fb, pat, held=13, params=page_params(6, 0, locks={"CUT": 110}))
    shot("sound-lock.png", sound_lock)

    shot("mix.png", lambda fb: mix(fb, flash=(3, 6), mutes=(5,)))
    shot("mix-pan.png", lambda fb: mix(fb, row=4, values=[64, 64, 54, 76, 50, 84, 64, 64]))
    shot("mix-duck.png", lambda fb: mix(fb, row=1,
                                        flash=(0,)))
    shot("fx-duck.png", lambda fb: fx_duck(fb, pat))
    shot("fx-space.png", lambda fb: fx_space(fb, pat))
    shot("fx-master.png", lambda fb: fx_master(fb, pat))
    shot("song.png", lambda fb: song(fb))

    def patov(fb):
        groove(fb, pat, 0, play=6, queued=5)
        pattern_overlay(fb)
    shot("pattern-overlay.png", patov)

    def muteov(fb):
        sound(fb, pat)
        mute_overlay(fb)
    shot("mute-overlay.png", muteov)

    def rollov(fb):
        groove(fb, pat, 1, play=13, flash=(1,), focus=(1, "TUNE", "60"))
        roll_overlay(fb)
    shot("roll-overlay.png", rollov)

    def tempo(fb):
        groove(fb, pat, 0, play=6)
        tempo_overlay(fb)
    shot("tempo-overlay.png", tempo)

    shot("project-load.png", lambda fb: project(fb, "LOAD", 2))

    def saveas(fb):
        project(fb, "SAVE AS", 0)
        name_dialog(fb)
    shot("project-save.png", saveas)

    def confirm(fb):
        project(fb, "LOAD", 3)
        dialog(fb, "LOAD BASSHAUS?", [("ACIDJAM HAS UNSAVED CHANGES", TEXT),
                                      ("", TEXT),
                                      ("ENTER  DISCARD AND LOAD", WHITE),
                                      ("ESC    CANCEL", DIM)])
    shot("confirm.png", confirm)

    def undo(fb):
        p2 = demo_pattern()
        p2[6] = [S(on=0) for _ in range(16)]
        groove(fb, p2, 6, play=6, flash=(0, 3))
        toast_msg(fb, "BASS CLEARED  CTRL+Z UNDO", WHITE, AMBER)
    shot("toast-undo.png", undo)

    shot("sound-kick.png", lambda fb: sound(fb, pat, track=0, focus=1, play=6))

    def fx_duck_track(fb):
        fx_duck(fb, pat, focus=None, track_focus=6)
    shot("fx-duck-track.png", fx_duck_track)

    shot("help.png", lambda fb: help_overlay(fb, help_keys_groove(), "KEYS", "GROOVE", GROOVE_LEGEND))
    shot("help-explain.png", lambda fb: help_explain(fb, help_keys_groove()))
    return out
