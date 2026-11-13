#include "lab.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "keymap.h"
#include "pg/ui/font.h"
#include "pg/ui/gfx.h"

namespace desk {

using namespace pg::ui;
namespace pm = pg::macro;

namespace {

// ---- panel geometry (device pixels inside the kLabW x kCanvasH panel) ------------------
constexpr int W = kLabW, H = kCanvasH;
constexpr int kSetY = 11, kSetH = 11, kPrevX = 2, kNextX = W - 15, kArrowW = 13;
constexpr int kPathY = 24, kStatusY = 33;
constexpr int kSlY0 = 52, kSlH = 12, kBarX = 42, kBarW = 104, kBarH = 7;
constexpr int kPadX = 4, kPadY = 101, kPadS = 66;
constexpr int kAxisY = 171, kAxisXX = 3, kAxisYX = 47, kAxisW = 43;
constexpr int kColX = 92, kColW = W - kColX - 2, kReadY = 101, kReadRows = 7;
constexpr int kMarkY = 158, kGoodX = kColX, kBadX = kColX + 46, kMarkW = 44, kMarkH = 11;
constexpr int kNoteY = 173;

// A tiny drawing target over the panel buffer (pg::ui::Gfx is fixed to 240x135).
struct Canvas {
  uint8_t* px;
  void pixel(int x, int y, uint8_t c) {
    if (unsigned(x) < unsigned(W) && unsigned(y) < unsigned(H)) px[y * W + x] = c;
  }
  void rect(int x, int y, int w, int h, uint8_t c) {
    for (int j = y; j < y + h; ++j)
      for (int i = x; i < x + w; ++i) pixel(i, j, c);
  }
  void frame(int x, int y, int w, int h, uint8_t c) {
    rect(x, y, w, 1, c);
    rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c);
    rect(x + w - 1, y, 1, h, c);
  }
  void rrect(int x, int y, int w, int h, uint8_t c) {
    rect(x + 1, y, w - 2, h, c);
    rect(x, y + 1, 1, h - 2, c);
    rect(x + w - 1, y + 1, 1, h - 2, c);
  }
  int text(int x, int y, const char* s, uint8_t c) {
    for (; *s; ++s) {
      const uint8_t* cols = glyph(uint8_t(*s));
      for (int cx = 0; cx < 5; ++cx) {
        uint8_t b = cols[cx];
        for (int cy = 0; b; ++cy, b >>= 1)
          if (b & 1) pixel(x + cx, y + cy, c);
      }
      x += 6;
    }
    return x;
  }
  void textC(int cx, int y, const char* s, uint8_t c) { text(cx - tw(s) / 2, y, s, c); }
  void textR(int xr, int y, const char* s, uint8_t c) { text(xr - tw(s) + 1, y, s, c); }
  void button(int x, int y, int w, int h, const char* s, bool on, uint8_t col) {
    rrect(x, y, w, h, on ? col : uint8_t(RAISED));
    textC(x + w / 2, y + (h - 7) / 2 + 1, s, on ? uint8_t(BG) : uint8_t(TEXT));
  }
};

std::string upperStr(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return s;
}

// at most n characters; a longer string keeps its end (paths) or its start (messages)
std::string fitEnd(const std::string& s, size_t n) { return s.size() <= n ? s : ".." + s.substr(s.size() - (n - 2)); }
std::string fitStart(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n); }

std::string baseName(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

uint32_t fnv(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

bool readFile(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  char b[4096];
  size_t n;
  while ((n = std::fread(b, 1, sizeof(b), f)) > 0) out->append(b, n);
  std::fclose(f);
  return true;
}

bool statFile(const std::string& path, int64_t* mtimeNs, int64_t* size, bool* isDir) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
#ifdef __APPLE__
  *mtimeNs = int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
  *mtimeNs = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
  *size = int64_t(st.st_size);
  *isDir = S_ISDIR(st.st_mode);
  return true;
}

std::string posText(float p) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.2f", double(p));
  return b;
}

std::string valueText(const pg::ParamInfo* in, int v) {
  if (in && in->fmt == pg::ParamFormat::Enum && in->enumNames && v >= in->min && v <= in->max &&
      in->enumNames[v - in->min])
    return in->enumNames[v - in->min];
  return std::to_string(v);
}

}  // namespace

// ============================================================================ files ====

