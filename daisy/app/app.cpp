#include "app.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "pg/io/project_io.h"
#include "pg/ui/gfx.h"

namespace pgd {

namespace {

constexpr uint32_t kFrameMs = 33;          // the UI's frame, as on the Cardputer (~30 fps)
constexpr uint32_t kStatsEveryMs = 5000;
constexpr uint32_t kConfirmMs = 3000;      // 'B' / 'R' wait this long for 'y'
constexpr uint32_t kXferSpinMs = 8;        // while a transfer runs: wait for the host's next line
constexpr float kGainStepDb = 2.0f, kGainMinDb = -40.0f, kGainMaxDb = 6.0f;
constexpr uint8_t kKMinus = 11, kKEquals = 12;   // pg/platform.h key ids
constexpr uint64_t kMatrixMask = (1ull << pg::kNumKeys) - 1;

struct SerialOut final : pg::io::XferOut {
  Hal* hal = nullptr;
  void send(const char* line, size_t len) override { hal->serialWrite(line, len); }
};
SerialOut g_xferOut;

int pct10(uint32_t permille) { return static_cast<int>(permille); }

}  // namespace

void App::writeRaw(const char* p, size_t n) {
  if (logging_) {
    const size_t room = kBootLog - 1 - bootLogLen_;
    const size_t k = n < room ? n : room;
    std::memcpy(bootLog_ + bootLogLen_, p, k);
    bootLogLen_ += k;
    bootLog_[bootLogLen_] = 0;
  }
  hal_->serialWrite(p, n);
}

void App::sp(const char* fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) writeRaw(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
}

void App::setGainDb(float db) {
  if (db < kGainMinDb) db = kGainMinDb;
  if (db > kGainMaxDb) db = kGainMaxDb;
  gainDb_ = db;
  hal_->setOutputGain(powf(10.0f, db / 20.0f));
}

// ---- boot ------------------------------------------------------------------------------

void App::boot(Hal& hal, const AppMemory& mem, const AppConfig& cfg) {
  hal_ = &hal;
  mem_ = mem;
  cfg_ = cfg;
  g_xferOut.hal = &hal;
  booted_ = false;
  led_ = LedMode::Booting;
  hal.setLed(true);
  bootLogLen_ = 0;
  bootLog_[0] = 0;
  logging_ = true;
  bootMs_ = hal.millis();

  sp("\n\n===== POCKET GROOVE %s on the DAISY (%s) =====\n", cfg.version, hal.stageName());
  sp("[boot] board %s\n", hal.boardName());

  // /int: RAM, loaded from the newest flash snapshot.
  const uint32_t payload = mem.fsBankBytes > FsPersist::kHeaderSector ? mem.fsBankBytes - FsPersist::kHeaderSector : 0;
  ramfs_.init(pg::io::kMountInt, mem.fsArena, mem.fsSlotBytes, payload);
  persist_.init(&ramfs_, mem.fsFlash, mem.fsBankBytes, mem.fsStaging);
  const uint32_t t0 = hal.millis();
  fsLoad_ = mem.fsFlash ? persist_.load() : FsPersist::Load::Empty;
  sp("[boot] /int: %s, %d files, %lu bytes (%lu ms)\n",
     fsLoad_ == FsPersist::Load::Loaded ? "snapshot loaded"
     : fsLoad_ == FsPersist::Load::Corrupt ? "SNAPSHOT CORRUPT: starting empty"
                                           : "no snapshot yet (first boot): empty",
     ramfs_.fileCount(), static_cast<unsigned long>(ramfs_.usedBytes()), static_cast<unsigned long>(hal.millis() - t0));
  fs_.set(&ramfs_, hal.sdCard());

  // The sample pool: RAM image, loaded from its flash copy.
  mirror_.init(mem.poolImage, mem.poolFlash, mem.poolStaging);
  const uint32_t t1 = hal.millis();
  poolLoaded_ = mem.poolImage && mirror_.loadFrom();
  if (mem.poolImage) {
    pool_.attach(mirror_.image());
    poolDropped_ = mirror_.dropBadSlots(pool_);
    writer_.attach(&mirror_, &pool_);
  } else {
    pool_.attach(nullptr);
  }
  int used = 0;
  for (int i = 0; i < pg::sampler::kPoolSlots; ++i) used += pool_.header(i) ? 1 : 0;
  if (poolDropped_) sp("[boot] samples: %d slot(s) whose audio did not match the header emptied\n", poolDropped_);
  sp("[boot] samples: pool %s, %d of %d slots hold a sample (%lu ms)\n",
     !mem.poolImage ? "NONE" : poolLoaded_ ? "loaded from flash" : "flash unreadable: empty", used,
     pg::sampler::kPoolSlots, static_cast<unsigned long>(hal.millis() - t1));
  if (romMissing_) sp("[boot] ROM part of the image MISSING: the built-in sounds are silent. Flash the whole .bin\n");

  // The Daisy's own settings (pads, display orientation, touch map), from /int.
  {
    settings_ = DaisySettings{};
    uint8_t b[DaisySettings::kBytes + 8];
    size_t n = 0;
    if (!fs_.exists(DaisySettings::kPath)) settingsLoad_ = "defaults (no file yet)";
    else if (fs_.read(DaisySettings::kPath, b, sizeof(b), &n) && settings_.decode(b, n)) settingsLoad_ = "loaded";
    else settingsLoad_ = "FILE UNREADABLE: defaults";
    pads_.reset();
    pads_.setScanHz(static_cast<float>(hal.sampleRate()) / static_cast<float>(hal.blockFrames() ? hal.blockFrames() : 96));
    for (int i = 0; i < PadScanner::kPads; ++i) pads_.setCal(i, settings_.pads[i]);
    pads_.setGlobal(settings_.padGlobal);
    hal.applySettings(settings_);
    sp("[boot] settings %s (%s)\n", DaisySettings::kPath, settingsLoad_);
  }

  // Recording: the codec's line input, into the pool (line_mic.h).
  lineMic_.init(mem.poolImage ? &writer_ : nullptr, mem.lineRing, mem.lineRingSamples, hal.sampleRate());
  const bool lineIn = hal.lineInput() && mem.lineRing && mem.poolImage;
  lineIn_ = lineIn;
  lineMic_.setAvailable(false);   // once the audio runs (servicePads)

  inst_.init(cfg.sampleRate, /*monoOutput=*/false);
  inst_.setSamplePool(mem.poolImage ? &pool_ : nullptr);
  pg::ui::initPalette(fb_);
  ui_.init(inst_.engine(), inst_.project(), &fs_);
  ui_.setSampler(&inst_.sampler());
  ui_.setMelodic(&inst_.melodic());
  ui_.setMic(lineIn ? &lineMic_ : nullptr);
  xfer_.init(&fs_, cfg.version);
  xfer_.setSamples(mem.poolImage ? &writer_ : nullptr);
  samplesChanged_ = xfer_.samplesChanged();
  ui_.boot(hal.millis());
  ui_.setPadJob(settings_.padJob ? pg::ui::PadJob::Notes : pg::ui::PadJob::Drums);
  for (InjPad& p : inj_) p = InjPad{};
  pendMask_ = 0;
  sp("[boot] UI up, project \"%s\"%s\n", ui_.projectName(), cfg.autoplay ? ", autoplay" : "");

  meter_.configure(hal.cpuHz(), static_cast<uint32_t>(static_cast<uint64_t>(hal.cpuHz()) * hal.blockFrames() /
                                                      (hal.sampleRate() ? hal.sampleRate() : 48000u)));
  setGainDb(gainDb_);
  if (cfg.autoplay) {
    pg::Command c{};
    c.type = pg::CmdType::Play;
    inst_.engine().post(c);
  }

  sched_ = Scheduler{};
  sched_.add("serial", 0, tSerial, this);
  sched_.add("keys", 0, tKeys, this);
  sched_.add("ui", kFrameMs, tUi, this);
  sched_.add("screen", 0, tScreen, this);
  sched_.add("store", 5, tStore, this);
  sched_.add("led", 10, tLed, this);
  sched_.add("stats", kStatsEveryMs, tStats, this);
  sched_.add("pots", 10, tPots, this);
  sched_.add("pads", 0, tPads, this);

  script_ = KeyScript{};
  link_ = SerialLink{};
  dirtyY0_ = 0;
  dirtyY1_ = 0;
  lastReportMs_ = hal.millis();
  meter_.takeWindow();
  booted_ = true;
  sp("[boot] ready in %lu ms. Serial: r = report, h = help\n", static_cast<unsigned long>(hal.millis() - bootMs_));
  logging_ = false;
}

void App::render(int16_t* out, int frames, const float* in) {
  meter_.begin(hal_->cycles());
  // The pads first: one scan per block, so the scan interval is the block's and
  // never waits for the main loop (pads.h LATENCY).
  float x[PadScanner::kPads];
  if (hal_->readPads(x)) {
    pads_.scan(x);
    padsSeen_ = true;
  }
  if (in) lineMic_.audioIn(in, frames);
  inst_.process(out, frames);
  int pk = 0;
  for (int i = 0; i < 2 * frames; ++i) {
    const int a = out[i] < 0 ? -out[i] : out[i];
    pk = a > pk ? a : pk;
  }
  if (static_cast<uint32_t>(pk) > outPeak_.load(std::memory_order_relaxed))
    outPeak_.store(static_cast<uint32_t>(pk), std::memory_order_relaxed);
  meter_.end(hal_->cycles());
}

void App::loopOnce() {
  if (!booted_) return;
  hal_->service(hal_->millis());
  sched_.runOnce(msClock, usClock, this);
  hal_->serialFlush();
}

// ---- keys ------------------------------------------------------------------------------

void App::handleKey(const pg::KeyEvent& e, uint32_t now) {
  const uint8_t k = e.key;
  if (k == KeyScript::kKeyPadJob) {   // the desktop's padjob key (F4): the pads' job
    if (e.down) ui_.togglePadJob();
    return;
  }
  if (k > pg::kKeyG0) return;
  const uint64_t bit = 1ull << k;
  if (!e.down) {
    if (k == pg::kKeyG0) {
      g0Held_ = false;
      ui_.tiltRelease(now);
      hal_->tiltRelease();
      return;
    }
    if (swallow_ & bit) {
      swallow_ &= ~bit;
      return;
    }
    ui_.onKey(e, now);
    return;
  }
  if (k == pg::kKeyG0) {
    g0Held_ = true;
    g0UsedForGain_ = false;
    if (hal_->imuPresent()) {
      ui_.tiltPress(now);
      if (ui_.tiltHeld()) {
        const pg::ui::Ui::TiltSpec ts = ui_.tiltSpec();
        if (ts.active) hal_->tiltPress(ts.response[0], ts.response[1]);
      }
    }
    return;
  }
  if (g0Held_ && (k == kKMinus || k == kKEquals)) {
    if (ui_.tiltHeld()) {
      ui_.tiltCancel(now);
      hal_->tiltRelease();
    }
    g0UsedForGain_ = true;
    swallow_ |= bit;
    setGainDb(gainDb_ + (k == kKEquals ? kGainStepDb : -kGainStepDb));
    char t[32];
    std::snprintf(t, sizeof(t), "OUTPUT %+d DB", static_cast<int>(lrintf(gainDb_)));
    ui_.notify(t);
    return;
  }
  ui_.onKey(e, now);
}

void App::serviceKeys(uint32_t now) {
  script_.step(now, emitKey, this);
  pg::KeyEvent e;
  for (int n = 0; n < 32 && hal_->pollKey(&e); ++n) {
    ++physical_;
    handleKey(e, hal_->millis());
  }
  if (ui_.tiltHeld()) {
    float x, y;
    if (hal_->tiltRead(&x, &y)) ui_.tiltInput(x, y, now);
  }
  servicePointer(now);
}

// ---- pads -------------------------------------------------------------------------------

// THE PADS' SOUND, in this one function: the core's pad API (pg/ui/pads.h). A hit
// is the Ui's padDown with the scanner's velocity (after the pad's curve, 0..1: the
// Ui makes it 1..127 linearly), pressure while held its padPressure, the release its
// padUp. What a pad plays is the Ui's: the job (drums: KICK SNARE CHH PERC; notes: the
// selected track up the scale), live record, the `vel` and `press` lines of the
// track's instrument. The sensors' events (servicePads) and the computer's (">hit")
// both come through here. The UI thread: the main loop.
void App::padEvent(const PadEvent& e) {
  const uint32_t now = hal_->millis();
  if (e.type == PadEvent::Down) {
    ui_.padDown(e.pad, e.value, now);
    ++padNotes_;
    lastPad_ = static_cast<int8_t>(e.pad);
    lastPadVel_ = e.value;
  } else if (e.type == PadEvent::Up) {
    ui_.padUp(e.pad, now);
    ++padUps_;
  } else {
    ui_.padPressure(e.pad, e.value, now);
    pressMs_[e.pad] = now;
    ++padPressures_;
  }
}

// Every main-loop pass: the scanner's events (queued by the audio interrupt, 500 scans
// a second) into the UI. Down and Up go at once, in order. Pressure is merged: only the
// newest value per pad is kept, and handed over at most once per pad every
// kPressEveryMs (the UI's own limit per track, so what arrives is sent, not left for
// its next frame); a hit or a release drops what waited from before it.
void App::servicePads(uint32_t now) {
  lineMic_.setAvailable(lineIn_ && hal_->audioRunning());
  PadEvent e;
  for (int n = 0; n < PadScanner::kRing && pads_.pop(&e); ++n) {
    if (e.pad >= kAppPads) continue;
    if (e.type == PadEvent::Pressure) {
      ++padPressuresIn_;
      pendPress_[e.pad] = e.value;
      pendMask_ |= 1u << e.pad;
      continue;
    }
    pendMask_ &= ~(1u << e.pad);
    padEvent(e);
  }
  for (int p = 0; p < kAppPads; ++p) {   // the computer's hits that end now
    if (inj_[p].down && inj_[p].upAt && static_cast<int32_t>(now - inj_[p].upAt) >= 0) {
      inj_[p].down = false;
      pendMask_ &= ~(1u << p);
      padEvent(PadEvent{PadEvent::Up, static_cast<uint8_t>(p), 0.0f, 0});
    }
  }
  if (pendMask_) {
    for (int p = 0; p < kAppPads; ++p) {
      if (!((pendMask_ >> p) & 1u) || now - pressMs_[p] < pg::ui::kPressEveryMs) continue;
      pendMask_ &= ~(1u << p);
      padEvent(PadEvent{PadEvent::Pressure, static_cast<uint8_t>(p), pendPress_[p], 0});
    }
  }
  if (static_cast<uint8_t>(ui_.padJob()) != settings_.padJob) {   // the job changed (key, command): kept
    settings_.padJob = static_cast<uint8_t>(ui_.padJob());
    saveSettings();
  }
  if (padsWatchMs_ && now - padsWatchLast_ >= padsWatchMs_) {
    padsWatchLast_ = now;
    printPads(false);
  }
}

// ---- touch ------------------------------------------------------------------------------

void App::pointer(const Hal::Pointer& p) {
  ++pointerEvents_;
  if (p.down && !(pointerSeen_ && lastPointer_.down)) ++pointerDowns_;
  lastPointer_ = p;
  pointerSeen_ = true;
  if (mirror_on_) {   // the screen mirror draws it (tools/pg-daisy mirror: a dot)
    char l[40];
    const int n = std::snprintf(l, sizeof(l), "~PGT %c %d %d\n", p.down ? 'D' : 'U', p.x, p.y);
    if (n > 0) hal_->serialWrite(l, static_cast<size_t>(n));
  }
}

void App::servicePointer(uint32_t now) {
  Hal::Pointer p;
  for (int n = 0; n < 16 && hal_->pollPointer(&p); ++n) pointer(p);
  if (injDown_ && static_cast<int32_t>(now - injUpMs_) >= 0) {
    injDown_ = false;
    pointer(Hal::Pointer{injX_, injY_, -1, -1, false, now});
  }
}

// ---- serial ----------------------------------------------------------------------------

void App::serviceXfer(uint32_t now) {
  pg::io::XferEnv env;
  env.nowMs = now;
  env.playing = inst_.engine().isPlaying();
  env.recording = ui_.micBusy();
  const uint32_t before = xfer_.stats().timeouts;
  xfer_.service(env, g_xferOut);
  if (xfer_.stats().timeouts != before) sp("[xfer] transfer abandoned by the host: timed out, nothing saved\n");
  if (xfer_.samplesChanged() != samplesChanged_) {
    samplesChanged_ = xfer_.samplesChanged();
    ui_.notify("SAMPLE POOL UPDATED");
    sp("[samples] the pool changed (tools/pg sample); flash copy follows in the background\n");
  }
}

void App::serviceSerial(uint32_t now) {
  const uint32_t t0 = hal_->millis();
  int letters = 0;
  for (int n = 0; n < 4096; ++n) {
    const int ch = hal_->serialRead();
    if (ch < 0) {
      // A transfer waits for each reply: spin a little for the next request, so a file
      // moves at several chunks per pass (the audio is an interrupt: it never waits).
      if (xfer_.busy() && hal_->millis() - t0 < kXferSpinMs) {
        hal_->serialFlush();
        continue;
      }
      break;
    }
    const uint32_t t = hal_->millis();
    switch (link_.feed(static_cast<uint8_t>(ch), t, xfer_)) {
      case SerialLink::Kind::Xfer:
        if (xfer_.lineReady()) {
          serviceXfer(t);
          hal_->serialFlush();
        }
        break;
      case SerialLink::Kind::Letter:
        letter(link_.letter(), t);
        ++letters;
        break;
      case SerialLink::Kind::ScriptLine:
        scriptLine(link_.line(), t);
        break;
      case SerialLink::Kind::None:
        break;
    }
    if (letters >= 16) break;
  }
  serviceXfer(hal_->millis());   // session and line timeouts
  if (pending_ && now - pendingMs_ > kConfirmMs) {
    pending_ = 0;
    sp("[serial] not confirmed: cancelled\n");
  }
}

void App::printHelp() {
  sp("Pocket Groove %s on the Daisy, serial commands:\n", cfg_.version);
  sp("  r report   h help   l boot log   f one screen frame   m screen mirror on/off\n");
  sp("  p pots on/off (the four pots move the selected track's four macros, with pickup)\n");
  sp("  S save /int to flash now   B then y: Daisy bootloader (flash)   R then y: restart\n");
  sp("  >down KEY  >up KEY  >tap KEY [ms]  >wait MS  >release  >shot NAME  >sync TAG\n");
  sp("      keys as in the desktop's key scripts (host/ui_scripts); KEY by legend:\n");
  sp("      ` 1 .. = del tab q .. \\ fn sft a .. ent ctl opt alt z .. / spc G0\n");
  sp("  >pads [watch [MS|off] | job [drums|notes] | window N | release N | hyst R | xtalk R [MS] | reset]\n");
  sp("  >pad N|all [thr US] [ceil US] [curve X] [on|off]   (the force-sensor pads' calibration)\n");
  sp("  >hit N VEL [hold MS] [press P]  >hit N press P  >hit N up   (a pad from the computer, 0..1)\n");
  sp("  >tap padjob   the pads' job, drums / notes (the desktop's F4)\n");
  sp("  >touch X Y [MS]  >touch map N|auto  >display turn  >display spi 2|4|8|16\n");
  sp("  ~PGX ...  the transfer protocol (tools/pg: ls, pull, push, sample)\n");
  sp("  tools/pg-daisy on the computer drives all of this (keys from its keyboard, the screen)\n");
}

void App::letter(char c, uint32_t now) {
  if (pending_) {
    const char what = pending_;
    pending_ = 0;
    if (c == 'y' || c == 'Y') {
      sp("[serial] %s\n", what == 'B' ? "into the Daisy bootloader: flash now (tools/daisy-flash.sh)" : "restarting");
      persist_.requestNow();
      // Finish a save that is under way (a restart in the middle would only lose this
      // one: the previous snapshot stays valid), within a few seconds.
      const uint32_t t0 = hal_->millis();
      while (persist_.dirty() && hal_->millis() - t0 < 5000) persist_.step(hal_->millis());
      for (int i = 0; i < 50; ++i) hal_->serialFlush();
      hal_->restart(what == 'B');
      return;
    }
    sp("[serial] cancelled\n");
  }
  switch (c) {
    case 'r':
      printReport();
      break;
    case 'h':
    case '?':
      printHelp();
      break;
    case 'l':
      sp("[serial] boot log (%u bytes):\n", static_cast<unsigned>(bootLogLen_));
      hal_->serialWrite(bootLog_, bootLogLen_);
      break;
    case 'f':
      fullFrameWanted_ = true;
      std::snprintf(shotName_, sizeof(shotName_), "frame");
      break;
    case 'm':
      mirror_on_ = !mirror_on_;
      fullFrameWanted_ = mirror_on_;
      std::snprintf(shotName_, sizeof(shotName_), "mirror");
      sp("[screen] mirror %s\n", mirror_on_ ? "on" : "off");
      break;
    case 'p':
      pots_.setEnabled(!pots_.enabled());
      sp("[pots] %s\n", pots_.enabled() ? "on: the four pots move the selected track's macros (pickup)" : "off");
      break;
    case 'S':
      persist_.requestNow();
      sp("[fs] save to flash requested (%s)\n", persist_.dirty() ? "changes pending" : "nothing changed");
      break;
    case 'B':
    case 'R':
      pending_ = c;
      pendingMs_ = now;
      sp("[serial] %s: send y within 3 s to confirm (anything else cancels)\n",
         c == 'B' ? "restart into the Daisy bootloader (for flashing)" : "restart");
      break;
    default:
      break;
  }
}

void App::scriptLine(const char* line, uint32_t now) {
  const KeyScript::Parse r = script_.parse(line);
  switch (r) {
    case KeyScript::Parse::Queued:
    case KeyScript::Parse::Skipped:
      return;
    case KeyScript::Parse::UnknownKey:
      sp("[key] unknown key in \"%s\"\n", line);
      return;
    case KeyScript::Parse::BadNumber:
      sp("[key] bad time in \"%s\"\n", line);
      return;
    case KeyScript::Parse::Full:
      sp("[key] queue full (%d commands): \"%s\" dropped\n", KeyScript::kQueue, line);
      return;
    case KeyScript::Parse::Other:
      break;
  }
  const char* v = script_.verb();
  if (std::strcmp(v, "release") == 0) {
    script_.releaseAll(now, emitKey, this);
  } else if (std::strcmp(v, "shot") == 0) {
    fullFrameWanted_ = true;
    std::snprintf(shotName_, sizeof(shotName_), "%s", script_.arg()[0] ? script_.arg() : "shot");
  } else if (std::strcmp(v, "expect-released") == 0) {
    const bool ok = script_.held() == 0 && (ui_.heldKeys() & kMatrixMask) == 0;
    sp("[key] expect-released: %s\n", ok ? "OK" : "FAIL (keys still held)");
  } else if (std::strcmp(v, "mirror") == 0) {
    mirror_on_ = std::strcmp(script_.arg(), "off") != 0;
    fullFrameWanted_ = mirror_on_;
  } else if (std::strcmp(v, "sync") == 0) {
    // The client waits for this line: everything queued before it has run.
    sp("[key] sync %s\n", script_.arg());
  } else if (settingsCommand(line)) {
    return;
  } else {
    sp("[key] unknown command \"%s\"\n", v);
  }
}

// ---- UI and screen ---------------------------------------------------------------------

void App::serviceUi(uint32_t now) {
  const uint32_t t0 = hal_->micros();
  pg::ui::UiEnv env;
  env.nowMs = now;
  env.batteryPct = hal_->batteryPercent();
  env.sdPresent = fs_.mounted(pg::io::kMountSd);
  const uint32_t pm = meter_.nowPermille();
  env.cpuPct = static_cast<uint8_t>(pm >= 1000 ? 100 : (pm + 5) / 10);
  const bool changed = ui_.update(env, fb_);
  if (changed) {
    int y0 = 0, y1 = 0;
    ui_.dirtyRows(&y0, &y1);
    if (hal_->displayPresent()) hal_->present(fb_, y0, y1 + 1);
    if (dirtyY1_ <= dirtyY0_) {
      dirtyY0_ = y0;
      dirtyY1_ = y1 + 1;
    } else {
      if (y0 < dirtyY0_) dirtyY0_ = y0;
      if (y1 + 1 > dirtyY1_) dirtyY1_ = y1 + 1;
    }
  }
  const uint32_t us = hal_->micros() - t0;
  frameUs_ = frameUs_ ? frameUs_ + ((static_cast<int32_t>(us) - static_cast<int32_t>(frameUs_)) / 16) : us;
  if (us > frameMaxUs_) frameMaxUs_ = us;
}

void App::serviceScreen(uint32_t now) {
  (void)now;
  if (!fbs_.active()) {
    if (fullFrameWanted_) {
      if (fbs_.start(&fb_, 0, pg::kScreenH, shotName_)) {
        fullFrameWanted_ = false;
        dirtyY0_ = dirtyY1_ = 0;
      }
    } else if (mirror_on_ && dirtyY1_ > dirtyY0_) {
      if (fbs_.start(&fb_, dirtyY0_, dirtyY1_, "mirror")) dirtyY0_ = dirtyY1_ = 0;
    }
  }
  if (fbs_.active()) {
    // Leave room for replies and log lines: the frame takes what is left over.
    const size_t room = hal_->serialRoom();
    const size_t keep = 2048;
    if (room > keep) {
      fbs_.pump(room - keep, [](void* u, const char* l, size_t n) { static_cast<App*>(u)->hal_->serialWrite(l, n); },
                this);
    }
  }
}

// ---- storage, LED, stats ---------------------------------------------------------------

void App::serviceStore(uint32_t now) {
  // One flash operation per call: the /int snapshot first, then the pool's mirror.
  // While a recording runs the pool's copy waits (its sector erases would stall the
  // main loop, and the slot is copied once, after the commit).
  mirror_.hold(lineMic_.armed());
  if (persist_.step(now)) return;
  mirror_.flushStep();
}

void App::serviceLed(uint32_t now) {
  if (romMissing_) led_ = LedMode::RomMissing;
  else if (!hal_->audioRunning()) led_ = LedMode::NoAudio;
  else if (inst_.engine().isPlaying()) led_ = LedMode::Playing;
  else led_ = LedMode::Running;
  hal_->setLed(ledOn(led_, now));
}

void App::serviceStats(uint32_t now) {
  (void)now;
  printStatsLine();
}

void App::servicePots(uint32_t now) {
  (void)now;
  float raw[PotMacros::kPots];
  if (!hal_->readPots(raw)) return;
  pots_.update(
      raw, ui_.selTrack(),
      [](void* u, int m) {
        App* a = static_cast<App*>(u);
        const int t = a->ui_.selTrack();
        return a->ui_.instrumentOf(t) ? a->ui_.macroValue(t, m) : -1;
      },
      [](void* u, int m, int v) {
        App* a = static_cast<App*>(u);
        a->ui_.setMacro(a->ui_.selTrack(), m, v);
      },
      this);
}

void App::printStatsLine() {
  const uint32_t pm = meter_.nowPermille();
  sp("[stats] t=%lus aud %d.%d%% max %luus/%luus late %lu+%lu q %lu ui %luus%s%s\n",
     static_cast<unsigned long>(hal_->millis() / 1000), pct10(pm) / 10, pct10(pm) % 10,
     static_cast<unsigned long>(meter_.bootMaxUs()), static_cast<unsigned long>(meter_.periodUs()),
     static_cast<unsigned long>(meter_.overruns()), static_cast<unsigned long>(meter_.lateStarts()),
     static_cast<unsigned long>(inst_.engine().queuePeak()), static_cast<unsigned long>(frameUs_),
     inst_.engine().isPlaying() ? " playing" : "", persist_.busy() ? " saving" : "");
}

void App::printReport() {
  const uint32_t nowMs = hal_->millis();
  const uint32_t winMs = nowMs - lastReportMs_;
  lastReportMs_ = nowMs;
  const LoadMeter::Window w = meter_.takeWindow();
  sp("\n===== POCKET GROOVE REPORT BEGIN %s =====\n", cfg_.version);
  sp("[sys] uptime %lus, platform daisy, %s, board %s\n", static_cast<unsigned long>(nowMs / 1000), hal_->stageName(),
     hal_->boardName());
  sp("[sys] window: %lu.%lu s since the previous report (peaks below are over it)\n",
     static_cast<unsigned long>(winMs / 1000), static_cast<unsigned long>(winMs % 1000 / 100));
  hal_->reportLines([](void* u, const char* l) { static_cast<App*>(u)->sp("%s\n", l); }, this);
  sp("[ram] static objects: app %lu B (instrument %lu, ui %lu, framebuffer %lu), engine state %lu\n",
     static_cast<unsigned long>(sizeof(App)), static_cast<unsigned long>(sizeof(pg::Instrument)),
     static_cast<unsigned long>(sizeof(pg::ui::Ui)), static_cast<unsigned long>(sizeof(pg::Framebuffer)),
     static_cast<unsigned long>(pg::Instrument::staticRamBytes() - sizeof(pg::Instrument)));
  {
    const uint32_t rate = hal_->sampleRate(), bf = hal_->blockFrames();
    const uint32_t blockUs = rate ? bf * 1000000u / rate : 0;
    sp("[audio] %s, %lu Hz, block %lu frames (%lu.%02lu ms), stereo, output %+d dB (digital)\n",
       hal_->audioRunning() ? "RUNNING" : "NOT RUNNING", static_cast<unsigned long>(rate),
       static_cast<unsigned long>(bf), static_cast<unsigned long>(blockUs / 1000),
       static_cast<unsigned long>(blockUs % 1000 / 10), static_cast<int>(lrintf(gainDb_)));
  }
  {
    const uint32_t now = meter_.nowPermille();
    const uint32_t opk = takeOutputPeak();
    sp("[audio] window: AUD now %lu.%lu%% peak %lu.%lu%%, longest block %luus of %luus, blocks %lu, late %lu; "
       "output peak %lu (%d dBFS)\n",
       static_cast<unsigned long>(now / 10), static_cast<unsigned long>(now % 10),
       static_cast<unsigned long>(w.peakPermille / 10), static_cast<unsigned long>(w.peakPermille % 10),
       static_cast<unsigned long>(w.maxUs), static_cast<unsigned long>(meter_.periodUs()),
       static_cast<unsigned long>(w.blocks), static_cast<unsigned long>(w.late), static_cast<unsigned long>(opk),
       opk ? static_cast<int>(lrintf(20.0f * log10f(static_cast<float>(opk) / 32767.0f))) : -99);
    sp("[audio] since boot: longest block %luus, blocks %lu, underruns (block longer than its period) %lu, "
       "late starts (> 1.5 periods apart) %lu\n",
       static_cast<unsigned long>(meter_.bootMaxUs()), static_cast<unsigned long>(meter_.blocks()),
       static_cast<unsigned long>(meter_.overruns()), static_cast<unsigned long>(meter_.lateStarts()));
  }
  sp("[engine] command queue peak %lu of 256, UI dropped commands %lu, playing %s\n",
     static_cast<unsigned long>(inst_.engine().queuePeak()), static_cast<unsigned long>(ui_.droppedCommands()),
     inst_.engine().isPlaying() ? "yes" : "no");
  sp("[main] loop passes %lu, longest pass %luus over the window; tasks:\n", static_cast<unsigned long>(sched_.passes()),
     static_cast<unsigned long>(sched_.maxPassUs()));
  for (int i = 0; i < sched_.count(); ++i) {
    const Scheduler::TaskStats& t = sched_.stats(i);
    const uint32_t avg = t.windowRuns ? static_cast<uint32_t>(t.totalUs / t.windowRuns) : 0;
    const uint32_t busy10 = winMs ? static_cast<uint32_t>(t.totalUs / winMs) : 0;   // permille of the window
    sp("[task] %-7s every %4lu ms, runs %8lu, avg %5luus max %6luus, %lu.%lu%% of the core\n", t.name,
       static_cast<unsigned long>(t.periodMs), static_cast<unsigned long>(t.windowRuns), static_cast<unsigned long>(avg),
       static_cast<unsigned long>(t.maxUs), static_cast<unsigned long>(busy10 / 10),
       static_cast<unsigned long>(busy10 % 10));
  }
  sched_.takeWindow();
  sp("[keys] from the computer %lu (script queue %d, dropped %lu), physical %lu, held by script 0x%llx\n",
     static_cast<unsigned long>(injected_), script_.queued(), static_cast<unsigned long>(script_.dropped()),
     static_cast<unsigned long>(physical_), static_cast<unsigned long long>(script_.held()));
  sp("[ui] frame avg %luus max %luus, full redraws %lu, status redraws %lu, view %d, project \"%s\"\n",
     static_cast<unsigned long>(frameUs_), static_cast<unsigned long>(frameMaxUs_),
     static_cast<unsigned long>(ui_.fullRedraws()), static_cast<unsigned long>(ui_.statusRedraws()),
     static_cast<int>(ui_.view()), ui_.projectName());
  frameMaxUs_ = 0;
  {
    const FsPersist::Stats& ps = persist_.stats();
    sp("[fs] /int (RAM, flash snapshots): %d files, %lu B, snapshot %lu of %lu B; bank %c seq %lu, saves %lu, "
       "failures %lu%s%s, %s\n",
       ramfs_.fileCount(), static_cast<unsigned long>(ramfs_.usedBytes()),
       static_cast<unsigned long>(ramfs_.serializedSize()), static_cast<unsigned long>(ramfs_.budget()),
       ps.bank < 0 ? '-' : static_cast<char>('A' + ps.bank), static_cast<unsigned long>(ps.seq),
       static_cast<unsigned long>(ps.saves), static_cast<unsigned long>(ps.failures),
       ps.lastError[0] ? ", last error: " : "", ps.lastError,
       persist_.busy() ? "SAVING" : persist_.dirty() ? "changes not yet on flash" : "on flash");
    sp("[fs] boot load: %s; last save %lu B in %lu ms; /sd %s\n",
       fsLoad_ == FsPersist::Load::Loaded ? "loaded" : fsLoad_ == FsPersist::Load::Corrupt ? "CORRUPT" : "empty",
       static_cast<unsigned long>(ps.lastBytes), static_cast<unsigned long>(ps.lastDurationMs),
       fs_.mounted(pg::io::kMountSd) ? "mounted" : "none");
  }
  {
    int used = 0;
    for (int i = 0; i < pg::sampler::kPoolSlots; ++i) used += pool_.header(i) ? 1 : 0;
    const MirrorFlash::Stats& ms = mirror_.stats();
    sp("[samples] pool in RAM %s, %d of %d slots hold a sample; flash copy: %d sectors to write, %lu written, "
       "%lu failed; %d bad slot(s) emptied at boot\n",
       mem_.poolImage ? (poolLoaded_ ? "(loaded at boot)" : "(flash unreadable at boot)") : "NONE", used,
       pg::sampler::kPoolSlots, mirror_.dirtySectors(), static_cast<unsigned long>(ms.flushed),
       static_cast<unsigned long>(ms.flushFailures), poolDropped_);
    for (int i = 0; i < pg::sampler::kPoolSlots; ++i) {
      const pg::sampler::SlotHeader* sh = pool_.header(i);
      if (!sh) continue;
      const uint32_t cs = sh->rate ? sh->samples * 100u / sh->rate : 0;
      sp("[samples] slot %2d %-8.8s %6lu samples %lu.%02lu s, peak %u, crc %08lx\n", i + 1, sh->name,
         static_cast<unsigned long>(sh->samples), static_cast<unsigned long>(cs / 100),
         static_cast<unsigned long>(cs % 100), static_cast<unsigned>(sh->peak), static_cast<unsigned long>(sh->dataCrc));
    }
  }
  {
    const pg::io::Xfer::Stats& xs = xfer_.stats();
    sp("[xfer] lines %lu, gets %lu, puts %lu, errors %lu, timeouts %lu, bad crc %lu, bad lines %lu, overflows %lu\n",
       static_cast<unsigned long>(xs.lines), static_cast<unsigned long>(xs.gets), static_cast<unsigned long>(xs.puts),
       static_cast<unsigned long>(xs.errors), static_cast<unsigned long>(xs.timeouts),
       static_cast<unsigned long>(xs.badCrc), static_cast<unsigned long>(xs.badLines),
       static_cast<unsigned long>(xs.overflows));
  }
  {
    float raw[PotMacros::kPots];
    if (hal_->readPots(raw)) {
      sp("[pots] %s, macro moves %lu; positions %d %d %d %d (0..255), engaged %d%d%d%d\n",
         pots_.enabled() ? "on" : "off", static_cast<unsigned long>(pots_.moves()), static_cast<int>(raw[0] * 255.0f),
         static_cast<int>(raw[1] * 255.0f), static_cast<int>(raw[2] * 255.0f), static_cast<int>(raw[3] * 255.0f),
         pots_.engaged(0), pots_.engaged(1), pots_.engaged(2), pots_.engaged(3));
    } else {
      sp("[pots] none\n");
    }
  }
  printPads(true);
  printTouch();
  printLineIn();
  sp("[led] %s; screen mirror %s, frames sent %lu; script lines %lu (overlong %lu)\n", ledModeName(led_),
     mirror_on_ ? "on" : "off", static_cast<unsigned long>(fbs_.framesSent()),
     static_cast<unsigned long>(link_.scriptLines()), static_cast<unsigned long>(link_.overlongLines()));
  sp("===== POCKET GROOVE REPORT END =====\n");
}

}  // namespace pgd
