#include "stems.h"

#include <SDL_scancode.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "json_lite.h"
#include "pg/ui/font.h"
#include "pg/ui/gfx.h"

extern char** environ;

namespace desk {

// ---- Job ------------------------------------------------------------------------------

bool Job::start(const std::vector<std::string>& argv, std::string* err) {
  stop();
  int fds[2];
  if (::pipe(fds) != 0) {
    *err = "pipe failed";
    return false;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);   // its own group: stop() ends its children too
  posix_spawnattr_setpgroup(&at, 0);
  std::vector<char*> av;
  for (const std::string& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, av[0], &fa, &at, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    *err = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc);
    return false;
  }
  pid_.store(pid);
  exit_.store(0);
  running_.store(true);
  th_ = std::thread([this, fd = fds[0]] { reader(fd); });
  return true;
}

void Job::reader(int fd) {
  std::string cur;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        std::lock_guard<std::mutex> g(mu_);
        lines_.push_back(cur);
        cur.clear();
      } else if (buf[i] != '\r') {
        cur += buf[i];
      }
    }
  }
  ::close(fd);
  if (!cur.empty()) {
    std::lock_guard<std::mutex> g(mu_);
    lines_.push_back(cur);
  }
  int status = 0;
  const pid_t pid = pid_.load();
  int code = 1;
  if (pid > 0 && ::waitpid(pid, &status, 0) == pid)
    code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  pid_.store(-1);
  exit_.store(code);
  running_.store(false);
}

std::vector<std::string> Job::take() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::string> out;
  out.swap(lines_);
  return out;
}

void Job::stop() {
  const pid_t pid = pid_.load();
  if (pid > 0 && running_.load()) ::kill(-pid, SIGTERM);
  join();
}

void Job::join() {
  if (th_.joinable()) th_.join();
}

// ---- helpers --------------------------------------------------------------------------

std::vector<std::string> wrapText(const std::string& s, int cols) {
  std::vector<std::string> out;
  std::string line;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    std::string word = s.substr(i, j - i);
    i = j;
    if (word.empty()) break;
    while (int(word.size()) > cols) {   // a path longer than a line
      if (!line.empty()) { out.push_back(line); line.clear(); }
      out.push_back(word.substr(0, size_t(cols)));
      word = word.substr(size_t(cols));
    }
    if (line.empty()) line = word;
    else if (int(line.size() + 1 + word.size()) <= cols) line += " " + word;
    else { out.push_back(line); line = word; }
  }
  if (!line.empty()) out.push_back(line);
  return out;
}

