// HELP (UX.md 10.10): hold / = key map of the current view (or of the held
// modifier's layer), tap / = explain mode. All tables are const (flash).
#include <cstring>

#include "font.h"
#include "gfx.h"
#include "keys.h"
#include "ui.h"
#include "ui_tables.h"

namespace pg {
namespace ui {

namespace {

// ---- key grids: 56 two-character labels + 56 group codes per table ----------------
// Groups: s step, v view, a value, c filter, n note (selected track mid), m modifier,
// p pad (track colour of its column), e esc, w white, r raised/text, g raised/white,
// l lead chord, R rec, G play, f field, t type, o off.
struct KeyTable {
  const char* name;
  const char* labels;   // 4 rows x 14 keys x 2 chars
  const char* groups;   // 56 chars
};

// Generated (labels padded to 2 chars, one literal per key so hex escapes stay safe).
constexpr KeyTable kTables[] = {
    {"GROOVE",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "SN" "MX" "- " "+ " "ER" "BP" "9 " "10" "11" "12" "13" "14" "15" "16" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "CD" "\x84 " "PG" "\x82 " "CT" "PT" "RL" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "\x85 " "? " "\x80 ",
     "essssssssvvaarwssssssssvvccvmmpppppppplngRmmmnnnnnnnnnwG"},
    {"STEP EDIT",
     "ES" "CP" "CP" "CP" "CP" "CP" "CP" "CP" "CP" "SN" "MX" "F-" "F+" "NL" "  " "CP" "CP" "CP" "CP" "CP" "CP" "CP" "CP" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "VL" "GT" "PB" "RT" "AC" "SL" "CH" "LK" "CH" "\x84 " "PG" "  " "CT" "PT" "RL" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "\x85 " "? " "\x80 ",
     "essssssssvvaarossssssssvvccvmmfffffffflngommmnnnnnnnnnwG"},
    {"STEP WRITE",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "SN" "MX" "- " "+ " "RS" "BP" "9 " "10" "11" "12" "13" "14" "15" "16" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "CD" "\x84 " "PG" "\x82 " "CT" "PT" "RL" "W1" "W2" "W3" "W4" "W5" "W6" "W7" "W8" "\x85 " "? " "\x80 ",
     "essssssssvvaarwssssssssvvccvmmpppppppplngRmmmnnnnnnnnnwG"},
    {"SOUND",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "NP" "MX" "- " "+ " "RL" "BP" "9 " "10" "11" "12" "13" "14" "15" "16" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "  " "\x84 " "PG" "\x82 " "CT" "PT" "RL" "P1" "P2" "P3" "P4" "P5" "P6" "P7" "P8" "\x85 " "? " "\x80 ",
     "essssssssvvaarwssssssssvvccvmmpppppppporgRmmmffffffffrwG"},
    {"MIX",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "SN" "NR" "- " "+ " "RL" "BP" "9 " "10" "11" "12" "13" "14" "15" "16" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "  " "\x84 " "PG" "\x82 " "CT" "PT" "RL" "LV" "DK" "DL" "RV" "PN" "  " "  " "  " "\x85 " "? " "\x80 ",
     "essssssssvvaarwssssssssvvccvmmpppppppporgRmmmfffffooorwG"},
    {"FX",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "SN" "MX" "- " "+ " "  " "BP" "9 " "10" "11" "12" "13" "14" "15" "16" "NT" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "  " "\x84 " "  " "\x82 " "CT" "PT" "RL" "C1" "C2" "C3" "C4" "C5" "C6" "C7" "C8" "\x85 " "? " "\x80 ",
     "essssssssvvaaowssssssssvvccvmmpppppppporoRmmmffffffffrwG"},
    {"SONG",
     "ES" "01" "02" "03" "04" "05" "06" "07" "08" "SN" "MX" "R-" "R+" "DS" "BP" "09" "10" "11" "12" "13" "14" "15" "16" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "  " "\x84 " "  " "CH" "CT" "PT" "RL" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "\x85 " "? " "\x80 ",
     "effffffffvvaarwffffffffvvccvmmpppppppporoGmmmnnnnnnnnrwG"},
    {"PROJECT",
     "ES" "  " "  " "  " "  " "  " "  " "  " "  " "SN" "MX" "  " "  " "DF" "BP" "  " "  " "  " "  " "  " "  " "  " "  " "FX" "SG" "LP" "HP" "LS" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "  " "\x84 " "  " "OK" "CT" "PT" "RL" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "\x85 " "? " "\x80 ",
     "eoooooooovvooRwoooooooovvccvmmpppppppporoGmmmnnnnnnnnrwG"},
    {"NAME ENTRY",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "9 " "0 " "- " "  " "BS" "IS" "Q " "W " "E " "R " "T " "Y " "U " "I " "O " "P " "  " "  " "  " "  " "  " "A " "S " "D " "F " "G " "H " "J " "K " "L " "  " "  " "OK" "  " "  " "  " "Z " "X " "C " "V " "B " "N " "M " "  " "  " "  " "_ ",
     "etttttttttttorvttttttttttoooootttttttttooGoootttttttooot"},
    {"INSTRUMENT",
     "ES" "1 " "2 " "3 " "4 " "5 " "6 " "7 " "8 " "NI" "MX" "- " "+ " "RL" "BP" "9 " "10" "11" "12" "13" "14" "15" "16" "FX" "SG" "LP" "HP" "PJ" "MU" "SH" "KK" "SN" "CP" "CH" "OH" "PC" "BS" "LD" "  " "\x84 " "PG" "\x82 " "CT" "PT" "RL" "1-" "1+" "2-" "2+" "3-" "3+" "4-" "4+" "\x85 " "? " "\x80 ",
     "essssssssvvaarwssssssssvvccvmmpppppppporgRmmmaaaaaaaarwG"},
    {"FN LAYER",
     "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "FN" "  " "MU" "MU" "MU" "MU" "MU" "MU" "MU" "MU" "  " "  " "  " "  " "  " "  " "  " "SO" "SO" "SO" "SO" "SO" "SO" "SO" "SO" "  " "  " "  ",
     "oooooooooooooooooooooooooooomoppppppppooooooonnnnnnnnooo"},
    {"SHIFT LAYER",
     "  " "LN" "LN" "LN" "LN" "LN" "LN" "LN" "LN" "  " "  " "x8" "x8" "  " "  " "LN" "LN" "LN" "LN" "LN" "LN" "LN" "LN" "  " "  " "BC" "BC" "  " "  " "SH" "SL" "SL" "SL" "SL" "SL" "SL" "SL" "SL" "CB" "  " "AP" "  " "  " "  " "  " "AC" "AC" "AC" "AC" "AC" "AC" "AC" "AC" "  " "  " "RS",
     "ossssssssooaaoossssssssooccoompppppppprogoooonnnnnnnnooG"},
    {"CTRL LAYER",
     "  " "TL" "TL" "TL" "TL" "TL" "TL" "TL" "TL" "  " "  " "  " "  " "  " "  " "TL" "TL" "TL" "TL" "TL" "TL" "TL" "TL" "  " "  " "  " "  " "  " "  " "  " "  " "SV" "  " "  " "  " "  " "  " "  " "  " "12" "  " "  " "CT" "  " "  " "UN" "CU" "CO" "PA" "  " "NW" "  " "  " "12" "  " "  ",
     "ossssssssoooooossssssssoooooooowoooooooroomooaaaaoaooroo"},
    {"OPT LAYER",
     "  " "01" "02" "03" "04" "05" "06" "07" "08" "  " "  " "  " "  " "  " "  " "09" "10" "11" "12" "13" "14" "15" "16" "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "OP" "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  ",
     "ossssssssoooooossssssssoooooooooooooooooooomoooooooooooo"},
    {"ALT LAYER",
     "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "RO" "RO" "RO" "RO" "RO" "RO" "RO" "RO" "  " "\x84 " "  " "  " "  " "  " "AL" "  " "  " "  " "  " "  " "  " "  " "  " "\x85 " "  " "  ",
     "oooooooooooooooooooooooooooooopppppppporoooomooooooooroo"},
    {"TAB LAYER",
     "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "B-" "B+" "  " "TB" "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "S+" "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "  " "S-" "  " "  ",
     "oooooooooooaaomooooooooooooooooooooooooroooooooooooooroo"},

    {"SHIFT SONG",
     "  " "IN" "IN" "IN" "IN" "IN" "IN" "IN" "IN" "  " "  " "x8" "x8" "  " "  " "IN" "IN" "IN" "IN" "IN" "IN" "IN" "IN" "  " "  " "BC" "BC" "  " "  " "SH" "SL" "SL" "SL" "SL" "SL" "SL" "SL" "SL" "  " "  " "  " "  " "  " "  " "  " "AC" "AC" "AC" "AC" "AC" "AC" "AC" "AC" "  " "  " "RS",
     "ossssssssooaaoossssssssooccoomppppppppooooooonnnnnnnnooG"},
    {"SHIFT INST",
     "  " "LN" "LN" "LN" "LN" "LN" "LN" "LN" "LN" "  " "  " "x8" "x8" "  " "  " "LN" "LN" "LN" "LN" "LN" "LN" "LN" "LN" "  " "  " "BC" "BC" "  " "  " "SH" "SL" "SL" "SL" "SL" "SL" "SL" "SL" "SL" "  " "  " "AP" "  " "  " "  " "  " "1-" "1+" "2-" "2+" "3-" "3+" "4-" "4+" "  " "  " "RS",
     "ossssssssooaaoossssssssooccoompppppppprogoooaaaaaaaaaooG"},
};
constexpr bool tablesOk() {
  for (const KeyTable& t : kTables) {
    int a = 0, b = 0;
    while (t.labels[a]) ++a;
    while (t.groups[b]) ++b;
    if (a != 112 || b != 56) return false;
  }
  return true;
}
static_assert(tablesOk(), "help tables must have 56 keys");

// Header: "KEYS <name>" (the name from x=31), the modifier hint right-aligned at 236 (6 px a character).
constexpr const char* kHelpHint = "FN/SFT/CTL/OPT/ALT/TAB";
constexpr int slen(const char* s) { return *s ? 1 + slen(s + 1) : 0; }
constexpr bool headerFits() {
  for (const KeyTable& t : kTables)
    if (31 + 6 * slen(t.name) + 6 > 236 - 6 * slen(kHelpHint)) return false;   // a space between
  return true;
}
static_assert(headerFits(), "help header: the table name runs into the modifier hint");

// (the layers start at kTFn: explainText() relies on that order)
enum TableId { kTGroove, kTStep, kTWrite, kTSound, kTMix, kTFx, kTSong, kTProject, kTText, kTInst,
               kTFn, kTShift, kTCtrl, kTOpt, kTAlt, kTTab, kTShiftSong, kTShiftInst };

// Legend colour switches inside strings
#define UP_ICON_TXT "\x84\x85"
#define CW "\x01"   // WHITE
#define CD "\x02"   // DIM
#define CT "\x03"   // TEXT
#define CA "\x04"   // AMBER
#define CC "\x05"   // CYAN
#define CK "\x06"   // kick red
#define CN "\x07"   // note colour (selected track mid)
#define CG "\x08"   // GREEN
const char* const kLegends[][5] = {
    {CW "1-16" CD " STEP: TAP=ON/OFF HOLD=EDIT",
     CK "KK-LD" CD " PADS " CN "1-8" CD " NOTES " CA "-+" CD " VALUE " CC "LP HP" CD " DJ",
     CT "SN MX FX SG PJ" CD " VIEWS " CD "ES" CD " HOME " CW "BP" CD " TEMPO",
     CT "MU" CD "+PAD MUTE " CT "PT" CD "+STEP PATTERN " CT "ER" CD " ERASE",
     CA "TAP / : EVERY KEY EXPLAINS ITSELF"},
    {CW "VL-LK" CD " PICK THE FIELD -/= CHANGES",
     CN "1-8" CD " SET NOTE (DRUMS: VELOCITY)",
     CW "CP" CD " TAP A STEP: COPY THE HELD STEP",
     CN UP_ICON_TXT CD " SEMITONE, CTRL OCTAVE " CT "NL" CD " NO LOCK",
     CA "HOLD STEP + 9/0: LOCK ANY PARAM"},
    {CN "W1-W8" CD " WRITE NOTE, CURSOR MOVES ON",
     CW "SHIFT" CD "+NOTE ACCENT. HOLD PREV = SLIDE",
     CT "RS" CD " REST: CLEAR STEP AND MOVE ON",
     CW "1-16" CD " MOVE THE CURSOR",
     CG "SPACE" CD " PLAYS = LIVE RECORD"},
    {CW "P1-P8" CD " FOCUS A PARAMETER COLUMN",
     CA "-+" CD " CHANGE IT. " CT "NP" CD "/" CT UP_ICON_TXT CD " PAGES",
     CW "HOLD STEP" CD " + " CA "-+" CD " = LOCK ON THAT STEP",
     CT "RL" CD " REMOVE THE HELD STEP'S LOCK",
     CA "FOCUS STAYS ON -/= IN GROOVE"},
    {CW "LV DK DL RV PN" CD " PICK THE MIX ROW",
     CK "KK-LD" CD " SELECT A STRIP " CA "-+" CD " FADER",
     CW "HOLD STEP" CD " + " CA "-+" CD " = LOCK ON THAT STEP",
     CT "PAN" CD " IS KEPT, THE OUTPUT IS MONO",
     CT "NR" CD " / " CT UP_ICON_TXT CD " NEXT / PREVIOUS ROW"},
    {CW "C1-C8" CD " FOCUS A GLOBAL OF THIS TAB",
     CK "KK-LD" CD " DUCK TAB: THAT TRACK'S DUCK",
     CT "NT" CD " / " CT UP_ICON_TXT CD " DUCK > SPACE > MASTER",
     CA "-+" CD " CHANGE IT FROM ANY VIEW",
     CD "GLOBALS CAN'T BE LOCKED"},
    {CW "01-16" CD " PUT PATTERN (SHIFT: INSERT)",
     CA "R- R+" CD " REPEATS OF THE CURSOR SLOT",
     CT UP_ICON_TXT CD " MOVE CURSOR  " CT "DS" CD " DELETE SLOT",
     CG "CH" CD " CHAIN ON/OFF FROM THE CURSOR",
     CD "OPT+STEP STILL QUEUES A PATTERN"},
    {CT UP_ICON_TXT CD " PICK A ROW  " CG "OK" CD " LOAD / SAVE",
     CT "LS" CD " LOAD <> SAVE AS TAB",
     CT "DF" CD " DELETE THE FILE (ASKS FIRST)",
     CW "CTRL+S" CD " SAVE   " CW "CTRL+N" CD " NEW",
     CD "LOADING STOPS THE TRANSPORT"},
    {CT "A-Z 0-9 - _" CD " TYPE THE NAME",
     CT "BS" CD " BACKSPACE  " CT "IS" CD " INT / SD",
     CG "OK" CD " SAVE   " CT "ES" CD " CANCEL",
     CD "SPACE TYPES _ HERE",
     CA "NAMES: UP TO 8 CHARACTERS"},
    {CA "1-..4+" CD " MACRO DOWN/UP. HOLD: SWEEP",
     CW "BOTH KEYS" CD "=DEFAULT  " CW "G0" CD "+TILT: 2 MACROS",
     CT UP_ICON_TXT CD " " CT "NI" CD " INSTRUMENT. CTRL+Z GOES BACK",
     CW "HOLD STEP" CD " + MACRO KEY = NOTE'S VALUE",
     CW "DEL" CD "+KEY CLEARS MOTION " CA "-+" CD " LAST"},
    {CW "FN" CD "+PAD: MUTE ON/OFF",
     CW "FN" CD "+NOTE KEY: SOLO THAT TRACK",
     CD "SAME KEY AGAIN: RESTORE MUTES", "", ""},
    {CW "LN" CD " PATTERN LENGTH  " CW "SL" CD " SILENT SELECT",
     CW "AC" CD " ACCENT  " CA "x8" CD " COARSE VALUE",
     CC "BC" CD " BASS SWEEP  " CT "AP" CD " ADD PAGE",
     CT "CB" CD " CHORD BACK  " CG "RS" CD " RESTART", ""},
    {CA "UN" CD " UNDO/REDO " CA "CU CO PA" CD " CUT COPY PASTE",
     CD "+SHIFT: THE WHOLE PATTERN",
     CW "SV" CD " SAVE  " CA "NW" CD " NEW PROJECT",
     CW "TL" CD " +SHIFT: SELECTED TRACK LENGTH",
     CT "12" CD " STEP HELD: NOTE OCTAVE UP / DOWN"},
    {CW "01-16" CD " QUEUE THAT PATTERN",
     CW "SHIFT" CD "+STEP: COPY THIS PATTERN THERE",
     CD "AND QUEUE IT. CTRL+Z UNDOES", "", ""},
    {CK "RO" CD " ROLL THE TRACK WHILE HELD",
     CT UP_ICON_TXT CD " ROLL RATE 1/8 1/16 1/16T 1/32", "", "", ""},
    {CA "B- B+" CD " TEMPO (SHIFT: 10 BPM)",
     CT "S+ S-" CD " SWING",
     CD "TAP TAB 3X: TAP TEMPO", "", ""},
    {CW "IN" CD " INSERT PATTERN BEFORE THE CURSOR",
     CA "x8" CD " REPEATS BY 8  " CW "SL" CD " SILENT SELECT",
     CC "BC" CD " BASS SWEEP  " CG "RS" CD " RESTART",
     CW "AC" CD " ACCENT", ""},
    {CA "1-..4+" CD " MACRO, COARSE: 16 OF 255",
     CW "LN" CD " PATTERN LENGTH  " CW "SL" CD " SILENT SELECT",
     CC "BC" CD " BASS SWEEP  " CT "AP" CD " ADD PAGE",
     CG "RS" CD " RESTART", ""},
};
#undef CW
#undef CD
#undef CT
#undef CA
#undef CC
#undef CK
#undef CN
#undef CG
#undef UP_ICON_TXT

char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}

// Draws a legend line with colour switches; returns nothing.
void legendLine(Gfx& g, int x, int y, const char* s, uint8_t noteCol) {
  char seg[48];
  uint8_t col = DIM;
  int n = 0;
  auto flush = [&]() {
    seg[n] = 0;
    if (n) x = g.text(x, y, seg, col);
    n = 0;
  };
  for (; *s; ++s) {
    const uint8_t c = uint8_t(*s);
    if (c >= 1 && c <= 8) {
      flush();
      static const uint8_t kMap[9] = {0, WHITE, DIM, TEXT, AMBER, CYAN, tc(0), 0, GREEN};
      col = c == 7 ? noteCol : kMap[c];
      continue;
    }
    if (n < 46) seg[n++] = char(c);
  }
  flush();
}

}  // namespace

bool Ui::helpVisible() const {
  return explain_ || (isHeld(kKSlash) && rec_[kKSlash].act == kActHelp && int32_t(now_ - (helpDownMs_ + 400)) >= 0);
}

void Ui::helpKeyLabel(int k, char* out) const {
  const char* l = kTables[helpTable()].labels;
  out[0] = out[1] = out[2] = 0;
  if (k < 0 || k >= kNumKeys) return;
  if (l[2 * k] != ' ') out[0] = l[2 * k];
  if (l[2 * k + 1] != ' ') out[1] = l[2 * k + 1];
}

int Ui::helpTable(bool layers) const {
  if (dialog_ == Dialog::Name) return kTText;
  // the order in which keyDown() lets the modifiers win
  if (layers && isHeld(kKFn)) return kTFn;
  if (layers && isHeld(kKOpt)) return kTOpt;
  if (layers && isHeld(kKAlt)) return kTAlt;
  if (layers && isHeld(kKCtrl)) return kTCtrl;
  if (layers && isHeld(kKTab)) return kTTab;
  if (layers && isHeld(kKShift))
    return view_ == View::Song ? kTShiftSong : (view_ == View::Sound && instrumentPage()) ? kTShiftInst : kTShift;
  if (heldStep_ >= 0 && view_ == View::Groove) return kTStep;
  switch (view_) {
    case View::Sound: return instrumentPage() ? kTInst : kTSound;
    case View::Mix: return kTMix;
    case View::Fx: return kTFx;
    case View::Song: return kTSong;
    case View::Project: return kTProject;
    default: return stepWriteActive() ? kTWrite : kTGroove;
  }
}

static char* catStep(char* o, int n) {   // "07", "12"
  if (n < 10) *o++ = '0';
  return o + itoa10(n, o);
}

// A key pressed with a modifier held (MANUAL 4.9): the combo's text. False when
// the modifier leaves the key alone; it then explains itself as without it.
static bool explainLayer(const Ui& ui, int table, int k, char* title, const char** l1, const char** l2) {
  const int step = stepKeyIndex(k), pad = padKeyIndex(k), note = noteKeyIndex(k);
  const View v = ui.view();
  const bool perf = v == View::Groove;   // notes play here (SOUND, MIX, FX: they focus columns)
  if (isModifierKey(k) || k == kKTab) return false;
  switch (table) {
    case kTFn:
      if (pad >= 0) {
        cat(cat(title, "FN+PAD: MUTE "), kTrackNames[pad]);
        *l1 = "MUTES / UNMUTES THE TRACK AT ONCE.";
        *l2 = "ENDS A SOLO, KEEPS THE MUTES.";
        return true;
      }
      if (note >= 0) {
        cat(cat(title, "FN+NOTE: SOLO "), kTrackNames[note]);
        *l1 = "MUTES EVERY OTHER TRACK.";
        *l2 = "SAME KEY AGAIN: MUTES COME BACK.";
        return true;
      }
      break;
    case kTOpt:
      if (step >= 0) {
        catStep(cat(title, "OPT+STEP: QUEUE P"), step + 1);
        *l1 = "STARTS WHEN THIS PATTERN ENDS.";
        *l2 = "+SHIFT: COPY THIS PATTERN THERE.";
        return true;
      }
      break;
    case kTAlt:
      if (pad >= 0) {
        cat(cat(title, "ALT+PAD: ROLL "), kTrackNames[pad]);
        *l1 = "RETRIGGERS THE TRACK WHILE HELD.";
        *l2 = "ON THE BEAT GRID, NOT RECORDED.";
        return true;
      }
      if (k == kKSemicolon || k == kKDot) {
        cat(title, k == kKSemicolon ? "ALT+UP: ROLL FASTER" : "ALT+DOWN: ROLL SLOWER");
        *l1 = "ROLL RATE 1/8 1/16 1/16T 1/32.";
        return true;
      }
      break;
    case kTCtrl: {
      struct C { uint8_t k; const char* t; const char* a; const char* b; };
      static const C kCtrl[] = {
          {kKNote0, "CTRL+Z: UNDO", "UNDOES THE LAST EDIT.", "AGAIN: REDO."},
          {kKNote0 + 1, "CTRL+X: CUT TRACK", "CUTS THE SELECTED TRACK.", "+SHIFT: THE WHOLE PATTERN."},
          {kKNote0 + 2, "CTRL+C: COPY TRACK", "COPIES THE SELECTED TRACK.", "+SHIFT: THE WHOLE PATTERN."},
          {kKNote0 + 3, "CTRL+V: PASTE", "PASTES THE CLIP: A TRACK ONTO THE", "SELECTED TRACK, OR A PATTERN."},
          {kKPad0 + 1, "CTRL+S: SAVE", "SAVES TO THE OPEN FILE. A DEMO OR", "UNNAMED PROJECT OPENS SAVE AS."},
          {kKNote0 + 5, "CTRL+N: NEW PROJECT", "ASKS FIRST IF THERE ARE", "UNSAVED CHANGES."},
          {kKSemicolon, "CTRL+UP: NOTE +12", "WITH A STEP HELD: ITS NOTE", "AN OCTAVE UP."},
          {kKDot, "CTRL+DOWN: NOTE -12", "WITH A STEP HELD: ITS NOTE", "AN OCTAVE DOWN."},
      };
      for (const C& c : kCtrl)
        if (c.k == k) {
          cat(title, c.t);
          *l1 = c.a;
          *l2 = c.b;
          return true;
        }
      if (step >= 0 && v != View::Song && v != View::Project) {
        itoa10(ui.shownPage() * 16 + step + 1, cat(title, "CTRL+SHIFT+STEP: LENGTH "));
        *l1 = "LENGTH OF THE SELECTED TRACK";
        *l2 = "ONLY (POLYMETER). UNDOABLE.";
        return true;
      }
      break;
    }
    case kTTab:
      if (k == kKMinus || k == kKEquals) {
        cat(title, k == kKMinus ? "TAB+-: TEMPO -1 BPM" : "TAB+=: TEMPO +1 BPM");
        *l1 = "SHIFT: 10 BPM.";
        return true;
      }
      if (k == kKSemicolon || k == kKDot) {
        cat(title, k == kKSemicolon ? "TAB+UP: SWING UP" : "TAB+DOWN: SWING DOWN");
        return true;
      }
      return false;   // TAB leaves every other key alone
    case kTShiftInst:
      if (note >= 0) {
        const pg::macro::MacroSet* s = ui.instrumentOf(ui.selTrack());
        if (!s || note / 2 >= s->numMacros) {
          cat(title, "SHIFT+MACRO KEY: NOT USED");
          *l1 = s ? "THIS INSTRUMENT HAS NO SUCH MACRO." : "PICK AN INSTRUMENT FIRST: . OR 9.";
          return true;
        }
        char* o = cat(title, "SHIFT+");
        *o++ = char(kNoteKeys[note]);
        *o = 0;
        cat(cat(cat(o, ": "), s->macros[note / 2].name), (note & 1) ? " +16" : " -16");
        *l1 = "COARSE: 16 OF 255 PER TAP OR REPEAT.";
        *l2 = "A STEP HELD: ITS VALUE, 4 LEVELS.";
        return true;
      }
      [[fallthrough]];
    case kTShift:
    case kTShiftSong:
      if (step >= 0 && v == View::Song) {
        catStep(cat(title, "SHIFT+STEP: INSERT P"), step + 1);
        *l1 = "INSERTS THE PATTERN BEFORE THE";
        *l2 = "CURSOR SLOT.";
        return true;
      }
      if (step >= 0 && v != View::Project) {
        itoa10(ui.shownPage() * 16 + step + 1, cat(title, "SHIFT+STEP: LENGTH "));
        *l1 = "PATTERN LENGTH = THIS STEP,";
        *l2 = "EVERY TRACK. UNDOABLE.";
        return true;
      }
      if (note >= 0 && perf) {
        itoa10(note + 1, cat(title, "SHIFT+NOTE "));
        cat(title + std::strlen(title), ": ACCENT");
        *l1 = ui.stepWriteActive() ? "WRITES THE NOTE ACCENTED." : "PLAYS THE NOTE ACCENTED.";
        *l2 = "LIVE RECORD: RECORDS THE ACCENT.";
        return true;
      }
      return false;   // the other SHIFT combos are in the key's own text
    default: return false;
  }
  // FN, OPT, ALT, CTRL take every key but the global ones (keyDown)
  switch (k) {
    case kKBacktick: case kKSpace: case kK9: case kK0: case kKO: case kKP: case kKBackslash: case kKSlash:
    case kKLBracket: case kKRBracket: case kKMinus: case kKEquals: case kKG0:
      return false;
    default: break;
  }
  cat(title, "NOTHING WITH ");
  cat(title + std::strlen(title), table == kTFn ? "FN" : table == kTOpt ? "OPT" : table == kTAlt ? "ALT" : "CTRL");
  *l1 = "RELEASE THE MODIFIER TO SEE WHAT";
  *l2 = "THIS KEY DOES ON ITS OWN.";
  return true;
}

// Explain texts: title (<= 30), two lines (<= 38). Dynamic for steps, pads,
// note/column keys; a per-view switch for the keys a view reuses.
static void explain(const Ui& ui, int table, int k, char* title, const char** l1, const char** l2) {
  *l1 = *l2 = "";
  const int step = stepKeyIndex(k), pad = padKeyIndex(k), note = noteKeyIndex(k);
  char* o = title;
  if (table == kTProject) {
    switch (k) {
      case kKSemicolon: case kKDot: cat(title, "LIST UP / DOWN"); *l1 = "MOVES THE CURSOR IN THE FILE LIST."; return;
      case kKEnter: cat(title, "LOAD / SAVE"); *l1 = "LOAD TAB: LOADS THE ROW (ASKS FIRST"; *l2 = "IF UNSAVED). SAVE TAB: SAVES THERE."; return;
      case kKDel: cat(title, "DELETE FILE"); *l1 = "DELETES THE FILE UNDER THE CURSOR."; *l2 = "ASKS FIRST. DEMOS CAN'T BE DELETED."; return;
      case kKBackslash: cat(title, "LOAD / SAVE AS TAB"); *l1 = "SWITCHES BETWEEN THE TWO LISTS."; return;
      default: if (step >= 0 || k == kKMinus || k == kKEquals || k == kKQuote || k == kKL) { cat(title, "NOT USED HERE"); return; }
    }
  }
  if (table == kTSong) {
    if (step >= 0) {
      cat(cat(title, "PUT PATTERN "), step < 9 ? "0" : "");
      itoa10(step + 1, title + std::strlen(title));
      *l1 = "WRITES IT INTO THE CURSOR SLOT,";
      *l2 = "ON END APPENDS. SHIFT: INSERT.";
      return;
    }
    switch (k) {
      case kKMinus: case kKEquals: cat(title, "REPEATS"); *l1 = "HOW OFTEN THE CURSOR SLOT PLAYS,"; *l2 = "1 TO 64."; return;
      case kKDel: cat(title, "REMOVE SLOT"); *l1 = "DELETES THE SLOT UNDER THE CURSOR."; return;
      case kKSemicolon: case kKDot: cat(title, "CURSOR UP / DOWN"); *l1 = "MOVES THROUGH THE CHAIN."; return;
      case kKEnter: cat(title, "CHAIN ON / OFF"); *l1 = "SONG MODE: PLAYS THE CHAIN FROM"; *l2 = "THE CURSOR SLOT."; return;
      default: break;
    }
  }
  if (table == kTInst) {
    const pg::macro::MacroSet* s = ui.instrumentOf(ui.selTrack());
    if (note >= 0) {
      char* o = title;
      *o++ = char(kNoteKeys[note]);
      *o = 0;
      if (!s) {
        cat(o, ": NO INSTRUMENT YET");
        *l1 = ". OR 9 PICKS ONE; ITS MACROS ARE THE";
        *l2 = "KEYS Z TO , (A PAIR EACH).";
        return;
      }
      const int m = note / 2;
      if (m >= s->numMacros) {
        cat(o, ": NOT USED");
        *l1 = "THIS INSTRUMENT HAS FEWER MACROS.";
        return;
      }
      cat(cat(cat(o, ": "), s->macros[m].name), (note & 1) ? " UP" : " DOWN");
      if (ui.heldStep() >= 0) {
        *l1 = "THE HELD NOTE'S OWN VALUE: THE MACRO";
        *l2 = "GLIDES TO IT FROM NOTE TO NOTE.";
      } else if (ui.recOn() && ui.playing()) {
        *l1 = "RECORDING: MOVED, IT GOES INTO EVERY";
        *l2 = "NOTE IT PASSES (RED NAME: LIVE).";
      } else if ((ui.motionMask(ui.selTrack()) >> m) & 1u) {
        *l1 = "\x88: ITS NOTES HAVE VALUES, IT FOLLOWS";
        *l2 = "THEM. HOLD STEP+KEY: A NOTE'S VALUE.";
      } else {
        *l1 = "TAP NUDGES, HOLD SWEEPS, SHIFT: COARSE.";
        *l2 = "BOTH: DEFAULT. STEP HELD: NOTE VALUE.";
      }
      return;
    }
    switch (k) {
      case kKSemicolon:
        cat(title, "PREVIOUS INSTRUMENT");
        *l1 = "STEPS THROUGH THIS TRACK'S LIST,";
        *l2 = ui.samplerOn(ui.selTrack()) ? "SHIFT: PREVIOUS SAMPLE SLOT." : "AUDIBLE AT ONCE. CTRL+Z: BACK.";
        return;
      case kKDot:
        cat(title, "NEXT INSTRUMENT");
        *l1 = "STEPS THROUGH THIS TRACK'S LIST,";
        *l2 = ui.samplerOn(ui.selTrack()) ? "SHIFT: NEXT SAMPLE SLOT." : "AUDIBLE AT ONCE. CTRL+Z: BACK.";
        return;
      case kK9:
        cat(title, "SOUND: NEXT INSTRUMENT");
        *l1 = "THE SELECTED TRACK'S INSTRUMENT.";
        *l2 = "AGAIN: THE NEXT ONE (; BACK).";
        return;
      case kKDel:
        cat(title, "CLEAR MOTION / VALUE");
        *l1 = "HELD + A MACRO KEY: CLEARS ITS MOTION";
        *l2 = "HERE. STEP HELD: THE NOTE'S VALUE.";
        return;
      case kKL:
        if (ui.samplerOn(ui.selTrack())) {
          cat(title, "HOLD: RECORD THE MIC");
          *l1 = "INTO THIS SLOT. SHIFT+L: RESAMPLE";
          *l2 = "WHAT YOU HEAR, IN WHOLE BARS.";
          return;
        }
        cat(title, ui.devMode() ? "DEV: PARAMETER PAGES" : "NOT USED HERE");
        if (ui.devMode()) *l1 = "DEVELOPER MODE: BARE PARAMETERS.";
        return;
      default: break;
    }
  }
  if (note >= 0 && (table == kTSound || table == kTMix || table == kTFx)) {
    const ValueRef r = table == kTSound ? ValueRef{false, uint8_t(ui.selTrack()), uint8_t(ui.soundPage(ui.selTrack()) * 8 + note)}
                       : table == kTMix  ? ValueRef{false, uint8_t(ui.selTrack()), kMixRowParam[note < kNumMixRows ? note : 0]}
                                         : ValueRef{true, 0, kFxPage[ui.fxTab()][note]};
    const bool none = (table == kTMix && note >= kNumMixRows) || (table == kTFx && kFxPage[ui.fxTab()][note] == 0xFF);
    const ParamInfo* info = none ? nullptr : (r.global ? Engine::globalParamInfo(r.index) : nullptr);
    if (!none && !r.global) info = ui.paramInfo(r.track, r.index);
    if (!info) { cat(title, "NOT USED HERE"); return; }
    char up[16];
    upperCopy(info->longName, up, sizeof(up));
    cat(cat(title, table == kTMix ? "ROW: " : "PARAM: "), up);
    *l1 = "PUTS IT ON -/= (THE STATUS CHIP).";
    *l2 = table == kTFx ? "GLOBALS CAN'T BE LOCKED ON STEPS." : "HOLD A STEP + -/=: LOCK IT THERE.";
    return;
  }
  if (step >= 0) {
    o = cat(title, "STEP ");
    itoa10(ui.shownPage() * 16 + step + 1, o);
    if (table == kTWrite) { *l1 = "MOVES THE STEP-WRITE CURSOR HERE."; *l2 = "HOLD: STEP EDIT AS USUAL."; return; }
    *l1 = "TAP: ON/OFF. HOLD: STEP EDIT.";
    *l2 = "SHIFT: PATTERN LENGTH. OPT: QUEUE.";
    return;
  }
  if (pad >= 0) {
    o = cat(title, "PAD ");
    *o++ = kPadKeys[pad];
    *o = 0;
    cat(cat(o, ": "), kTrackNames[pad]);
    *l1 = "PLAYS AND SELECTS THE TRACK.";
    *l2 = "SHIFT: SELECT WITHOUT SOUND.";
    return;
  }
  if (note >= 0) {
    o = cat(title, "NOTE ");
    o += itoa10(note + 1, o);
    o = cat(o, ": ");
    const int t = ui.selTrack();
    if (t >= kNumDrumTracks) {
      noteName(ui.noteForKey(note), o);
      o += std::strlen(o);
    } else {
      o = cat(o, "VEL ");
      o += itoa10(kVelRow[note], o);
    }
    cat(cat(o, " ON "), kTrackNames[t]);
    *l1 = table == kTWrite ? "WRITES THE NOTE AND MOVES ON." : "PLAYS THE SELECTED TRACK.";
    *l2 = "HOLD A STEP + KEY: SET ITS NOTE";
    return;
  }
  struct T { uint8_t k; const char* t; const char* a; const char* b; };
  static const T kTexts[] = {
      {kKBacktick, "HOME", "BACK TO GROOVE FROM ANYWHERE.", "CLOSES OVERLAYS AND ENDS REC."},
      {kK9, "SOUND VIEW", "THE SELECTED TRACK'S SOUND.", "AGAIN: NEXT PAGE."},
      {kK0, "MIX VIEW", "LEVEL, DUCK, SENDS AND PAN.", "AGAIN: NEXT ROW."},
      {kKMinus, "VALUE DOWN", "CHANGES THE VALUE IN THE CHIP.", "HOLD: SWEEP. SHIFT: 8X. +=: DEFAULT."},
      {kKEquals, "VALUE UP", "CHANGES THE VALUE IN THE CHIP.", "HOLD: SWEEP. SHIFT: 8X. +-: DEFAULT."},
      {kKDel, "ERASE", "HOLD WHILE PLAYING: WIPES THE", "TRACK AS THE PLAYHEAD PASSES."},
      {kKTab, "TEMPO", "HOLD: -/= BPM, ;/. SWING.", "TAP 3 TIMES: TAP TEMPO."},
      {kKO, "FX VIEW", "DUCKER, DELAY, REVERB, MASTER.", "AGAIN: NEXT TAB."},
      {kKP, "SONG VIEW", "CHAIN PATTERNS INTO A SONG.", ""},
      {kKLBracket, "DJ FILTER DOWN", "HOLD: LOW-PASS SWEEP. BOTH: OFF.", "SHIFT: BASS MACRO 1 (CLASSIC: CUT)."},
      {kKRBracket, "DJ FILTER UP", "HOLD: HIGH-PASS SWEEP. BOTH: OFF.", "SHIFT: BASS MACRO 1 (CLASSIC: CUT)."},
      {kKBackslash, "PROJECT VIEW", "LOAD, SAVE AND DELETE PROJECTS.", "CTRL+S SAVES FROM ANY VIEW."},
      {kKFn, "FN", "HOLD: MUTE STRIP. +PAD: MUTE.", "+NOTE KEY: SOLO."},
      {kKShift, "SHIFT", "+STEP LENGTH  +PAD SILENT SELECT", "+NOTE ACCENT  +-/= 8X"},
      {kKL, "CHORD MODE", "LEAD: AUTO, AUTO7 OR A FIXED", "CHORD. SHIFT: PREVIOUS."},
      {kKSemicolon, "OCTAVE UP", "NOTE KEYS PLAY AN OCTAVE HIGHER.", "STEP HELD: +1 SEMITONE, CTRL +12."},
      {kKQuote, "PAGE", "SHOWS THE NEXT 16 STEPS.", "SHIFT: ADD A PAGE (UP TO 64)."},
      {kKEnter, "RECORD", "PLAYING: LIVE RECORD. STOPPED:", "STEP-WRITE WITH THE NOTE KEYS."},
      {kKCtrl, "CTRL", "+Z UNDO +X CUT +C COPY +V PASTE", "+S SAVE +N NEW +SHIFT: PATTERN"},
      {kKOpt, "OPT", "+STEP: QUEUE PATTERN 1-16.", "+SHIFT+STEP: COPY THERE + QUEUE."},
      {kKAlt, "ALT", "+PAD: ROLL THE TRACK WHILE HELD.", "+;/.: ROLL RATE."},
      {kKDot, "OCTAVE DOWN", "NOTE KEYS PLAY AN OCTAVE LOWER.", "STEP HELD: -1 SEMITONE, CTRL -12."},
      {kKSlash, "HELP", "HOLD: KEY MAP. TAP: EXPLAIN MODE.", "/ OR ` LEAVES EXPLAIN MODE."},
      {kKSpace, "PLAY / STOP", "SHIFT: RESTART / CONTINUE.", ""},
      {kKG0, "G0: TILT / VOLUME / DIAG", "HOLD + TILT: THE TRACK'S MACROS MOVE.", "HOLD + -/=: VOLUME. 2 TAPS: DIAG."},
  };
  for (const T& t : kTexts)
    if (t.k == k) {
      cat(title, t.t);
      *l1 = t.a;
      *l2 = t.b;
      if (table == kTSound && (k == kKSemicolon || k == kKDot)) { *l1 = "PREVIOUS / NEXT SOUND PAGE."; *l2 = ""; }
      if (table == kTMix && (k == kKSemicolon || k == kKDot)) { *l1 = "PREVIOUS / NEXT MIX ROW."; *l2 = ""; }
      if (table == kTFx && (k == kKSemicolon || k == kKDot)) { *l1 = "PREVIOUS / NEXT FX TAB."; *l2 = ""; }
      return;
    }
  cat(title, "NOT USED");
}

void Ui::explainText(char* title, const char** l1, const char** l2) const {
  title[0] = 0;
  *l1 = *l2 = "";
  if (!explain_ || explainKey_ < 0) return;
  if (explainLayer(*this, explainTable_, explainKey_, title, l1, l2)) return;
  title[0] = 0;
  // the key on its own, in the view underneath the modifier's layer
  explain(*this, explainTable_ >= kTFn ? helpTable(false) : explainTable_, explainKey_, title, l1, l2);
}

void Ui::drawHelp(Framebuffer& fb) {
  Gfx g(fb);
  const int table = helpTable();
  const KeyTable& T = kTables[table];
  g.clear(BG);
  g.rect(0, 0, kScreenW, 11, WHITE);
  const int x = g.text(3, 2, "KEYS", BG);
  g.text(x + 4, 2, T.name, LINE);
  g.textR(236, 2, kHelpHint, LINE);
  const uint8_t noteCol = tc(sel_, kMid);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 14; ++c) {
      const int i = r * 14 + c;
      const int kx = 1 + c * 17, ky = 14 + r * 19;
      char lab[3] = {T.labels[2 * i], T.labels[2 * i + 1], 0};
      if (lab[1] == ' ') lab[1] = 0;
      uint8_t fill = RAISED, ink = WHITE;
      switch (T.groups[i]) {
        case 's': fill = RAISED; ink = WHITE; break;
        case 'v': fill = TEXT; ink = BG; break;
        case 'a': fill = AMBER; ink = BG; break;
        case 'c': fill = CYAN; ink = BG; break;
        case 'n': fill = noteCol; ink = BG; break;
        case 'm': fill = LINE; ink = TEXT; break;
        case 'p': fill = tc((c - 2) & 7); ink = BG; break;
        case 'e': fill = DIM; ink = BG; break;
        case 'w': fill = WHITE; ink = BG; break;
        case 'r': fill = RAISED; ink = TEXT; break;
        case 'g': fill = RAISED; ink = WHITE; break;
        case 'l': fill = tc(kTrackLead, kMid); ink = BG; break;
        case 'R': fill = RED; ink = WHITE; break;
        case 'G': fill = GREEN; ink = BG; break;
        case 'f': fill = WHITE; ink = BG; break;
        case 't': fill = PANEL; ink = TEXT; break;
        default:
          g.rframe(kx, ky, 16, 17, PANEL);
          continue;
      }
      // the arrow keys take the selected track's note colour in the performance tables
      if ((uint8_t(lab[0]) == 0x84 || uint8_t(lab[0]) == 0x85) && T.groups[i] == 'n') fill = noteCol;
      g.rrect(kx, ky, 16, 17, fill);
      if (lab[0] && lab[0] != ' ') g.textC(kx + 8, ky + 5, lab, ink);
    }
  if (explain_) {
    if (explainKey_ >= 0 && explainKey_ < kNumKeys) {   // (G0 has no cell in the grid)
      const int r = explainKey_ / 14, c = explainKey_ % 14;
      g.rframe(1 + c * 17 - 1, 14 + r * 19 - 1, 18, 19, WHITE);
    }
    g.rrect(2, 93, 236, 40, PANEL);
    g.rframe(2, 93, 236, 40, AMBER);
    g.textR(233, 98, "/ EXIT", AMBER);
    if (explainKey_ < 0) {
      g.text(7, 98, "EXPLAIN MODE", WHITE);
      g.text(7, 109, "PRESS ANY KEY TO SEE WHAT IT DOES.", TEXT);
      g.text(7, 118, "SPACE STILL PLAYS AND STOPS.", TEXT);
      return;
    }
    char cap[4];
    upperCopy(keyLegend(explainKey_), cap, sizeof(cap));
    const int xx = g.chip(7, 97, cap, WHITE);
    char title[40];
    const char *l1, *l2;
    explainText(title, &l1, &l2);
    title[30] = 0;
    g.text(xx + 5, 98, title, WHITE);
    g.text(7, 109, l1, TEXT);
    g.text(7, 118, l2, TEXT);
    return;
  }
  for (int i = 0; i < 5; ++i) legendLine(g, 3, 92 + i * 9, kLegends[table][i], noteCol);
}

}  // namespace ui
}  // namespace pg
