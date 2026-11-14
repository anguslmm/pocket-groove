// The App's stage 1 commands and report lines: the pads' calibration, the touch, the
// display's orientation, the line input (docs/DAISY.md "Driving it from the
// computer"). Key-script lines from the serial port:
//   >pads                      the [pads] lines with every pad's calibration
//   >pads watch [MS|off]       a [pads] line every MS (default 100): raw readings,
//                              conductance, peaks since the last line, pressure, state
//   >pads window N             the velocity window, 1..4 scans
//   >pads release N            scans below the release level before the note ends, 1..4
//   >pads hyst R               the release level as a fraction of the threshold (0.1..0.95)
//   >pads xtalk R [MS]         crosstalk: a pad below R of a neighbour hit within MS is dropped
//   >pads reset                every pad and the above back to the defaults
//   >pad N|all [thr US] [ceil US] [curve X] [on|off]
//   >touch X Y [MS]            a touch from the computer at canvas X Y, lifted after MS
//   >touch map N|auto          the touch orientation 0..7 (touch.h), or from the display
//   >display turn              the picture turned over (180 degrees); the touch follows
//   >display spi N             the display's SPI clock: 25 MHz / N (2, 4, 8, 16)
// Every change is saved at once (/int/pg/daisy.cfg, settings.h) and printed back.
// No %f anywhere: newlib-nano's printf has no floating point (fixed-point helpers).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.h"

