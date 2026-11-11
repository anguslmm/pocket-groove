"""Real parameter tables (copied from lib/pg/src/pg/{drums,bass,lead}/*.cpp and
engine/globals.cpp, Nov 2026) and the display-format rules of docs/UX.md
section "Value display".  The firmware UI never hard-codes these: it reads
Engine::trackParamInfo / globalParamInfo.  The mockups use this copy.
"""

N, PCT, BI, EN, ONOFF, SEMI, BPM = "Number", "Percent", "Bipolar", "Enum", "OnOff", "Semitones", "Bpm"


def p(name, long, lo, hi, d, fmt=N, enum=None):
    return dict(name=name, long=long, min=lo, max=hi, d=d, fmt=fmt, enum=enum)


KICK = [p("TUNE", "Tune", 0, 127, 60), p("DCY", "Decay", 0, 127, 46), p("PNCH", "Punch", 0, 127, 76),
        p("SWP", "Sweep Time", 0, 127, 58), p("CLK", "Click", 0, 127, 72), p("DRV", "Drive", 0, 127, 28),
        p("TONE", "Tone", 0, 127, 100), p("HOLD", "Hold", 0, 127, 30), p("HARM", "Harmonics", 0, 127, 36),
        p("CTON", "Click Tone", 0, 127, 56)]
SNARE = [p("TUNE", "Tune", 0, 127, 60), p("DCY", "Decay", 0, 127, 56), p("SNAP", "Snappy", 0, 127, 80),
         p("TONE", "Tone", 0, 127, 70), p("BODY", "Body Decay", 0, 127, 52), p("BEND", "Pitch Bend", 0, 127, 60),
         p("RING", "Ring", 0, 127, 64), p("DTUN", "Detune", 0, 127, 50), p("DRV", "Drive", 0, 127, 26)]
CLAP = [p("TONE", "Tone", 0, 127, 57), p("DCY", "Decay", 0, 127, 56), p("SPRD", "Spread", 0, 127, 64),
        p("TAIL", "Tail", 0, 127, 70), p("BRST", "Bursts", 0, 4, 3), p("RES", "Resonance", 0, 127, 50),
        p("AIR", "Air", 0, 127, 48), p("DRV", "Drive", 0, 127, 30)]
CHH = [p("TUNE", "Tune", 0, 127, 64), p("DCY", "Decay", 0, 127, 58), p("TONE", "Tone", 0, 127, 80),
       p("MTL", "Metal", 0, 127, 84), p("COLR", "Colour", 0, 127, 56), p("SNAP", "Snap", 0, 127, 40),
       p("SPRD", "Spread", 0, 127, 64)]
OHH = [p("TUNE", "Tune", 0, 127, 64), p("DCY", "Decay", 0, 127, 52), p("TONE", "Tone", 0, 127, 72),
       p("MTL", "Metal", 0, 127, 84), p("COLR", "Colour", 0, 127, 56), p("SNAP", "Snap", 0, 127, 32),
       p("SPRD", "Spread", 0, 127, 64)]
PERC = [p("MODL", "Model", 0, 5, 1, EN, ["TOM", "RIM", "COWB", "CLAV", "SHKR", "ZAP"]),
        p("TUNE", "Tune", 0, 127, 64), p("DCY", "Decay", 0, 127, 64), p("TONE", "Tone", 0, 127, 64),
        p("BEND", "Bend", 0, 127, 64), p("HARM", "Harmonic", 0, 127, 64), p("NOIS", "Noise", 0, 127, 64),
        p("DRV", "Drive", 0, 127, 32)]
