#include "boot_menu.h"

#include <cstdio>
#include <cstring>

#include "../platform/display.h"
#include "../platform/keys.h"
#include "../platform/storage.h"
#include "../platform/system.h"
#if PG_USB_AUDIO
#include "../platform/usb_audio.h"
#endif
#include "pg/io/project_io.h"
#include "pg/ui/gfx.h"

namespace app {

namespace sys = plat::sys;
namespace keys = plat::keys;
using pg::ui::Gfx;

namespace {

#if PG_USB_AUDIO
// 7: the USB audio build's way into the ROM download mode without buttons
// (its USB port is TinyUSB, which esptool cannot reset by itself).
enum Item : int { kRate = 0, kBackend, kVolume, kResetSession, kFormat, kStart, kFlash, kNumItems };
#else
enum Item : int { kRate = 0, kBackend, kVolume, kResetSession, kFormat, kStart, kNumItems };
#endif

// Key ids (pg/platform.h layout)
constexpr uint8_t kKBacktick = 0, kKEnter = 41, kKSemicolon = 39, kKDot = 53, kKSpace = 55;

struct MenuState {
  int sel = kStart;
  int confirm = -1;          // item waiting for its second press
  char msg[48] = "";
  uint8_t msgCol = pg::ui::TEXT;
  bool done = false;
};

void itemText(const BootMenuEnv& env, int i, char* out, size_t cap, const MenuState& m) {
  const Settings& s = *env.settings;
  switch (i) {
    case kRate:
      snprintf(out, cap, "1 SAMPLE RATE    %s", s.rate == Rate::k32000 ? "32 KHZ" : "44.1 KHZ");
      break;
    case kBackend: snprintf(out, cap, "2 AUDIO OUTPUT   %s", backendName(s.backend)); break;
    case kVolume: {
      char db[24];
      const float v = volDb(s.volReg);
      const int t = static_cast<int>(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
      snprintf(db, sizeof(db), "%d.%d", t / 10, (t < 0 ? -t : t) % 10);
      snprintf(out, cap, "3 CODEC VOLUME   %s DB", db);
      break;
    }
    case kResetSession:
      snprintf(out, cap, "%s", m.confirm == i ? "4 RESET SESSION? AGAIN=YES" : "4 RESET SESSION + SETTINGS");
      break;
    case kFormat:
      snprintf(out, cap, "%s", m.confirm == i ? "5 FORMAT /INT? AGAIN = YES" : "5 FORMAT INTERNAL STORAGE");
      break;
    case kStart: snprintf(out, cap, "6 START"); break;
#if PG_USB_AUDIO
    case kFlash:
      snprintf(out, cap, "%s", m.confirm == i ? "7 FLASH MODE? AGAIN = YES" : "7 FLASH MODE (FOR UPLOADING)");
      break;
#endif
    default: out[0] = 0; break;
  }
}

void draw(const BootMenuEnv& env, const MenuState& m) {
  pg::Framebuffer& fb = *env.fb;
  Gfx g(fb);
  g.clear(pg::ui::BG);
  g.rect(0, 0, pg::kScreenW, 11, pg::ui::PANEL);
  g.text(3, 2, "POCKET GROOVE BOOT MENU", pg::ui::WHITE);
  g.textR(pg::kScreenW - 3, 2, PG_VERSION, pg::ui::DIM);
  char line[64];
  int y = 14;
  if (env.abnormalReset) {
    snprintf(line, sizeof(line), "LAST RUN ENDED: %s", env.resetReason);
    g.text(3, y, line, pg::ui::RED);
    y += 9;
    if (strncmp(env.resetReason, "BROWNOUT", 8) == 0) {
      g.text(3, y, "SUPPLY DIPPED: BATTERY LOW? CHARGE IT", pg::ui::AMBER);
      y += 9;
    }
  } else {
    g.text(3, y, "KEY HELD AT POWER-ON", pg::ui::DIM);
    y += 9;
  }
  if (!plat::storage::info().intMounted) {   // live: a format from this menu changes it
    g.text(3, y, "INTERNAL STORAGE NOT MOUNTED", pg::ui::AMBER);
    y += 9;
  }
  y += 2;
  for (int i = 0; i < kNumItems; ++i) {
    itemText(env, i, line, sizeof(line), m);
    const bool s = (i == m.sel);
    if (s) g.rect(0, y - 1, pg::kScreenW, 10, pg::ui::RAISED);
    const uint8_t c = (m.confirm == i) ? pg::ui::RED : s ? pg::ui::WHITE : pg::ui::TEXT;
    g.text(6, y, line, c);
    y += 11;
  }
  if (m.msg[0]) g.text(3, pg::kScreenH - 20, m.msg, m.msgCol);
  g.text(3, pg::kScreenH - 9, "; . MOVE  ENT SET  ` START  G0 TAP/HOLD", pg::ui::DIM);
  plat::display::present(fb);
}

void setMsg(MenuState& m, const char* text, uint8_t col) {
  strlcpy(m.msg, text, sizeof(m.msg));
  m.msgCol = col;
}

void activate(const BootMenuEnv& env, MenuState& m, int i) {
  Settings& s = *env.settings;
  pg::FileSystem& fs = *env.fs;
#if PG_USB_AUDIO
  if (i != m.confirm && (i == kResetSession || i == kFormat || i == kFlash)) {
#else
  if (i != m.confirm && (i == kResetSession || i == kFormat)) {
#endif
    m.confirm = i;
    setMsg(m, "PRESS AGAIN TO CONFIRM", pg::ui::AMBER);
    return;
  }
  const int was = m.confirm;
  m.confirm = -1;
  switch (i) {
    case kRate:
      s.rate = (s.rate == Rate::k44100) ? Rate::k32000 : Rate::k44100;
      setMsg(m, saveSettings(fs, s) ? "SAVED" : "NOT SAVED (STORAGE)", pg::ui::DIM);
      break;
    case kBackend:
      s.backend = static_cast<AudioBackend>((static_cast<int>(s.backend) + 1) % 3);
      setMsg(m, saveSettings(fs, s) ? "SAVED" : "NOT SAVED (STORAGE)", pg::ui::DIM);
      break;
    case kVolume:
      s.volReg = kVolDefault;
      setMsg(m, saveSettings(fs, s) ? "VOLUME RESET TO DEFAULT" : "NOT SAVED (STORAGE)", pg::ui::DIM);
      break;
    case kResetSession: {
      if (was != kResetSession) break;
      // Session, UI settings, firmware settings and any half-written temp files.
      static const char* const kFiles[] = {pg::io::kSessionPath, pg::io::kUiSettingsPath, kFwSettingsPath,
                                           "/int/pg/_save.tmp", "/int/pg/_ui.tmp"};
      int removed = 0;
      for (const char* f : kFiles)
        if (fs.exists(f) && fs.remove(f)) ++removed;
      s = Settings();
      char t[48];
      snprintf(t, sizeof(t), "SESSION RESET (%d FILES REMOVED)", removed);
      setMsg(m, t, pg::ui::GREEN);
      break;
    }
    case kFormat: {
      if (was != kFormat) break;
      drawMessage(*env.fb, "FORMATTING STORAGE", "ERASING THE INTERNAL FLASH FILE SYSTEM.",
                  "THIS TAKES UP TO ~30 S. DO NOT POWER OFF.");
      sys::serialWrite("[boot] formatting /int...\n");
      const bool ok = plat::storage::formatInternal();
      s = Settings();
      if (ok) saveSettings(fs, s);
      setMsg(m, ok ? "FORMATTED. ALL INTERNAL SONGS ERASED" : "FORMAT FAILED", ok ? pg::ui::GREEN : pg::ui::RED);
      sys::serialWrite(ok ? "[boot] format OK\n" : "[boot] format FAILED\n");
      break;
    }
    case kStart: m.done = true; break;
#if PG_USB_AUDIO
    case kFlash: {
      if (was != kFlash) break;
      drawMessage(*env.fb, "DOWNLOAD MODE", "FLASH NOW FROM THE COMPUTER:", "PIO RUN -E <ENV> -T UPLOAD",
                  "POWER OFF AND ON TO LEAVE");
      sys::serialWrite("[boot] rebooting into the ROM download mode (boot menu): flash now\n");
      sys::delayMs(150);
      plat::usbaudio::rebootToDownloadMode();
      break;
    }
#endif
    default: break;
  }
}

void moveSel(MenuState& m, int d) {
  m.sel = (m.sel + d + kNumItems) % kNumItems;
  if (m.confirm >= 0) { m.confirm = -1; m.msg[0] = 0; }
}

}  // namespace

void drawMessage(pg::Framebuffer& fb, const char* title, const char* l1, const char* l2, const char* l3,
                 uint8_t colour) {
  Gfx g(fb);
  g.clear(pg::ui::BG);
  g.textC(pg::kScreenW / 2, 30, title, colour ? colour : static_cast<uint8_t>(pg::ui::AMBER), 2);
  if (l1) g.textC(pg::kScreenW / 2, 64, l1, pg::ui::TEXT);
  if (l2) g.textC(pg::kScreenW / 2, 76, l2, pg::ui::TEXT);
  if (l3) g.textC(pg::kScreenW / 2, 88, l3, pg::ui::DIM);
  g.textR(pg::kScreenW - 3, pg::kScreenH - 9, PG_VERSION, pg::ui::DIM);
  plat::display::present(fb);
}

uint64_t runBootMenu(const BootMenuEnv& env) {
  MenuState m;
  if (env.abnormalReset) setMsg(m, "CHECK SETTINGS, THEN START", pg::ui::AMBER);
  sys::serialWrite("[boot] BOOT MENU. Serial: 1..6 select+run, j/k move, Enter run.\n");
  uint32_t g0DownMs = 0;
  bool g0Held = false, g0LongDone = false;
  bool redraw = true;
  while (!m.done) {
    if (redraw) { draw(env, m); redraw = false; }
    keys::Event e;
    if (keys::waitEvent(&e, 50)) {
      const uint8_t k = e.ev.key;
      if (k == keys::kKeyG0) {
        if (e.ev.down) { g0Held = true; g0LongDone = false; g0DownMs = sys::millis(); }
        else {
          if (g0Held && !g0LongDone) moveSel(m, 1);
          g0Held = false;
        }
        redraw = true;
      } else if (e.ev.down) {
        if (k == kKSemicolon) moveSel(m, -1);
        else if (k == kKDot) moveSel(m, 1);
        else if (k == kKEnter || k == kKSpace) activate(env, m, m.sel);
        else if (k == kKBacktick) m.done = true;
        else if (k >= 1 && k <= kNumItems) {   // number row "1".."6" are ids 1..6
          if (m.sel != k - 1) moveSel(m, k - 1 - m.sel);
          activate(env, m, k - 1);
        }
        redraw = true;
      }
    }
    if (g0Held && !g0LongDone && sys::millis() - g0DownMs >= 1000) {
      g0LongDone = true;
      activate(env, m, m.sel);
      redraw = true;
    }
    for (int n = 0; n < 8; ++n) {
      const int ch = sys::serialRead();
      if (ch < 0) break;
      static int prev = 0;
      const int last = prev;
      prev = ch;
      if (ch == '\n' && last == '\r') continue;   // CR LF = one Enter
      if (ch >= '1' && ch < '1' + kNumItems) {
        const int i = ch - '1';
        if (m.sel != i) moveSel(m, i - m.sel);
        activate(env, m, i);
      } else if (ch == 'j') moveSel(m, 1);
      else if (ch == 'k') moveSel(m, -1);
      else if (ch == '\r' || ch == '\n') activate(env, m, m.sel);
      else continue;
      redraw = true;
    }
  }
  sys::serialWrite("[boot] boot menu: START\n");
  return keys::heldMask();
}

}  // namespace app