void Lab::init(const std::string& dir, const std::string& notesPath, const pm::ParamTable& pt, LabHost* host,
               uint32_t nowMs) {
  dir_ = dir;
  while (dir_.size() > 1 && dir_.back() == '/') dir_.pop_back();
  notesPath_ = notesPath;
  pt_ = pt;
  host_ = host;
  now_ = nowMs;
  scan(nowMs, true);
  int good = 0;
  for (const File& f : files_) good += f.good;
  std::printf("[lab] watching %s/: %d set(s), %d with errors; marks go to %s\n", dir_.c_str(), good, errors(),
              notesPath_.c_str());
  std::fflush(stdout);
  nextPoll_ = nowMs + kPollMs;
}

Lab::File* Lab::find(const std::string& path) {
  for (File& f : files_)
    if (f.path == path) return &f;
  return nullptr;
}

int Lab::errors() const {
  int n = 0;
  for (const File& f : files_) n += !f.err.empty();
  return n;
}

int Lab::warnings() const {
  int n = 0;
  for (const File& f : files_) n += f.good ? f.warnCount : 0;
  return n;
}

void Lab::scanDir(const std::string& dir, int depth) {
  if (depth > 6) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;   // also editors' hidden swap files
    names.push_back(n);
  }
  closedir(d);
  for (const std::string& n : names) {
    const std::string p = dir + "/" + n;
    int64_t mt = 0, sz = 0;
    bool isDir = false;
    if (!statFile(p, &mt, &sz, &isDir)) continue;
    if (isDir) scanDir(p, depth + 1);
    else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0) found_.push_back(p);
  }
}

void Lab::scan(uint32_t nowMs, bool first) {
  (void)nowMs;
  found_.clear();
  scanDir(dir_, 0);
  for (File& f : files_) f.seen = false;
  const int64_t wallNs = int64_t(std::time(nullptr)) * 1000000000;
  for (const std::string& p : found_) {
    int64_t mt = 0, sz = 0;
    bool isDir = false;
    if (!statFile(p, &mt, &sz, &isDir)) continue;
    File* f = find(p);
    if (!f) {
      files_.push_back(File());
      f = &files_.back();
      f->path = p;
      if (!first) std::printf("[lab] new file %s\n", p.c_str());
    }
    f->seen = true;
    // Changed size or time; or written in the last 2 s (a second write within the file
    // system's time resolution): read it, and act only if the content differs.
    if (mt != f->mtimeNs || sz != f->size || wallNs - mt < 2000000000LL) {
      f->mtimeNs = mt;
      f->size = sz;
      load(*f, first);
    }
  }
  for (size_t i = 0; i < files_.size();) {
    if (files_[i].seen) { ++i; continue; }
    const std::string p = files_[i].path;
    for (int t = 0; t < pg::kNumTracks; ++t)
      if (tr_[t].path == p) deactivate(t);
    std::printf("[lab] removed %s\n", p.c_str());
    setStatus("REMOVED " + upperStr(baseName(p)), AMBER, 2500);
    files_.erase(files_.begin() + long(i));
    ++reloads_;
  }
  std::sort(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.path < b.path; });
  std::fflush(stdout);
  dirty_ = true;
}

void Lab::load(File& f, bool first) {
  std::string text;
  if (!readFile(f.path, &text)) return;   // vanished between stat and read: the next poll sees it
  const uint32_t h = fnv(text);
  if (h == f.hash && (f.good || !f.err.empty())) return;   // same content as last time
  f.hash = h;
  static pm::MacroSet fresh;   // 1.9 KB; the main thread only
  pm::ParseError e;
  pm::Warnings w;
  if (!pm::parse(text.data(), text.size(), pt_, &fresh, &e, &w)) {
    char b[160];
    std::snprintf(b, sizeof(b), "%d: %s", e.line, e.msg);
    f.err = b;
    std::printf("[lab] %s:%s (%s)\n", f.path.c_str(), b,
                f.good ? "keeping the last good version" : "not loaded");
    dirty_ = true;
    return;
  }
  const bool had = f.good;
  for (int t = 0; t < pg::kNumTracks; ++t)   // the file now names another voice: give this one back
    if (tr_[t].path == f.path && fresh.track != t) deactivate(t);   // (saves positions under the old names)
  const pm::MacroSet old = f.set;
  f.set = fresh;
  f.goodHash = h;
  f.good = true;
  f.err.clear();
  f.warns.clear();
  f.warnCount = w.count;
  for (int k = 0; k < w.count && k < pm::kMaxWarnings; ++k) {
    char b[160];
    std::snprintf(b, sizeof(b), "%d: %s", w.w[k].line, w.w[k].msg);
    f.warns.push_back(b);
    std::printf("[lab] %s:%s (warning)\n", f.path.c_str(), b);
  }
  if (w.count > pm::kMaxWarnings)
    std::printf("[lab] %s: %d more warning(s)\n", f.path.c_str(), w.count - pm::kMaxWarnings);
  f.label =f.set.name[0] ? std::string(f.set.name) : upperStr(baseName(f.path).substr(0, baseName(f.path).size() - 4));
  for (int t = 0; t < pg::kNumTracks; ++t)
    if (tr_[t].path == f.path) reconcile(t, old, had);
  if (!first) {
    std::printf("[lab] %s %s (%s, %s, %d macro%s)\n", had ? "reloaded" : "loaded", f.path.c_str(), f.label.c_str(),
                pm::trackName(f.set.track), f.set.numMacros, f.set.numMacros == 1 ? "" : "s");
    setStatus((had ? "RELOADED " : "LOADED ") + upperStr(baseName(f.path)), GREEN, 2500);
    ++reloads_;
  }
  dirty_ = true;
}