RATES = ["2BAR", "1BAR", "1/2", "1/4.", "1/4", "1/8.", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"]
BASS = [p("CUT", "Cutoff", 0, 127, 46), p("RES", "Resonance", 0, 127, 78), p("ENV", "Env Mod", 0, 127, 70),
        p("DCY", "Decay", 0, 127, 44), p("ACC", "Accent", 0, 127, 90), p("DRV", "Drive", 0, 127, 30),
        p("LFO", "LFO Depth", 0, 127, 0), p("RATE", "LFO Rate", 0, 11, 7, EN, RATES),
        p("WAVE", "Waveform", 0, 127, 0), p("SUB", "Sub Level", 0, 127, 56), p("FMIX", "FM Osc Mix", 0, 127, 0),
        p("FM", "FM Depth", 0, 127, 64),
        p("RATI", "FM Ratio", 0, 8, 3, EN, ["0.5", "1", "1.5", "2", "3", "3.5", "4", "5", "7"]),
        p("FOLD", "Fold", 0, 127, 0), p("TENV", "Timbre Env", 0, 127, 90),
        p("LSHP", "LFO Shape", 0, 5, 0, EN, ["TRI", "SINE", "DOWN", "UP", "SQR", "S&H"]),
        p("SLID", "Slide Time", 0, 127, 65), p("ADCY", "Amp Decay", 0, 127, 96), p("REL", "Release", 0, 127, 32),
        p("ATK", "Attack", 0, 127, 18), p("KTRK", "Key Track", 0, 127, 40), p("LTMB", "LFO>Timbre", 0, 127, 0),
        p("SUBO", "Sub Octave", 0, 1, 0, EN, ["UNI", "-1OCT"]), p("TUNE", "Osc Tune", 0, 48, 24, SEMI)]
LEAD = [p("CUT", "Cutoff", 0, 127, 72), p("RES", "Resonance", 0, 127, 30), p("ENV", "Env Amount", 0, 127, 48),
        p("FDCY", "Env Decay", 0, 127, 60),
        p("WAVE", "Osc1 Wave", 0, 7, 0, EN, ["SAW", "PULS", "TRI", "SUPR", "FM1", "FM2", "BELL", "ORGN"]),
        p("SHP", "Shape", 0, 127, 0), p("DET", "Detune", 0, 127, 56), p("WID", "Width", 0, 127, 96, PCT),
        p("ATK", "Attack", 0, 127, 0), p("DCY", "Decay", 0, 127, 70), p("SUS", "Sustain", 0, 127, 80),
        p("REL", "Release", 0, 127, 44), p("FATK", "Env Attack", 0, 127, 0),
        p("FLT", "Filter Type", 0, 2, 0, EN, ["LP", "BP", "HP"]), p("KTRK", "Key Track", 0, 127, 64, PCT),
        p("DRV", "Drive", 0, 127, 0),
        p("OSC2", "Osc2 Wave", 0, 3, 0, EN, ["SAW", "PULS", "TRI", "SINE"]),
        p("INTV", "Osc2 Pitch", 0, 48, 24, SEMI), p("MIX", "Osc Mix", 0, 127, 64, BI),
        p("LFO", "LFO Rate", 0, 127, 94), p("VIB", "Vibrato", 0, 127, 0), p("LSHP", "LFO>Shape", 0, 127, 0),
        p("LCUT", "LFO>Cutoff", 0, 127, 0), p("GLID", "Glide", 0, 127, 64)]
TRACKS = [KICK, SNARE, CLAP, CHH, OHH, PERC, BASS, LEAD]

MIX_DEF = dict(LVL=[104, 96, 92, 84, 84, 86, 98, 88], PAN=[64, 64, 64, 58, 70, 74, 64, 64],
               DLY=[0, 0, 12, 0, 10, 28, 0, 48], REV=[0, 28, 44, 10, 24, 30, 0, 44],
               DUCK=[0, 0, 20, 0, 36, 36, 110, 64])
MIX = [p("LVL", "Level", 0, 127, 100), p("PAN", "Pan", 0, 127, 64, BI), p("DLY", "Delay Send", 0, 127, 0, PCT),
       p("REV", "Reverb Send", 0, 127, 0, PCT), p("DUCK", "Duck Amount", 0, 127, 0, PCT)]

DELAY_TIMES = ["1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4"]
SCALES = ["CHRO", "MAJ", "MIN", "DOR", "PHRY", "MIXO", "MPEN", "JPEN", "BLUE", "HMIN"]
ROOTS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
GLOBALS = [p("BPM", "Tempo", 60, 200, 124, BPM), p("SWNG", "Swing", 0, 50, 0, PCT),
           p("VOL", "Master Vol", 0, 127, 100), p("DSRC", "Duck Source", 0, 2, 1, EN, ["OFF", "KICK", "BEAT"]),
           p("DDEP", "Duck Depth", 0, 127, 100, PCT), p("DHLD", "Duck Hold", 0, 127, 49),
           p("DREL", "Duck Release", 0, 127, 78), p("DCRV", "Duck Curve", 0, 127, 64, BI),
           p("DFX", "Duck FX", 0, 127, 64, PCT), p("DTIM", "Delay Time", 0, 8, 7, EN, DELAY_TIMES),
           p("DFB", "Delay Fdbk", 0, 127, 58, PCT), p("DTON", "Delay Tone", 0, 127, 64, BI),
           p("DWID", "Delay Width", 0, 127, 100, PCT), p("RSIZ", "Reverb Size", 0, 127, 60),
           p("RDMP", "Reverb Damp", 0, 127, 48), p("FILT", "DJ Filter", 0, 127, 64, BI),
           p("FRES", "Filter Reso", 0, 127, 36), p("DRIV", "Drive", 0, 127, 0),
           p("ROOT", "Scale Root", 0, 11, 0, EN, ROOTS), p("SCAL", "Scale", 0, 9, 2, EN, SCALES)]
G = {g["name"]: i for i, g in enumerate(GLOBALS)}

# FX view pages: global ids in column order (z x c v b n m ,).
FX_PAGES = [("DUCK", ["DSRC", "DDEP", "DHLD", "DREL", "DCRV", "DFX"]),
            ("SPACE", ["DTIM", "DFB", "DTON", "RSIZ", "RDMP", "DWID"]),
            ("MASTER", ["BPM", "SWNG", "VOL", "FILT", "FRES", "DRIV", "ROOT", "SCAL"])]


def centre(info):
    return (info["min"] + info["max"] + 1) // 2


def fmt(info, v):
    """Display text for value v, per UX.md 'Value display' (max 5 chars)."""
    f = info["fmt"]
    if info["name"] == "SWNG":                       # special case: 0..50 -> 50..75 %
        return "%d%%" % (50 + v // 2)
    if info["name"] == "FILT":                       # special case: DJ filter
        return "OFF" if v == 64 else ("LP%d" % (64 - v) if v < 64 else "HP%d" % (v - 64))
    if f in (N, BPM):
        return "%d" % v
    if f == PCT:
        return "%d%%" % int(round((v - info["min"]) * 100.0 / (info["max"] - info["min"])))
    if f in (BI, SEMI):
        d = v - centre(info) if f == BI else v - (info["min"] + info["max"]) // 2
        return "0" if d == 0 else "%+d" % d
    if f == EN:
        return info["enum"][v - info["min"]]
    if f == ONOFF:
        return "ON" if v else "OFF"
    return "%d" % v


def frac(info, v):
    return (v - info["min"]) / float(info["max"] - info["min"])


def kind(info):
    if info["fmt"] == EN or (info["max"] - info["min"]) <= 8:
        return "enum"
    if info["fmt"] in (BI, SEMI):
        return "bi"
    return "uni"
