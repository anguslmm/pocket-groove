// Stems (Cmd+B, docs/DESKTOP.md): a list of the projects on the connected
// device (its autosaved session included) and in the local projects
// directory; choosing one renders its stems with the existing bounce code and
// opens the folder.
//
// Everything that touches the device or renders runs `tools/pg --json` as a
// subprocess (ls, bounce: it already finds the port, pulls and renders), read
// line by line on a worker thread, so the window and the audio never wait.
// The list itself is drawn by the desktop app into its own 240x135
// framebuffer with the instrument's font and palette, framed in cyan so it is
// never mistaken for the device's screen.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pg/platform.h"

namespace desk {

// One subprocess with its stdout and stderr merged, read on a thread.
class Job {
 public:
  ~Job() { stop(); }
  bool start(const std::vector<std::string>& argv, std::string* err);
  bool running() const { return running_.load(); }
  // Lines read since the last call (main thread).
  std::vector<std::string> take();
  int exitCode() const { return exit_.load(); }   // valid once !running()
  void stop();                                     // SIGTERM, then reap
  void join();

 private:
  void reader(int fd);
  std::thread th_;
  std::mutex mu_;
  std::vector<std::string> lines_;
  std::atomic<bool> running_{false};
  std::atomic<int> exit_{0};
  std::atomic<int> pid_{-1};
};

struct StemsConfig {
  std::string tool;            // tools/pg (launcher) path
  std::string port;            // --port for tools/pg; empty = auto-detect ($PG_PORT works too)
  std::string projectsDir;     // the app's --dir
  std::string outRoot = "bounces";
  std::string reveal;          // command that opens a folder ("open" / "xdg-open"); empty = do not
};

class Stems {
 public:
  void init(const StemsConfig& c) { cfg_ = c; }
  bool isOpen() const { return open_; }
  void open();
  void close();
  void key(int scancode, bool repeat);   // SDL scancodes: Up, Down, Return, Escape
  void poll();                            // main loop: job output
  bool busy() const { return job_.running(); }
  bool changed() const { return dirty_; }
  void draw(pg::Framebuffer& fb);         // clears changed()
  void shutdown();

 private:
  struct Item {
    bool device;               // on the device (pull first) or a local file
    std::string name;          // device name (_last = session) or local file stem
    std::string path;          // local: the .pgv
    std::string where;         // "FLASH" / "SD" / ""
    int bpm;
  };
  enum class Dev { Asking, Listed, Failed } dev_ = Dev::Asking;
  enum class Run { Idle, Listing, Bouncing, Done, Failed } run_ = Run::Idle;
  void scanLocal();
  void startLs();
  void startBounce(const Item& it);
  void handleLine(const std::string& line);
  void finished();
  std::vector<std::string> baseArgs() const;
  int selectable() const { return int(items_.size()); }

  StemsConfig cfg_;
  bool open_ = false, dirty_ = true;
  std::vector<Item> items_;   // device items first, then local
  int nDevice_ = 0;
  int sel_ = 0, top_ = 0;
  std::string devMsg_;        // device section: error or empty
  std::string firmware_;
  // the running / last job
  Job job_, reveal_;
  std::string jobName_, stage_, result_, outDir_, errText_, lastPlain_;
  int done_ = 0, total_ = 0, nFiles_ = 0;
  bool sawJsonError_ = false;
  std::vector<std::string> devLines_;
};

// Word-wraps s into lines of at most `cols` characters.
std::vector<std::string> wrapText(const std::string& s, int cols);

}  // namespace desk