std::vector<int> Lab::setsFor(int track) const {
  std::vector<int> v;
  for (size_t i = 0; i < files_.size(); ++i)
    if (files_[i].good && files_[i].set.track == track) v.push_back(int(i));
  return v;
}

// ======================================================================= applying ====

void Lab::savePositions(int track) {
  Track& t = tr_[track];
  File* f = find(t.path);
  if (!f || !f->good) return;
  Saved& s = saved_[t.path];
  s.n = f->set.numMacros;
  for (int m = 0; m < s.n; ++m) {
    s.names[m] = f->set.macros[m].name;
    s.pos[m] = t.pos[m];
  }
}

void Lab::activate(int track, int idx) {
  Track& t = tr_[track];
  const std::string path = idx >= 0 ? files_[size_t(idx)].path : std::string();
  if (path == t.path) return;
  deactivate(track);
  if (idx < 0) {
    std::printf("[lab] %s: none (bare parameters, back to the values from before)\n", pm::trackName(track));
    std::fflush(stdout);
    return;
  }
  const pm::MacroSet& set = files_[size_t(idx)].set;
  t.path = path;
  for (int p = 0; p < pg::kParamsPerTrack; ++p) t.under[p] = uint8_t(host_->paramValue(track, p));
  const auto it = saved_.find(path);
  for (int m = 0; m < set.numMacros; ++m) {
    t.pos[m] = set.macros[m].start();
    if (it != saved_.end())
      for (int k = 0; k < it->second.n; ++k)
        if (it->second.names[k] == set.macros[m].name) t.pos[m] = it->second.pos[k];
  }
  t.owned = set.ownedMask();
  t.sentMask = 0;
  t.force = true;
  t.focus = 0;
  t.axisX = 0;
  t.axisY = set.numMacros > 1 ? 1 : 0;
  std::printf("[lab] %s: %s (%s)\n", pm::trackName(track), files_[size_t(idx)].label.c_str(), path.c_str());
  std::fflush(stdout);
  dirty_ = true;
}

void Lab::deactivate(int track) {
  Track& t = tr_[track];
  if (t.path.empty()) return;
  savePositions(track);
  for (int p = 0; p < pg::kParamsPerTrack; ++p)
    if ((t.owned >> p) & 1) host_->setParam(track, p, t.under[p]);
  t.path.clear();
  t.owned = 0;
  t.sentMask = 0;
  dirty_ = true;
}

// The active set's file was reloaded: keep the positions of macros that kept their name,
// give back parameters it no longer owns, and send everything once.
void Lab::reconcile(int track, const pm::MacroSet& old, bool hadOld) {
  Track& t = tr_[track];
  const File* f = find(t.path);
  if (!f) return;
  const pm::MacroSet& set = f->set;
  float pos[pm::kMaxMacros];
  for (int m = 0; m < set.numMacros; ++m) {
    pos[m] = set.macros[m].start();
    if (hadOld) {
      const int k = old.findMacro(set.macros[m].name);
      if (k >= 0) pos[m] = t.pos[k];
    }
  }
  std::memcpy(t.pos, pos, sizeof(pos));
  const uint32_t owned = set.ownedMask();
  for (int p = 0; p < pg::kParamsPerTrack; ++p) {
    const uint32_t b = 1u << p;
    if ((t.owned & b) && !(owned & b)) host_->setParam(track, p, t.under[p]);   // given back
    if (!(t.owned & b) && (owned & b)) t.under[p] = uint8_t(host_->paramValue(track, p));   // newly owned
  }
  t.owned = owned;
  t.force = true;
  if (t.focus >= set.numMacros) t.focus = 0;
  if (t.axisX >= set.numMacros) t.axisX = 0;
  if (t.axisY >= set.numMacros) t.axisY = set.numMacros > 1 ? 1 : 0;
}