namespace {

std::string relToCwd(const std::string& p) {
  char cwd[4096];
  if (!::getcwd(cwd, sizeof(cwd))) return p;
  const std::string c = std::string(cwd) + "/";
  return p.compare(0, c.size(), c) == 0 ? p.substr(c.size()) : p;
}

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

// ---- Stems ----------------------------------------------------------------------------

std::vector<std::string> Stems::baseArgs() const {
  std::vector<std::string> a{cfg_.tool};
  if (!cfg_.port.empty()) { a.push_back("--port"); a.push_back(cfg_.port); }
  a.push_back("--json");
  return a;
}

void Stems::open() {
  open_ = true;
  dirty_ = true;
  scanLocal();
  if (!job_.running()) startLs();
  std::printf("[stems] open\n");
  std::fflush(stdout);
}

void Stems::close() {
  open_ = false;
  dirty_ = true;
  std::printf("[stems] closed\n");
  std::fflush(stdout);
}

void Stems::scanLocal() {
  // keep the device items, rebuild the local ones
  items_.resize(size_t(nDevice_));
  std::vector<Item> local;
  if (DIR* d = ::opendir(cfg_.projectsDir.c_str())) {
    while (dirent* e = ::readdir(d)) {
      const std::string f = e->d_name;
      if (f.size() < 5 || f.compare(f.size() - 4, 4, ".pgv") != 0 || f[0] == '.') continue;
      Item it{false, f.substr(0, f.size() - 4), cfg_.projectsDir + "/" + f, "", 0};
      if (FILE* fp = std::fopen(it.path.c_str(), "rb")) {
        uint8_t h[16] = {};
        if (std::fread(h, 1, sizeof(h), fp) == sizeof(h) && !std::memcmp(h, "PGVF", 4)) it.bpm = h[10];
        std::fclose(fp);
      }
      local.push_back(it);
    }
    ::closedir(d);
  }
  std::sort(local.begin(), local.end(), [](const Item& a, const Item& b) {
    if ((a.name == "_last") != (b.name == "_last")) return a.name == "_last";
    return a.name < b.name;
  });
  items_.insert(items_.end(), local.begin(), local.end());
  if (sel_ >= selectable()) sel_ = selectable() - 1;
  if (sel_ < 0) sel_ = 0;
}

void Stems::startLs() {
  dev_ = Dev::Asking;
  devMsg_.clear();
  run_ = Run::Listing;
  jobName_.clear();
  sawJsonError_ = false;
  errText_.clear();
  lastPlain_.clear();
  devLines_.clear();
  std::vector<std::string> a = baseArgs();
  a.push_back("ls");
  std::string err;
  if (!job_.start(a, &err)) {
    dev_ = Dev::Failed;
    devMsg_ = err;
    run_ = Run::Idle;
  }
  dirty_ = true;
}

void Stems::startBounce(const Item& it) {
  std::vector<std::string> a = baseArgs();
  a.push_back("bounce");
  std::string dir;
  if (it.device) {
    const bool session = it.name == "_last";
    dir = session ? "SESSION" : it.name;
    a.push_back(session ? "session" : it.name);
    a.push_back("--pull-dir");   // the pulled project goes next to its stems, not over ./projects
    a.push_back(cfg_.outRoot + "/" + dir);
    if (!session) a.push_back(it.where == "SD" ? "--sd" : "--int");
    jobName_ = session ? "THE DEVICE'S SESSION" : it.name;
  } else {
    dir = it.name == "_last" ? "DESKTOP-SESSION" : it.name;
    a.push_back(it.path);
    jobName_ = it.name == "_last" ? "THE DESKTOP SESSION" : it.name;
  }
  a.push_back("--out");
  a.push_back(cfg_.outRoot + "/" + dir);
  run_ = Run::Bouncing;
  stage_ = "STARTING";
  done_ = total_ = 0;
  nFiles_ = 0;
  outDir_.clear();
  errText_.clear();
  lastPlain_.clear();
  sawJsonError_ = false;
  std::string err;
  std::printf("[stems] bouncing %s:", jobName_.c_str());
  for (const std::string& s : a) std::printf(" %s", s.c_str());
  std::printf("\n");
  std::fflush(stdout);
  if (!job_.start(a, &err)) {
    run_ = Run::Failed;
    errText_ = err;
    std::printf("[stems] failed: %s\n", err.c_str());
  }
  dirty_ = true;
}

void Stems::key(int sc, bool repeat) {
  if (sc == SDL_SCANCODE_ESCAPE) {
    if (!repeat) close();
    return;
  }
  if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) {
    const int n = selectable();
    if (n > 0) sel_ = (sel_ + (sc == SDL_SCANCODE_UP ? n - 1 : 1)) % n;
    dirty_ = true;
    return;
  }
  if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && !repeat) {
    if (job_.running() || sel_ >= selectable()) return;   // one job at a time
    startBounce(items_[size_t(sel_)]);
  }
}

void Stems::handleLine(const std::string& line) {
  if (line.empty()) return;
  JVal v;
  if (line[0] != '{' || !JsonReader::parse(line, &v) || v.type != JVal::Obj) {
    lastPlain_ = line;
    if (run_ == Run::Listing) devLines_.push_back(line);
    std::printf("[pg] %s\n", line.c_str());
    return;
  }
  const std::string type = v.str("type");
  if (type == "ls") {
    std::vector<Item> dev;
    firmware_ = v.str("firmware");
    if (const JVal* ps = v.get("projects"))
      for (const JVal& p : ps->a) {
        Item it{true, p.str("name"), "", p.str("mount") == "sd" ? "SD" : "FLASH", int(p.num("bpm"))};
        if (!p.boolean("valid", true)) it.where += " ?";
        dev.push_back(it);
      }
    std::stable_sort(dev.begin(), dev.end(), [](const Item& a, const Item& b) {
      return (a.name == "_last") > (b.name == "_last");   // the session first
    });
    std::vector<Item> local(items_.begin() + nDevice_, items_.end());
    items_ = dev;
    nDevice_ = int(dev.size());
    items_.insert(items_.end(), local.begin(), local.end());
    dev_ = Dev::Listed;
    std::printf("[stems] device: %d project(s), firmware %s\n", nDevice_, firmware_.c_str());
  } else if (type == "progress") {
    const std::string st = v.str("stage");
    done_ = int(v.num("done"));
    total_ = int(v.num("total"));
    stage_ = st == "connect" ? "CONNECTING TO THE DEVICE" : st == "pull" ? "PULLING THE PROJECT"
             : st == "build" ? "BUILDING THE RENDERER (ONCE)" : "RENDERING";
  } else if (type == "done") {
    outDir_ = v.str("dir");
    if (const JVal* f = v.get("files")) nFiles_ = int(f->a.size());
  } else if (type == "error") {
    errText_ = v.str("message");
    sawJsonError_ = true;
  }
  dirty_ = true;
}

