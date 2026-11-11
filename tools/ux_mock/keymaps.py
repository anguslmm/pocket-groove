"""Keyboard-map diagrams, one per mode (docs/ux/keys-*.png).

Logical canvas 528 x N drawn with the same palette and font as the screens,
saved at 2x.  Each key shows its keycap legend (top left, small) and what it
does in that mode (up to 3 lines of 5 characters).  These tables are the same
as docs/UX.md section 6 ("Key maps"); if you change one, change the other.
"""
from pgfb import (FB, tc, BG, PANEL, RAISED, LINE, DIM, TEXT, WHITE, AMBER, RED, GREEN, CYAN,
                  TRACK_NAMES)

CAPS = [
    ["`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "DEL"],
    ["TAB", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "\\"],
    ["FN", "SHF", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "ENT"],
    ["CTL", "OPT", "ALT", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "SPC"],
]
KW, KH, KP_X, KP_Y = 36, 40, 37, 42
X0, Y0 = 5, 24
SEL = 6  # diagrams assume the BASS track is selected

# group -> (fill, ink, cap ink)
GROUPS = {
    "step": (RAISED, WHITE, DIM),
    "note": (tc(SEL, 1), BG, tc(SEL, 3)),
    "value": (AMBER, BG, 11),
    "filter": (CYAN, BG, 14),
    "view": (TEXT, BG, DIM),
    "play": (GREEN, BG, 13),
    "rec": (RED, WHITE, 12),
    "mod": (LINE, WHITE, DIM),
    "nav": (RAISED, TEXT, DIM),
    "field": (WHITE, BG, DIM),
    "type": (PANEL, TEXT, LINE),
    "off": (BG, LINE, LINE),
}


def K(group, *lines):
    return (group, list(lines))


OFF = K("off", "-")


def pad(t, *lines):
    return ("pad%d" % t, list(lines) or ["PAD", TRACK_NAMES[t]])


def globals_row():
    """The keys that mean the same thing in GROOVE, SOUND, MIX, FX, SONG and PROJECT."""
    k = [[None] * 14 for _ in range(4)]
    k[0][0] = K("nav", "HOME", "/ESC")
    k[0][9], k[0][10] = K("view", "SOUND"), K("view", "MIX")
    k[1][9], k[1][10], k[1][13] = K("view", "FX"), K("view", "SONG"), K("view", "PROJ")
    k[1][0] = K("nav", "TEMPO", "hold", "tap x3")
    k[1][11], k[1][12] = K("filter", "DJ", "FILT", "< LP"), K("filter", "DJ", "FILT", "HP >")
    k[2][0], k[2][1] = K("mod", "FN", "mute"), K("mod", "SHIFT")
    k[3][0], k[3][1], k[3][2] = K("mod", "CTRL", "edit"), K("mod", "OPT", "patt"), K("mod", "ALT", "roll")
    for t in range(8):
        k[2][2 + t] = pad(t)
    k[3][12] = K("nav", "HELP", "hold", "tap")
    k[3][13] = K("play", "PLAY", "STOP")
    return k


def base_groove():
    k = globals_row()
    for n in range(8):
        k[0][1 + n] = K("step", "STEP", "%d" % (n + 1))
        k[1][1 + n] = K("step", "STEP", "%d" % (n + 9))
        k[3][3 + n] = K("note", "NOTE", "%d" % (n + 1))
    k[0][11], k[0][12] = K("value", "VALUE", "-"), K("value", "VALUE", "+")
    k[0][13] = K("nav", "ERASE", "hold")
    k[2][10] = K("nav", "CHORD", "mode", "(lead)")
    k[2][11], k[3][11] = K("note", "OCT", "UP"), K("note", "OCT", "DOWN")
    k[2][12] = K("nav", "PAGE")
    k[2][13] = K("rec", "REC")
    return k


def mode_groove():
    return base_groove(), "GROOVE  (home view: 8-track overview + 16 step keys)", [
        [("STEP", WHITE), ("   tap = on/off     hold = STEP EDIT (keys-step-held.png)", DIM)],
        [("PAD", tc(0)), ("    play that track and select it (SHIFT+pad: select without sound)", DIM)],
        [("NOTE", tc(SEL, 1)), ("   play the selected track in the project scale (drums: 8 velocities)", DIM)],
        [("VALUE", AMBER), ("  change the parameter in the status-bar chip; tap = 1, hold = sweep", DIM)],
        [("DJ FILT", CYAN), (" master filter in every view; both keys = off; SHIFT = BASS CUTOFF", DIM)],
        [("REC", RED), ("    playing: pads and notes are recorded; stopped: step-write", DIM)],
        [("ERASE", TEXT), ("  hold while playing: wipe the selected track as the playhead passes", DIM)],
        [("PAGE", TEXT), ("   next 16 steps of a longer track     CHORD: lead chord mode", DIM)],
        [("Held modifiers (FN SHIFT CTRL OPT ALT TAB): keys-modifiers.png", DIM)],
    ]


def mode_step_held():
    k = base_groove()
    fields = ["VEL", "GATE", "PROB", "RATCH", "ACCNT", "SLIDE", "CHORD", "LOCK"]
    for c in range(1, 9):
        k[0][c] = K("step", "COPY", "HERE")
        k[1][c] = K("step", "COPY", "HERE")
    k[0][5] = K("field", "HELD", "STEP")
    k[0][11], k[0][12] = K("value", "FIELD", "-"), K("value", "FIELD", "+")
    k[0][13] = K("nav", "REMOV", "LOCK")
    for t in range(8):
        k[2][2 + t] = K("field", "FIELD" if t not in (4, 5) else "FLIP", fields[t])
    k[2][10] = K("nav", "CHORD", "next")
    k[2][11], k[3][11] = K("note", "NOTE", "+1", "semi"), K("note", "NOTE", "-1", "semi")
    k[2][12] = K("nav", "PAGE", "(copy", "over)")
    k[2][13] = OFF
    for n in range(8):
        k[3][3 + n] = K("note", "SET", "NOTE", "%d" % (n + 1))
    k[1][0] = OFF
    return k, "STEP EDIT  (any step key held; here step 5, BASS)", [
        [("FIELD", WHITE), ("    pick what VALUE -/+ changes on this step (shown highlighted)", DIM)],
        [("FLIP", WHITE), ("     accent / slide toggle at once.   CHORD: lead only. Drums: S = PITCH", DIM)],
        [("SET NOTE", tc(SEL, 1)), (" write that scale note (drums: velocity) and hear it", DIM)],
        [("NOTE +/-", tc(SEL, 1)), (" one semitone; with SHIFT one octave", DIM)],
        [("COPY HERE", WHITE), (" tap other steps: the held step is copied onto them", DIM)],
        [("LOCK", WHITE), ("     K + VALUE locks the focused param on this step; DEL removes it", DIM)],
        [("Views still switch: hold a step and press SOUND/MIX to lock any parameter.", AMBER)],
        [("A short tap on a lit step turns it off; any edit while held keeps it on.", DIM)],
    ]


def mode_step_write():
    k = base_groove()
    for c in range(1, 9):
        k[0][c] = K("step", "MOVE", "CURS", "%d" % c)
        k[1][c] = K("step", "MOVE", "CURS", "%d" % (c + 8))
    for n in range(8):
        k[3][3 + n] = K("note", "WRITE", "+ NEXT", "%d" % (n + 1))
    k[0][13] = K("nav", "REST", "+ NEXT")
    k[2][13] = K("rec", "EXIT", "REC")
    k[3][13] = K("play", "PLAY", "(live", "rec)")
    return k, "STEP-WRITE  (REC pressed while stopped: 303-style note entry)", [
        [("WRITE", tc(SEL, 1)), ("    write the note at the cursor, cursor moves on (drums: a hit)", DIM)],
        [("SHIFT", WHITE), ("    + note = with accent.  Keep the previous note key held = SLIDE into this one", DIM)],
        [("REST", TEXT), ("     clear the cursor step and move on.   MOVE CURS: put the cursor on a step", DIM)],
        [("Hold a step key = STEP EDIT as usual.  PLAY starts live recording from step 1.", DIM)],
    ]


def mode_sound():
    k = base_groove()
    for n in range(8):
        k[3][3 + n] = K("field", "PARAM", "%d" % (n + 1))
    k[0][9] = K("view", "SOUND", "next", "page")
    k[2][11], k[3][11] = K("nav", "PAGE", "PREV"), K("nav", "PAGE", "NEXT")
    k[2][10] = OFF
    k[0][13] = K("nav", "REMOV", "LOCK", "(held)")
    k[2][12] = K("nav", "STEP", "PAGE")
    return k, "SOUND  (selected track's sound: 8 parameters per page, up to 3 pages)", [
        [("PARAM", WHITE), ("  focus a column; VALUE -/+ changes it (hold = sweep, SHIFT = coarse)", DIM)],
        [("STEP", WHITE), ("   toggles steps as in GROOVE; HOLD a step + VALUE = lock on that step", DIM)],
        [("PAD", tc(0)), ("    show another track's sound (and hear it)", DIM)],
        [("The focused parameter stays on VALUE -/+ when you go back to GROOVE.", AMBER)],
    ]


def mode_fx():
    k = base_groove()
    names = ["DSRC", "DDEP", "DHLD", "DREL", "DCRV", "DFX", "-", "-"]
    for n in range(8):
        k[3][3 + n] = K("field", "PARAM", names[n]) if n < 6 else OFF
    for t in range(8):
        k[2][2 + t] = pad(t, "DUCK", TRACK_NAMES[t])
    k[1][9] = K("view", "FX", "next", "tab")
    k[2][11], k[3][11] = K("nav", "TAB", "PREV"), K("nav", "TAB", "NEXT")
    k[2][10] = OFF
    k[0][13] = OFF
    k[2][12] = OFF
    return k, "FX  (globals: DUCK / SPACE / MASTER tabs; shown: DUCK)", [
        [("PARAM", WHITE), ("  focus one global (DUCK tab: source, depth, hold, release, curve, FX)", DIM)],
        [("PAD", tc(0)), ("    DUCK tab: select the track and put its DUCK AMOUNT on VALUE -/+", DIM)],
        [("TAB", TEXT), ("    FX again or the arrows: DUCK > SPACE > MASTER", DIM)],
        [("Globals cannot be locked on steps; per-track DUCK amount can.", DIM)],
    ]


def mode_mix():
    k = base_groove()
    rows = ["LEVEL", "DUCK", "DELAY", "REVRB", "PAN"]
    for n in range(8):
        k[3][3 + n] = K("field", "ROW", rows[n]) if n < 5 else OFF
    k[2][11], k[3][11] = K("nav", "ROW", "PREV"), K("nav", "ROW", "NEXT")
    k[0][10] = K("view", "MIX", "next", "row")
    k[2][10] = OFF
    k[0][13] = K("nav", "REMOV", "LOCK", "(held)")
    k[2][12] = K("nav", "STEP", "PAGE")
    for t in range(8):
        k[2][2 + t] = pad(t, "STRIP", TRACK_NAMES[t])
    return k, "MIX  (8 channel strips; one mixer row at a time)", [
        [("ROW", WHITE), ("   what the faders show: level, duck amount, delay send, reverb send, pan", DIM)],
        [("STRIP", tc(0)), (" select (and hear) a track; VALUE -/+ moves its fader", DIM)],
        [("PAN", TEXT), ("   kept for completeness: the headphone output is mono", DIM)],
        [("STEP", WHITE), ("  as in SOUND: hold + VALUE locks this mixer value on a step", DIM)],
    ]


def mode_song():
    k = base_groove()
    for c in range(1, 9):
        k[0][c] = K("field", "PUT", "P%02d" % c)
        k[1][c] = K("field", "PUT", "P%02d" % (c + 8))
    k[0][11], k[0][12] = K("value", "REPS", "-"), K("value", "REPS", "+")
    k[0][13] = K("nav", "DEL", "SLOT")
    k[2][11], k[3][11] = K("nav", "SLOT", "UP"), K("nav", "SLOT", "DOWN")
    k[2][13] = K("play", "CHAIN", "ON/", "OFF")
    k[2][10], k[2][12] = OFF, OFF
    return k, "SONG  (the chain: up to 32 slots of pattern x repeats)", [
        [("PUT", WHITE), ("    write that pattern into the slot under the cursor (on END: append)", DIM)],
        [("SHIFT", WHITE), ("  + PUT = insert before the cursor instead of overwriting", DIM)],
        [("REPS", AMBER), ("   repeats of the slot under the cursor, 1-64", DIM)],
        [("CHAIN", GREEN), ("  song mode on/off; starts from slot 1 at the next pattern end", DIM)],
        [("Pads, notes, mutes, tempo, DJ filter and transport work as everywhere.", DIM)],
    ]


def mode_project():
    k = globals_row()
    for r in range(4):
        for c in range(14):
            if k[r][c] is None:
                k[r][c] = OFF
    for n in range(8):
        k[3][3 + n] = K("note", "NOTE", "%d" % (n + 1))
    k[1][13] = K("view", "LOAD/", "SAVE", "tab")
    k[2][11], k[3][11] = K("nav", "LIST", "UP"), K("nav", "LIST", "DOWN")
    k[2][13] = K("play", "LOAD/", "SAVE")
    k[0][13] = K("rec", "DEL", "FILE")
    return k, "PROJECT  (file list on internal flash and SD card)", [
        [("LOAD/SAVE", GREEN), (" ENTER acts on the row under the cursor; destructive actions ask first", DIM)],
        [("CTRL", WHITE), ("+S save   CTRL+N new project (from any view)", DIM)],
        [("Loading stops the transport.  Saving never does.", DIM)],
    ]


def mode_text():
    k = [[K("type", c if len(c) == 1 and (c.isalnum() or c == "-") else "") for c in row] for row in CAPS]
    for r in range(4):
        for c in range(14):
            if k[r][c][1] == [""]:
                k[r][c] = OFF
    k[0][0] = K("nav", "ESC", "CANCL")
    k[0][13] = K("nav", "BACK", "SPACE")
    k[1][0] = K("view", "INT/", "SD")
    k[2][13] = K("play", "OK")
    k[3][13] = K("type", "_")
    return k, "NAME ENTRY  (save as: up to 8 characters, A-Z 0-9 - _)", [
        [("Letters are stored upper case.  SPACE types an underscore.  Audio keeps playing.", DIM)],
        [("The only modal text mode: here SPACE does not start/stop the transport.", AMBER)],
    ]


def mode_modifiers():
    k = [[OFF for _ in range(14)] for _ in range(4)]
    for c in range(1, 9):
        k[0][c] = K("step", "O PAT", "S LEN")
        k[1][c] = K("step", "O PAT", "S LEN")
    for t in range(8):
        k[2][2 + t] = ("pad%d" % t, ["F MUT", "S SEL", "A ROL"])
    cmd = {0: "C UND", 1: "C CUT", 2: "C CPY", 3: "C PST", 5: "C NEW"}
    for n in range(8):
        k[3][3 + n] = K("note", "F SOL", "S ACC", cmd[n]) if n in cmd else K("note", "F SOL", "S ACC")
    k[2][3] = ("pad1", ["F MUT", "S SEL", "C SAV"])
    k[0][11], k[0][12] = K("value", "S x8", "T BPM"), K("value", "S x8", "T BPM")
    k[1][11], k[1][12] = K("filter", "S BAS", "CUT"), K("filter", "S BAS", "CUT")
    k[2][11], k[3][11] = K("nav", "T SWG", "A RATE"), K("nav", "T SWG", "A RATE")
    k[2][12] = K("nav", "S ADD", "PAGE")
    k[3][13] = K("play", "S RE-", "START")
    k[2][10] = K("nav", "S CHD", "BACK")
    k[1][0] = K("mod", "T =", "TAB")
    k[2][0] = K("mod", "F =", "FN")
    k[2][1] = K("mod", "S =", "SHIFT")
    k[3][0] = K("mod", "C =", "CTRL")
    k[3][1] = K("mod", "O =", "OPT")
    k[3][2] = K("mod", "A =", "ALT")
    return k, "MODIFIER COMBOS  (hold the left-edge key, tap the key shown)", [
        [("F", WHITE), (" FN+pad = mute    FN+note key = solo the track above it", DIM)],
        [("S", WHITE), (" SHIFT+step = pattern length   SHIFT+pad = select silently   SHIFT+note = accent", DIM)],
        [("  SHIFT+VALUE = 8x steps    SHIFT+[ ] = BASS CUTOFF sweep    SHIFT+' = add a page", DIM)],
        [("C", WHITE), (" CTRL+Z undo/redo   X cut   C copy   V paste the selected track", DIM)],
        [("  CTRL+SHIFT+X/C = cut/copy the whole pattern    CTRL+S save    CTRL+N new", DIM)],
        [("  CTRL+SHIFT+step = length of the selected track only (polymeter)", DIM)],
        [("O", WHITE), (" OPT+step = queue pattern 1-16    OPT+SHIFT+step = copy this pattern there + queue", DIM)],
        [("A", WHITE), (" ALT+pad = roll that track while held    ALT+arrows = roll rate", DIM)],
        [("T", WHITE), (" TAB+VALUE = BPM    TAB+arrows = swing    tap TAB 3x = tap tempo", DIM)],
        [("Every combo is one held key + one tapped key (plus SHIFT for the variants).", AMBER)],
    ]


MODES = (("keys-groove.png", mode_groove), ("keys-step-held.png", mode_step_held),
         ("keys-step-write.png", mode_step_write), ("keys-sound.png", mode_sound),
         ("keys-mix.png", mode_mix), ("keys-fx.png", mode_fx), ("keys-song.png", mode_song),
         ("keys-project.png", mode_project), ("keys-text.png", mode_text),
         ("keys-modifiers.png", mode_modifiers))


def draw(name, keys, title, legend):
    h = Y0 + 4 * KP_Y + 6 + len(legend) * 10 + 4
    fb = FB(528, h)
    fb.rect(0, 0, 528, 17, PANEL)
    fb.text(6, 5, "POCKET GROOVE", DIM)
    fb.text(90, 5, title, WHITE)
    for r in range(4):
        for c in range(14):
            group, lines = keys[r][c]
            x, y = X0 + c * KP_X, Y0 + r * KP_Y
            if group.startswith("pad"):
                t = int(group[3:])
                fill, ink, capink = tc(t), BG, tc(t, 2)
            else:
                fill, ink, capink = GROUPS[group]
            if group == "off":
                fb.rframe(x, y, KW, KH, PANEL)
            else:
                fb.rrect(x, y, KW, KH, fill)
            fb.text(x + 3, y + 3, CAPS[r][c], capink)
            if group == "off":
                continue
            n = len(lines)
            ty = y + 13 + (3 - n) * 4
            for ln in lines:
                assert len(ln) <= 6, "key label too long: %r" % ln
                fb.text_c(x + KW // 2, ty, ln, ink)
                ty += 9
    y = Y0 + 4 * KP_Y + 6
    for ln in legend:
        x = 6
        for s, col in ln:
            x = fb.text(x, y, s, col)
        assert x <= 526, "legend line too long by %d px: %r" % (x - 526, ln)
        y += 10
    return fb.save(name, scale=2)


def render_all():
    out = []
    for name, fn in MODES:
        keys, title, legend = fn()
        out.append(draw(name, keys, title, legend))
    return out