void Lab::apply(int track) {
  Track& t = tr_[track];
  if (t.path.empty()) return;
  const File* f = find(t.path);
  if (!f || !f->good) return;
  uint8_t out[pg::kParamsPerTrack];
  pm::evaluate(f->set, pt_, t.pos, t.under, out);
  for (int p = 0; p < pg::kParamsPerTrack; ++p) {
    const uint32_t b = 1u << p;
    if (!(t.owned & b)) continue;
    if (t.force || !(t.sentMask & b) || out[p] != t.sent[p]) {
      host_->setParam(track, p, out[p]);
      t.sent[p] = out[p];
      t.sentMask |= b;
    }
  }
  t.force = false;
}

void Lab::tick(uint32_t nowMs) {
  if (!host_) return;
  now_ = nowMs;
  if (int32_t(nowMs - nextPoll_) >= 0) {
    scan(nowMs, false);
    nextPoll_ = nowMs + kPollMs;
  }
  for (int t = 0; t < pg::kNumTracks; ++t) apply(t);
  if (statusUntil_ && int32_t(nowMs - statusUntil_) >= 0) {
    statusUntil_ = 0;
    status_.clear();
    dirty_ = true;
  }
  // redraw when what the panel shows changed: the track, its values (hand edits, p-locks)
  const int sel = (host_->selectedTrack() & 7);
  uint32_t h = 2166136261u;
  for (int p = 0; p < pg::kParamsPerTrack; ++p) h = (h ^ uint32_t(host_->paramValue(sel, p))) * 16777619u;
  if (sel != lastShownTrack_ || h != lastDrawHash_) {
    lastShownTrack_ = sel;
    lastDrawHash_ = h;
    dirty_ = true;
  }
}

bool Lab::synced() {
  for (int t = 0; t < pg::kNumTracks; ++t) {
    const Track& s = tr_[t];
    if (s.path.empty()) continue;
    if (s.force) return false;
    for (int p = 0; p < pg::kParamsPerTrack; ++p)
      if (((s.owned >> p) & 1) && (!((s.sentMask >> p) & 1) || host_->paramValue(t, p) != s.sent[p])) return false;
  }
  return true;
}

// ======================================================================= controls ====

bool Lab::selectSet(int track, const std::string& which) {
  if (track < 0 || track >= pg::kNumTracks) return false;
  if (which == "none") {
    activate(track, -1);
    return true;
  }
  for (int i : setsFor(track)) {
    const File& f = files_[size_t(i)];
    const std::string fn = baseName(f.path);
    if (upperStr(which) == f.label || which == f.path || which == fn || which + ".pgm" == fn) {
      activate(track, i);
      return true;
    }
  }
  return false;
}

bool Lab::setMacro(int track, int m, float pos) {
  if (track < 0 || track >= pg::kNumTracks) return false;
  const File* f = find(tr_[track].path);
  if (!f || m < 0 || m >= f->set.numMacros) return false;
  tr_[track].pos[m] = pos < 0.0f ? 0.0f : pos > 1.0f ? 1.0f : pos;
  dirty_ = true;
  return true;
}

void Lab::step(int dir) {
  const int track = (host_->selectedTrack() & 7);
  const std::vector<int> sets = setsFor(track);
  // positions: none, then the sets in path order
  int cur = 0;
  for (size_t i = 0; i < sets.size(); ++i)
    if (files_[size_t(sets[i])].path == tr_[track].path) cur = int(i) + 1;
  const int n = int(sets.size()) + 1;
  const int next = ((cur + dir) % n + n) % n;
  activate(track, next == 0 ? -1 : sets[size_t(next - 1)]);
  dirty_ = true;
}

void Lab::nudge(float d) {
  const int track = (host_->selectedTrack() & 7);
  Track& t = tr_[track];
  setMacro(track, t.focus, t.pos[t.focus] + d);
}

void Lab::action(int act, bool shift) {
  if (!host_) return;
  const int track = (host_->selectedTrack() & 7);
  const File* f = find(tr_[track].path);
  const int nm = f ? f->set.numMacros : 0;
  if (act == kActLabGood || act == kActLabBad) mark(act == kActLabGood);
  else if (act == kActLabPrev) step(-1);
  else if (act == kActLabNext) step(+1);
  else if (act == kActLabUp) nudge(shift ? 0.01f : 0.05f);
  else if (act == kActLabDown) nudge(shift ? -0.01f : -0.05f);
  else if (act >= kActLabMacro1 && act < kActLabMacro1 + pm::kMaxMacros && act - kActLabMacro1 < nm)
    tr_[track].focus = act - kActLabMacro1;
  dirty_ = true;
}