void Stems::finished() {
  const int code = job_.exitCode();
  job_.join();
  std::string err;
  if (code != 0) {
    if (sawJsonError_) err = errText_;
    else if (code == 126 || code == 127) err = "cannot run " + cfg_.tool + " (set PG_TOOL or --pg-tool)";
    else {
      bool pyserial = false;
      for (const std::string& l : devLines_) pyserial = pyserial || l.find("pyserial") != std::string::npos;
      pyserial = pyserial || lastPlain_.find("pyserial") != std::string::npos;
      if (pyserial)
        err = "pyserial is missing: tools/pg needs Python 3 with pyserial (docs/MANUAL.md, Bouncing stems)";
      else
        err = lastPlain_.empty() ? "tools/pg failed (exit " + std::to_string(code) + ")" : lastPlain_;
    }
  }
  if (run_ == Run::Listing) {
    if (err.empty() && dev_ != Dev::Listed) err = "no answer from tools/pg";
    if (!err.empty()) {
      dev_ = Dev::Failed;
      devMsg_ = err;
      std::printf("[stems] device: error: %s\n", err.c_str());
    }
    run_ = Run::Idle;
  } else if (run_ == Run::Bouncing) {
    if (err.empty() && outDir_.empty()) err = "the bounce wrote no files";
    if (!err.empty()) {
      run_ = Run::Failed;
      errText_ = err;
      std::printf("[stems] failed: %s\n", err.c_str());
    } else {
      run_ = Run::Done;
      std::printf("[stems] done: %s (%d files)\n", outDir_.c_str(), nFiles_);
      if (!cfg_.reveal.empty()) {
        std::string e;
        reveal_.stop();
        if (reveal_.start({cfg_.reveal, outDir_}, &e)) std::printf("[stems] revealed with %s\n", cfg_.reveal.c_str());
        else std::printf("[stems] cannot reveal: %s\n", e.c_str());
      }
      scanLocal();
    }
  }
  std::fflush(stdout);
  dirty_ = true;
}

void Stems::poll() {
  const bool wasRunning = job_.running();
  for (const std::string& l : job_.take()) handleLine(l);
  if (!wasRunning && (run_ == Run::Listing || run_ == Run::Bouncing)) {
    for (const std::string& l : job_.take()) handleLine(l);   // the last lines before it ended
    finished();
  }
  for (const std::string& l : reveal_.take()) std::printf("[reveal] %s\n", l.c_str());
}

void Stems::shutdown() {
  job_.stop();
  reveal_.join();
}

// ---- drawing --------------------------------------------------------------------------