namespace pgd {

namespace {

struct Fx {
  char s[32];
};
// v with `dec` decimals (0..3), as text.
Fx fx(float v, int dec) {
  Fx f;
  int scale = 1;
  for (int i = 0; i < dec; ++i) scale *= 10;
  const bool neg = v < 0.0f;
  const float a = neg ? -v : v;
  if (!(a < 2.0e9f / float(scale))) {
    std::snprintf(f.s, sizeof(f.s), "%s", neg ? "-big" : "big");
    return f;
  }
  const long q = lrintf(a * float(scale));
  if (dec == 0) std::snprintf(f.s, sizeof(f.s), "%s%ld", neg ? "-" : "", q);
  else std::snprintf(f.s, sizeof(f.s), "%s%ld.%0*ld", neg ? "-" : "", q / scale, dec, q % scale);
  return f;
}

// A decimal number: [-]digits[.digits]. false if malformed.
bool num(const char* s, float* out) {
  if (!s || !*s) return false;
  bool neg = false;
  if (*s == '-') {
    neg = true;
    ++s;
  }
  float v = 0.0f, frac = 0.1f;
  bool digits = false, dot = false;
  for (; *s; ++s) {
    if (*s >= '0' && *s <= '9') {
      digits = true;
      if (!dot) v = v * 10.0f + float(*s - '0');
      else {
        v += frac * float(*s - '0');
        frac *= 0.1f;
      }
    } else if (*s == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  *out = neg ? -v : v;
  return true;
}

int words(char* buf, char** w, int max) {
  int n = 0;
  char* p = buf;
  while (*p && n < max) {
    while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
    if (!*p) break;
    w[n++] = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r') ++p;
    if (*p) *p++ = 0;
  }
  return n;
}

const char* const kPadName[PadScanner::kPads] = {"kick", "snare", "clap", "hat"};

int dbfs(uint16_t peak) { return peak ? static_cast<int>(lrintf(20.0f * log10f(float(peak) / 32767.0f))) : -99; }

}  // namespace

void App::saveSettings() {
  uint8_t b[DaisySettings::kBytes];
  const size_t n = settings_.encode(b, sizeof(b));
  const bool ok = n && fs_.write(DaisySettings::kPath, b, n);
  for (int i = 0; i < PadScanner::kPads; ++i) pads_.setCal(i, settings_.pads[i]);
  pads_.setGlobal(settings_.padGlobal);
  hal_->applySettings(settings_);
  if (!ok) sp("[settings] COULD NOT WRITE %s\n", DaisySettings::kPath);
}

void App::printPads(bool withCal) {
  if (!padsSeen_) {
    sp("[pads] none (%s)\n", hal_->stageName());
    return;
  }
  const PadGlobal& g = pads_.global();
  const float scanMs = 1000.0f / pads_.scanHz();
  if (withCal) {
    sp("[pads] scan %s Hz (%s ms, the audio interrupt); window %d scans (the note %s ms after the crossing scan); "
       "release below %d%% of the threshold for %d scans; crosstalk: below %s of a neighbour within %d scans (%s ms)\n",
       fx(pads_.scanHz(), 0).s, fx(scanMs, 1).s, g.windowScans, fx(scanMs * float(g.windowScans - 1), 1).s,
       static_cast<int>(lrintf(g.releaseRatio * 100.0f)), g.releaseScans, fx(g.xtalkRatio, 2).s, g.xtalkScans,
       fx(scanMs * float(g.xtalkScans), 0).s);
    sp("[pads] notes %lu, ups %lu, pressure events %lu, events dropped %lu; settings %s\n",
       static_cast<unsigned long>(padNotes_), static_cast<unsigned long>(padUps_),
       static_cast<unsigned long>(padPressures_), static_cast<unsigned long>(pads_.dropped()), settingsLoad_);
    for (int i = 0; i < PadScanner::kPads; ++i) {
      const PadCal& c = pads_.cal(i);
      const float x = pads_.lastReading(i);
      sp("[pads] %d %-5s %-5s x %s (%s mV) G %s uS, peak %s uS, pressure %s | thr %s uS ceil %s uS curve %s %s | "
         "hits %lu ghosts %lu blips %lu, last velocity %s\n",
         i + 1, kPadName[i], PadScanner::stateName(pads_.state(i)), fx(x, 4).s, fx(x * 3300.0f, 0).s,
         fx(pads_.lastUs(i), 1).s, fx(pads_.peakUs(i), 1).s, fx(pads_.pressure(i), 2).s, fx(c.threshold, 1).s,
         fx(c.ceiling, 0).s, fx(c.curve, 2).s, c.on ? "on" : "OFF", static_cast<unsigned long>(pads_.hits(i)),
         static_cast<unsigned long>(pads_.ghosts(i)), static_cast<unsigned long>(pads_.blips(i)),
         fx(pads_.lastVelocity(i), 2).s);
    }
    pads_.clearPeaks();
    return;
  }
  // the watch line: everything for tuning in one line per interval
  char l[300];
  int n = std::snprintf(l, sizeof(l), "[pads] x");
  for (int i = 0; i < PadScanner::kPads; ++i) n += std::snprintf(l + n, sizeof(l) - size_t(n), " %s", fx(pads_.lastReading(i), 3).s);
  n += std::snprintf(l + n, sizeof(l) - size_t(n), " | uS");
  for (int i = 0; i < PadScanner::kPads; ++i) n += std::snprintf(l + n, sizeof(l) - size_t(n), " %s", fx(pads_.lastUs(i), 1).s);
  n += std::snprintf(l + n, sizeof(l) - size_t(n), " | peak");
  for (int i = 0; i < PadScanner::kPads; ++i) n += std::snprintf(l + n, sizeof(l) - size_t(n), " %s", fx(pads_.peakUs(i), 1).s);
  n += std::snprintf(l + n, sizeof(l) - size_t(n), " | p");
  for (int i = 0; i < PadScanner::kPads; ++i) n += std::snprintf(l + n, sizeof(l) - size_t(n), " %s", fx(pads_.pressure(i), 2).s);
  n += std::snprintf(l + n, sizeof(l) - size_t(n), " |");
  for (int i = 0; i < PadScanner::kPads; ++i)
    n += std::snprintf(l + n, sizeof(l) - size_t(n), " %s", PadScanner::stateName(pads_.state(i)));
  n += std::snprintf(l + n, sizeof(l) - size_t(n), " | vel");
  for (int i = 0; i < PadScanner::kPads; ++i)
    n += std::snprintf(l + n, sizeof(l) - size_t(n), " %s", fx(pads_.lastVelocity(i), 2).s);
  pads_.clearPeaks();
  sp("%s\n", l);
}

void App::printTouch() {
  Hal::TouchInfo t{};
  const bool have = hal_->touchInfo(&t);
  char last[96];
  if (pointerSeen_)
    std::snprintf(last, sizeof(last), "last canvas (%d,%d) raw (%d,%d) %s", lastPointer_.x, lastPointer_.y,
                  lastPointer_.rawX, lastPointer_.rawY, lastPointer_.down ? "DOWN" : "up");
  else
    std::snprintf(last, sizeof(last), "no touch yet");
  if (!have) {
    sp("[touch] no touch controller in this build; touches %lu, events %lu (from the computer: >touch X Y); %s\n",
       static_cast<unsigned long>(pointerDowns_), static_cast<unsigned long>(pointerEvents_), last);
    return;
  }
  sp("[touch] FT6206 %s (chip id 0x%02x, firmware %u), IRQ %s; touches %lu, events %lu; %s; reads %lu, read errors "
     "%lu; map %u%s\n",
     t.state, t.chipId, t.firmware, t.irq < 0 ? "?" : t.irq ? "high (no touch)" : "LOW (touched)",
     static_cast<unsigned long>(pointerDowns_), static_cast<unsigned long>(pointerEvents_), last,
     static_cast<unsigned long>(t.reads), static_cast<unsigned long>(t.errors), t.orientation,
     settings_.touchOrientation < 0 ? " (from the display)" : " (forced: >touch map auto undoes it)");
}

void App::printLineIn() {
  if (!lineIn_) {
    sp("[linein] none (no codec input in this build or no sample pool)\n");
    return;
  }
  const LineMic::Last& l = lineMic_.last();
  sp("[linein] codec line in, %lu Hz in and stored (a slot holds %lu.%02lu s); %s%s\n",
     static_cast<unsigned long>(hal_->sampleRate()),
     static_cast<unsigned long>(pg::sampler::kSlotMaxSamples / (hal_->sampleRate() ? hal_->sampleRate() : 48000)),
     static_cast<unsigned long>(pg::sampler::kSlotMaxSamples * 100u / (hal_->sampleRate() ? hal_->sampleRate() : 48000) % 100),
     lineMic_.armed() ? "RECORDING" : "idle", l.valid ? "" : "; no recording yet (hold L on a sampler page; >down l)");
  if (!l.valid) return;
  const uint32_t cs = l.rate ? l.samples * 100u / l.rate : 0;
  sp("[linein] last: slot %d %s (commit %s), %lu samples at %lu Hz = %lu.%02lu s; mono %s; input peaks L %d R %d "
     "dBFS\n",
     l.slot + 1, pg::sampler::recStateName(l.state), l.result, static_cast<unsigned long>(l.samples),
     static_cast<unsigned long>(l.rate), static_cast<unsigned long>(cs / 100), static_cast<unsigned long>(cs % 100),
     l.mono == LineMic::Mono::Sum ? "(L+R)/2" : "L alone (R silent)", dbfs(l.peakL), dbfs(l.peakR));
  sp("[linein] last: threshold %u, room rms %u, recorded peak %u; ring overruns %lu samples, pool writes %lu "
     "(errors %lu)\n",
     l.threshold, l.floorRms, l.peak, static_cast<unsigned long>(l.overruns), static_cast<unsigned long>(l.writes),
     static_cast<unsigned long>(l.writeErrors));
}

bool App::settingsCommand(const char* line) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s", line);
  char* w[12];
  const int n = words(buf, w, 12);
  if (n == 0) return false;
  const char* v = w[0];
  float f = 0.0f;
  if (std::strcmp(v, "pads") == 0) {
    if (n == 1) {
      printPads(true);
      return true;
    }
    PadGlobal& g = settings_.padGlobal;
    const char* k = w[1];
    bool changed = true;
    if (std::strcmp(k, "watch") == 0) {
      changed = false;
      if (n > 2 && std::strcmp(w[2], "off") == 0) padsWatchMs_ = 0;
      else padsWatchMs_ = (n > 2 && num(w[2], &f) && f >= 10.0f) ? static_cast<uint32_t>(f) : 100u;
      sp("[pads] watch %s\n", padsWatchMs_ ? "on" : "off");
    } else if (std::strcmp(k, "window") == 0 && n > 2 && num(w[2], &f)) {
      g.windowScans = static_cast<uint8_t>(f < 1.0f ? 1 : f > 4.0f ? 4 : f);
    } else if (std::strcmp(k, "release") == 0 && n > 2 && num(w[2], &f)) {
      g.releaseScans = static_cast<uint8_t>(f < 1.0f ? 1 : f > 4.0f ? 4 : f);
    } else if (std::strcmp(k, "hyst") == 0 && n > 2 && num(w[2], &f)) {
      g.releaseRatio = f;
    } else if (std::strcmp(k, "xtalk") == 0 && n > 2 && num(w[2], &f)) {
      g.xtalkRatio = f;
      float ms = 0.0f;
      if (n > 3 && num(w[3], &ms)) {
        const float scans = ms * pads_.scanHz() / 1000.0f;
        g.xtalkScans = static_cast<uint8_t>(scans < 0.0f ? 0 : scans > 20.0f ? 20 : lrintf(scans));
      }
    } else if (std::strcmp(k, "reset") == 0) {
      for (PadCal& c : settings_.pads) c = PadCal{};
      g = PadGlobal{};
    } else {
      changed = false;
      sp("[pads] ? >pads [watch [MS|off] | window N | release N | hyst R | xtalk R [MS] | reset]\n");
    }
    if (changed) {
      g = PadScanner::sanitize(g);
      saveSettings();
      printPads(true);
    }
    return true;
  }
  if (std::strcmp(v, "pad") == 0) {
    int first = 0, last = PadScanner::kPads - 1;
    if (n < 2 || (std::strcmp(w[1], "all") != 0 && !(num(w[1], &f) && f >= 1.0f && f <= float(PadScanner::kPads)))) {
      sp("[pads] ? >pad N|all [thr US] [ceil US] [curve X] [on|off]\n");
      return true;
    }
    if (std::strcmp(w[1], "all") != 0) first = last = static_cast<int>(f) - 1;
    bool bad = false;
    for (int p = first; p <= last; ++p) {
      PadCal c = settings_.pads[p];
      for (int i = 2; i < n; ++i) {
        if (std::strcmp(w[i], "on") == 0) c.on = true;
        else if (std::strcmp(w[i], "off") == 0) c.on = false;
        else if (i + 1 < n && num(w[i + 1], &f) && (std::strcmp(w[i], "thr") == 0 || std::strcmp(w[i], "threshold") == 0)) {
          c.threshold = f;
          ++i;
        } else if (i + 1 < n && num(w[i + 1], &f) && (std::strcmp(w[i], "ceil") == 0 || std::strcmp(w[i], "ceiling") == 0)) {
          c.ceiling = f;
          ++i;
        } else if (i + 1 < n && num(w[i + 1], &f) && std::strcmp(w[i], "curve") == 0) {
          c.curve = f;
          ++i;
        } else {
          bad = true;
        }
      }
      settings_.pads[p] = PadScanner::sanitize(c);
    }
    if (bad) sp("[pads] some words not understood (>pad N|all [thr US] [ceil US] [curve X] [on|off])\n");
    saveSettings();
    printPads(true);
    return true;
  }
  if (std::strcmp(v, "touch") == 0) {
    float x = 0.0f, y = 0.0f, ms = 100.0f;
    if (n >= 3 && std::strcmp(w[1], "map") == 0) {
      if (std::strcmp(w[2], "auto") == 0) settings_.touchOrientation = -1;
      else if (num(w[2], &f) && f >= 0.0f && f <= 7.0f) settings_.touchOrientation = static_cast<int8_t>(f);
      saveSettings();
      printTouch();
      return true;
    }
    if (n >= 3 && num(w[1], &x) && num(w[2], &y)) {
      if (n > 3) num(w[3], &ms);
      injX_ = static_cast<int16_t>(x);
      injY_ = static_cast<int16_t>(y);
      injDown_ = true;
      injUpMs_ = hal_->millis() + static_cast<uint32_t>(ms < 0.0f ? 0.0f : ms);
      pointer(Hal::Pointer{injX_, injY_, -1, -1, true, hal_->millis()});
      return true;
    }
    sp("[touch] ? >touch X Y [MS] | >touch map N|auto\n");
    return true;
  }
  if (std::strcmp(v, "display") == 0) {
    if (n >= 2 && std::strcmp(w[1], "turn") == 0) {
      settings_.displayTurned = !settings_.displayTurned;
    } else if (n >= 3 && std::strcmp(w[1], "spi") == 0 && num(w[2], &f) &&
               (f == 2.0f || f == 4.0f || f == 8.0f || f == 16.0f)) {
      settings_.spiDivider = static_cast<uint8_t>(f);
    } else {
      sp("[display] ? >display turn | >display spi 2|4|8|16\n");
      return true;
    }
    saveSettings();
    sp("[display] %s, SPI 25 MHz / %u (saved)\n", settings_.displayTurned ? "turned over (MADCTL 0xE8)" : "upright (MADCTL 0x28)",
       settings_.spiDivider);
    return true;
  }
  return false;
}

}  // namespace pgd