void Lab::mouseDown(int x, int y) {
  if (!host_) return;
  const int track = (host_->selectedTrack() & 7);
  Track& t = tr_[track];
  const File* f = find(t.path);
  const int nm = f ? f->set.numMacros : 0;
  auto in = [&](int x0, int y0, int w, int h) { return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h; };
  if (in(kPrevX, kSetY, kArrowW, kSetH)) return step(-1);
  if (in(kNextX, kSetY, kArrowW, kSetH)) return step(+1);
  if (in(kGoodX, kMarkY, kMarkW, kMarkH)) return mark(true);
  if (in(kBadX, kMarkY, kMarkW, kMarkH)) return mark(false);
  if (nm == 0) return;
  if (in(kAxisXX, kAxisY, kAxisW, 9)) { t.axisX = (t.axisX + 1) % nm; dirty_ = true; return; }
  if (in(kAxisYX, kAxisY, kAxisW, 9)) { t.axisY = (t.axisY + 1) % nm; dirty_ = true; return; }
  for (int m = 0; m < nm; ++m)
    if (in(0, kSlY0 + m * kSlH, W, kSlH)) {
      drag_ = kDragSlider;
      dragSlider_ = m;
      t.focus = m;
      return mouseMove(x, y);
    }
  if (in(kPadX - 2, kPadY - 2, kPadS + 4, kPadS + 4)) {
    drag_ = kDragPad;
    return mouseMove(x, y);
  }
}

void Lab::mouseMove(int x, int y) {
  if (drag_ == kDragNone || !host_) return;
  const int track = (host_->selectedTrack() & 7);
  Track& t = tr_[track];
  if (drag_ == kDragSlider) {
    setMacro(track, dragSlider_, float(x - kBarX) / float(kBarW - 1));
  } else {
    setMacro(track, t.axisX, float(x - kPadX) / float(kPadS - 1));
    if (t.axisY != t.axisX) setMacro(track, t.axisY, 1.0f - float(y - kPadY) / float(kPadS - 1));
  }
}

void Lab::mouseUp() { drag_ = kDragNone; }

void Lab::sliderPoint(int m, float pos, int* x, int* y) const {
  *x = kBarX + int(pos * float(kBarW - 1) + 0.5f);
  *y = kSlY0 + m * kSlH + kSlH / 2;
}

void Lab::padPoint(float px, float py, int* x, int* y) const {
  *x = kPadX + int(px * float(kPadS - 1) + 0.5f);
  *y = kPadY + int((1.0f - py) * float(kPadS - 1) + 0.5f);
}

void Lab::buttonCentre(Button b, int* x, int* y) const {
  switch (b) {
    case kBtnPrev: *x = kPrevX + kArrowW / 2; *y = kSetY + kSetH / 2; break;
    case kBtnNext: *x = kNextX + kArrowW / 2; *y = kSetY + kSetH / 2; break;
    case kBtnGood: *x = kGoodX + kMarkW / 2; *y = kMarkY + kMarkH / 2; break;
    case kBtnBad: *x = kBadX + kMarkW / 2; *y = kMarkY + kMarkH / 2; break;
    case kBtnAxisX: *x = kAxisXX + kAxisW / 2; *y = kAxisY + 4; break;
    case kBtnAxisY: *x = kAxisYX + kAxisW / 2; *y = kAxisY + 4; break;
  }
}

void Lab::setStatus(const std::string& s, uint8_t colour, uint32_t ms) {
  status_ = s;
  statusColour_ = colour;
  statusUntil_ = (now_ + ms) | 1u;
  dirty_ = true;
}

// ========================================================================== marks ====