void Stems::draw(pg::Framebuffer& fb) {
  using namespace pg::ui;
  dirty_ = false;
  Gfx g(fb);
  g.clear(BG);
  // title bar and a cyan frame: the desktop app's, not the device's
  g.rect(0, 0, pg::kScreenW, 11, CYAN);
  g.text(3, 2, "STEMS", BG);
  g.text(37, 2, "DEVICE + LOCAL PROJECTS", CYAN_D);
  g.textR(pg::kScreenW - 3, 2, "ESC", BG);
  g.frame(0, 0, pg::kScreenW, pg::kScreenH, CYAN);

  struct Row {
    int kind;   // 0 header, 1 item, 2 message
    int item;
    std::string text;
    uint8_t col;
  };
  std::vector<Row> rows;
  std::string h = "ON THE DEVICE";
  if (dev_ == Dev::Listed && !firmware_.empty()) h += " (FIRMWARE " + upper(firmware_) + ")";
  rows.push_back({0, -1, h, AMBER});
  if (dev_ == Dev::Asking) rows.push_back({2, -1, "asking the device...", DIM});
  else if (dev_ == Dev::Failed)
    for (const std::string& l : wrapText(devMsg_, 37)) rows.push_back({2, -1, l, RED});
  else if (nDevice_ == 0) rows.push_back({2, -1, "no saved projects", DIM});
  for (int i = 0; i < nDevice_; ++i) rows.push_back({1, i, "", TEXT});
  rows.push_back({0, -1, "IN " + upper(cfg_.projectsDir) + "/", AMBER});
  if (int(items_.size()) == nDevice_) rows.push_back({2, -1, "no .pgv files", DIM});
  for (int i = nDevice_; i < int(items_.size()); ++i) rows.push_back({1, i, "", TEXT});

  constexpr int kTop = 14, kRowH = 9, kListBottom = 96;
  const int visible = (kListBottom - kTop) / kRowH;
  int selRow = 0;
  for (int r = 0; r < int(rows.size()); ++r)
    if (rows[size_t(r)].kind == 1 && rows[size_t(r)].item == sel_) selRow = r;
  if (selRow < top_ + 1) top_ = selRow > 0 ? selRow - 1 : 0;   // keep its header in view when possible
  if (selRow >= top_ + visible) top_ = selRow - visible + 1;
  if (top_ > int(rows.size()) - visible) top_ = std::max(0, int(rows.size()) - visible);
  for (int r = top_; r < int(rows.size()) && r < top_ + visible; ++r) {
    const Row& row = rows[size_t(r)];
    const int y = kTop + (r - top_) * kRowH;
    if (row.kind == 0) { g.text(4, y + 1, row.text.c_str(), row.col); continue; }
    if (row.kind == 2) { g.text(10, y + 1, row.text.c_str(), row.col); continue; }
    const Item& it = items_[size_t(row.item)];
    const bool sel = row.item == sel_;
    if (sel) g.rect(2, y, pg::kScreenW - 4, kRowH, RAISED);
    if (sel) g.text(4, y + 1, PG_ICON_TRI_R, CYAN);
    std::string label = it.device ? (it.name == "_last" ? "SESSION (AUTOSAVED)" : it.name)
                                  : (it.name == "_last" ? "DESKTOP SESSION" : it.name);
    if (label.size() > 22) label = label.substr(0, 22);
    g.text(12, y + 1, label.c_str(), sel ? WHITE : TEXT);
    char right[32];
    if (it.bpm) std::snprintf(right, sizeof(right), "%3d BPM %s", it.bpm, it.where.c_str());
    else std::snprintf(right, sizeof(right), "%s", it.where.c_str());
    g.textR(pg::kScreenW - 5, y + 1, right, DIM);
  }
  if (top_ > 0) g.text(pg::kScreenW - 10, kTop - 1, PG_ICON_UP, DIM);
  if (top_ + visible < int(rows.size())) g.text(pg::kScreenW - 10, kListBottom - 7, PG_ICON_DOWN, DIM);

  // status: what happens / happened
  g.rect(1, 98, pg::kScreenW - 2, 1, LINE);
  int y = 100;
  auto line = [&](const std::string& s, uint8_t c) {
    if (y > pg::kScreenH - 8) return;
    g.text(5, y, s.c_str(), c);
    y += 8;
  };
  switch (run_) {
    case Run::Idle:
    case Run::Listing:
      line("UP/DOWN CHOOSE   RETURN BOUNCE STEMS", TEXT);
      line("the stems go to " + cfg_.outRoot + "/<NAME>/", DIM);
      line("with tools/pg bounce's defaults", DIM);
      break;
    case Run::Bouncing: {
      line("BOUNCING " + upper(jobName_), AMBER);
      char t[64];
      if (total_ > 1 && stage_ == "RENDERING") std::snprintf(t, sizeof(t), "%s  PASS %d OF %d", stage_.c_str(), done_, total_);
      else if (total_ > 1) std::snprintf(t, sizeof(t), "%s  %d%%", stage_.c_str(), 100 * done_ / std::max(total_, 1));
      else std::snprintf(t, sizeof(t), "%s", stage_.c_str());
      line(t, TEXT);
      const int w = pg::kScreenW - 12;
      g.rect(6, y + 1, w, 5, PANEL);
      if (total_ > 0) g.rect(6, y + 1, w * std::min(done_, total_) / total_, 5, AMBER);
      break;
    }
    case Run::Done:
      line("DONE: " + std::to_string(nFiles_) + " FILES IN", GREEN);
      for (const std::string& l : wrapText(relToCwd(outDir_) + "/", 37)) line(l, TEXT);
      if (!cfg_.reveal.empty()) line("(opened the folder)", DIM);
      break;
    case Run::Failed:
      line("FAILED: " + upper(jobName_), RED);
      for (const std::string& l : wrapText(errText_, 37)) line(l, RED);
      break;
  }
}

}  // namespace desk