// "SHORT=value" for the parameters the set owns (all sound parameters with no set),
// in index order; with flagHand a '*' marks a value that differs from what the set
// computes (edited by hand since).
std::string Lab::valuesText(int track, bool flagHand) {
  const Track& t = tr_[track];
  const File* f = find(t.path);
  uint8_t out[pg::kParamsPerTrack];
  uint32_t owned = 0;
  if (f && f->good) owned = pm::evaluate(f->set, pt_, t.pos, t.under, out);
  const uint32_t locked = host_->lockedMask(track);
  std::string s;
  for (int p = 0; p < pg::kParamsPerTrack; ++p) {
    const pg::ParamInfo* in = pt_.info[track][p];
    if (!in) continue;
    if (owned ? !((owned >> p) & 1) : p >= pg::kSoundParams) continue;
    const int v = host_->paramValue(track, p);
    if (!s.empty()) s += " ";
    s += std::string(in->name) + "=" + valueText(in, v);
    if (flagHand && owned && v != out[p]) s += "*";
    if (flagHand && ((locked >> p) & 1)) s += "~";
  }
  return s;
}

std::string Lab::describe(int track) {
  const Track& t = tr_[track];
  const File* f = find(t.path);
  std::string s = pm::trackName(track);
  if (!f || !f->good) {
    s += " none -";
  } else {
    char h[12];
    std::snprintf(h, sizeof(h), "%08x", f->goodHash);
    s += " " + f->label + " " + f->path + "@" + h;
    for (int m = 0; m < f->set.numMacros; ++m) s += std::string(" ") + f->set.macros[m].name + "=" + posText(t.pos[m]);
  }
  return s + " | " + valuesText(track, true);
}

void Lab::mark(bool good) {
  if (typing_) finishTyping(true);
  char ts[32];
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
  pendingLine_ = std::string(ts) + (good ? " GOOD " : " BAD  ") + describe((host_->selectedTrack() & 7));
  note_.clear();
  typing_ = true;
  dirty_ = true;
}

void Lab::typeText(const char* text) {
  if (!typing_) return;
  for (const char* c = text; *c; ++c)
    if (uint8_t(*c) >= 0x20 && uint8_t(*c) < 0x7F && note_.size() < 160) note_ += *c;
  dirty_ = true;
}

void Lab::typeKey(TypeKey k) {
  if (!typing_) return;
  if (k == kTypeBackspace) {
    if (!note_.empty()) note_.pop_back();
    dirty_ = true;
  } else {
    finishTyping(k == kTypeEnter);
  }
}

void Lab::finishTyping(bool withNote) {
  if (!typing_) return;
  typing_ = false;
  std::string line = pendingLine_;
  if (withNote && !note_.empty()) line += " | " + note_;
  FILE* f = std::fopen(notesPath_.c_str(), "a");
  if (f) {
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
    std::printf("[lab] marked: %s\n", line.c_str());
    setStatus(std::string(line.compare(20, 4, "GOOD") == 0 ? "GOOD" : "BAD") + " SAVED TO " + upperStr(baseName(notesPath_)),
              GREEN, 2500);
  } else {
    std::printf("[lab] cannot write %s: the mark is lost: %s\n", notesPath_.c_str(), line.c_str());
    setStatus("CANNOT WRITE " + upperStr(notesPath_), RED, 4000);
  }
  std::fflush(stdout);
  dirty_ = true;
}

// ========================================================================== panel ====

void Lab::draw(uint8_t* px) {
  dirty_ = false;
  Canvas g{px};
  g.rect(0, 0, W, H, BG);
  g.frame(0, 0, W, H, CYAN_D);
  if (!host_) return;
  const int track = (host_->selectedTrack() & 7);
  Track& t = tr_[track];
  const File* f = find(t.path);
  const bool active = f && f->good;
  const int nm = active ? f->set.numMacros : 0;

  // header: the tool's name, the track
  g.text(4, 2, "MACRO LAB", CYAN);
  const std::string tn = upperStr(pm::trackName(track));
  g.rrect(W - 4 - tw(tn.c_str()) - 4, 1, tw(tn.c_str()) + 4, 9, tc(track, kBright));
  g.text(W - 4 - tw(tn.c_str()) - 2, 2, tn.c_str(), BG);

  // the set: < NAME n/N >
  const std::vector<int> sets = setsFor(track);
  int cur = 0;
  for (size_t i = 0; i < sets.size(); ++i)
    if (files_[size_t(sets[i])].path == t.path) cur = int(i) + 1;
  g.button(kPrevX, kSetY, kArrowW, kSetH, PG_ICON_LEFT, false, CYAN);
  g.button(kNextX, kSetY, kArrowW, kSetH, PG_ICON_RIGHT, false, CYAN);
  char setText[40];
  if (active) std::snprintf(setText, sizeof(setText), "%s  %d/%d", f->label.c_str(), cur, int(sets.size()));
  else std::snprintf(setText, sizeof(setText), "NONE  0/%d", int(sets.size()));
  g.textC(W / 2, kSetY + 2, setText, active ? WHITE : DIM);
  // under the name: the set's about line ("family: ..."), else its file
  if (active && f->set.about[0]) g.text(4, kPathY, fitStart(upperStr(f->set.about), 29).c_str(), TEXT);
  else
    g.text(4, kPathY, fitEnd(active ? f->path : sets.empty() ? "no set for this track" : "bare parameters", 29).c_str(),
           DIM);

  // status: typing > an error in this set > any error > a passing notice > a warning of this
  // set > p-locks > the keyboard macro's description > the keys
  std::string s1, s2;
  uint8_t sc = DIM;
  const File* errFile = nullptr;
  if (f && !f->err.empty()) errFile = f;
  for (const File& e : files_)
    if (!errFile && !e.err.empty()) errFile = &e;
  if (typing_) {
    s1 = "TYPE A NOTE: RETURN SAVES";
    s2 = "ESC SAVES WITHOUT ONE";
    sc = AMBER;
  } else if (errFile) {
    const std::string m = baseName(errFile->path) + ":" + errFile->err;
    s1 = fitStart(m, 29);
    s2 = m.size() > 29 ? fitStart(m.substr(29), 29) : std::string(errFile->good ? "(last good version kept)" : "(not loaded)");
    sc = RED;
  } else if (!status_.empty()) {
    s1 = fitStart(status_, 29);
    sc = statusColour_;
  } else if (active && !f->warns.empty()) {
    // amber, "line: reason" of the active set (the terminal has every warning in full)
    std::string m = f->warns[0];
    if (f->warnCount > 1) m = "(" + std::to_string(f->warnCount) + ") " + m;
    s1 = fitStart(m, 29);
    s2 = m.size() > 29 ? fitStart(m.substr(29), 29) : std::string();
    sc = AMBER;
  } else if (active && (host_->lockedMask(track) & t.owned)) {
    s1 = "CYAN: P-LOCKED ON STEPS OF";
    s2 = "THIS PATTERN (LOCKS WIN)";
    sc = CYAN;
  } else if (active && t.focus < nm && f->set.macros[t.focus].desc[0]) {
    const std::string m = std::string(f->set.macros[t.focus].name) + ": " + f->set.macros[t.focus].desc;
    s1 = fitStart(m, 29);
    s2 = m.size() > 29 ? fitStart(m.substr(29), 29) : std::string();
    sc = TEXT;
  } else {
    s1 = "CMD+ARROWS SET/MACRO, 1-4";
    s2 = "CMD+G GOOD  CMD+D BAD";
  }
  g.text(4, kStatusY, s1.c_str(), sc);
  g.text(4, kStatusY + 8, s2.c_str(), sc == RED || sc == AMBER || sc == TEXT ? sc : uint8_t(DIM));

  // sliders
  for (int m = 0; m < pm::kMaxMacros; ++m) {
    const int y = kSlY0 + m * kSlH;
    if (m >= nm) {
      g.rect(kBarX, y + 2, kBarW, kBarH, PANEL);
      continue;
    }
    const bool foc = m == t.focus;
    const pm::Macro& mac = f->set.macros[m];
    g.text(4, y + 2, mac.name, foc ? AMBER : TEXT);
    g.rect(kBarX, y + 2, kBarW, kBarH, RAISED);
    const int fill = int(t.pos[m] * float(kBarW - 1) + 0.5f);
    g.rect(kBarX, y + 2, fill + 1, kBarH, foc ? AMBER_D : CYAN_D);
    // the end labels inside the bar, at their ends
    if (mac.lo[0]) g.text(kBarX + 2, y + 2, upperStr(mac.lo).c_str(), TEXT);
    if (mac.hi[0]) g.textR(kBarX + kBarW - 3, y + 2, upperStr(mac.hi).c_str(), TEXT);
    // a tick under the bar where an enum parameter jumps (the other macros where they are)
    float jp[16];
    uint8_t jparam[16];
    const int nj = pm::enumJumps(f->set, pt_, m, t.pos, t.under, jp, jparam, 16);
    for (int k = 0; k < nj; ++k) g.rect(kBarX + int(jp[k] * float(kBarW - 1) + 0.5f), y + 10, 1, 2, AMBER);
    g.rect(kBarX + fill - 1, y + 1, 3, kBarH + 2, WHITE);
    g.textR(W - 3, y + 2, posText(t.pos[m]).c_str(), TEXT);
  }

  // XY pad
  g.rect(kPadX, kPadY, kPadS, kPadS, PANEL);
  g.frame(kPadX - 1, kPadY - 1, kPadS + 2, kPadS + 2, LINE);
  g.rect(kPadX + kPadS / 2, kPadY, 1, kPadS, RAISED);
  g.rect(kPadX, kPadY + kPadS / 2, kPadS, 1, RAISED);
  if (nm > 0) {
    // the axes' end labels: X along the bottom edge, Y up the left edge
    const pm::Macro& mx = f->set.macros[t.axisX];
    // (the bottom row holds 10 characters with a gap: the low label gets up to 5, the high one the rest)
    const std::string xlo = fitStart(upperStr(mx.lo), 5), xhi = fitStart(upperStr(mx.hi), 10 - xlo.size());
    if (mx.lo[0]) g.text(kPadX + 1, kPadY + kPadS - 8, xlo.c_str(), DIM);
    if (mx.hi[0]) g.textR(kPadX + kPadS - 2, kPadY + kPadS - 8, xhi.c_str(), DIM);
    if (nm > 1 && t.axisY != t.axisX) {
      const pm::Macro& my = f->set.macros[t.axisY];
      if (my.hi[0]) g.text(kPadX + 1, kPadY + 1, upperStr(my.hi).c_str(), DIM);
      if (my.lo[0]) g.text(kPadX + 1, kPadY + kPadS - 17, upperStr(my.lo).c_str(), DIM);
    }
    int x = 0, y = 0;
    padPoint(t.pos[t.axisX], t.axisY != t.axisX ? t.pos[t.axisY] : 0.5f, &x, &y);
    g.rect(x - 1, y - 1, 3, 3, AMBER);
    char ax[16];
    std::snprintf(ax, sizeof(ax), "X %s", f->set.macros[t.axisX].name);
    g.button(kAxisXX, kAxisY, kAxisW, 9, ax, false, CYAN);
    std::snprintf(ax, sizeof(ax), "Y %s", nm > 1 ? f->set.macros[t.axisY].name : "-");
    g.button(kAxisYX, kAxisY, kAxisW, 9, ax, false, CYAN);
  }

  // readout: what the set drives, as the instrument has it now (amber = edited by hand,
  // cyan = p-locked on steps of the current pattern, where the lock plays instead)
  if (active) {
    const uint32_t locked = host_->lockedMask(track);
    uint8_t out[pg::kParamsPerTrack];
    const uint32_t owned = pm::evaluate(f->set, pt_, t.pos, t.under, out);
    int n = 0, total = __builtin_popcount(owned);
    for (int p = 0; p < pg::kParamsPerTrack; ++p) {
      if (!((owned >> p) & 1)) continue;
      if (n == kReadRows * 2 - 1 && total > kReadRows * 2) {
        char more[12];
        std::snprintf(more, sizeof(more), "+%d", total - n);
        g.text(kColX + 46, kReadY + (n / 2) * 8, more, DIM);
        break;
      }
      const pg::ParamInfo* in = pt_.info[track][p];
      const int v = host_->paramValue(track, p);
      // the name, then the value right-aligned in its 7-character cell
      const int x = kColX + (n % 2) * 46, y = kReadY + (n / 2) * 8;
      g.text(x, y, in->name, ((locked >> p) & 1) ? uint8_t(CYAN) : uint8_t(DIM));
      g.textR(x + 44, y, fitStart(valueText(in, v), 3).c_str(), v != out[p] ? uint8_t(AMBER) : uint8_t(TEXT));
      ++n;
    }
  } else {
    g.text(kColX, kReadY, "NO SET:", DIM);
    g.text(kColX, kReadY + 8, "PARAMETERS ARE", DIM);
    g.text(kColX, kReadY + 16, "AS THEY ARE", DIM);
  }

  // marks
  g.button(kGoodX, kMarkY, kMarkW, kMarkH, "GOOD", typing_ && pendingLine_.compare(20, 4, "GOOD") == 0, GREEN);
  g.button(kBadX, kMarkY, kMarkW, kMarkH, "BAD", typing_ && pendingLine_.compare(20, 3, "BAD") == 0, RED);
  if (typing_) {
    g.rect(kColX, kNoteY - 1, kColW, 9, RAISED);
    const std::string vis = note_.size() > 13 ? note_.substr(note_.size() - 13) : note_;
    const int x = g.text(kColX + 2, kNoteY, vis.c_str(), WHITE);
    g.rect(x, kNoteY, 2, 7, AMBER);
  } else {
    g.text(kColX, kNoteY, fitStart(baseName(notesPath_), 15).c_str(), DIM);
  }
}

}  // namespace desk
