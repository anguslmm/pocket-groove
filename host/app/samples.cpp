#include "samples.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "common/sample_convert.h"
#include "common/wav.h"

namespace desk {

namespace {

bool statFile(const std::string& path, int64_t* mtimeNs, int64_t* size) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
#ifdef __APPLE__
  *mtimeNs = int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
  *mtimeNs = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
  *size = int64_t(st.st_size);
  return true;
}

int64_t wallNs() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

bool isAudioName(const std::string& n) {
  const size_t dot = n.rfind('.');
  if (dot == std::string::npos) return false;
  std::string e = n.substr(dot + 1);
  for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
  return e == "wav" || e == "wave" || e == "aif" || e == "aiff" || e == "aifc" || e == "mp3" || e == "m4a" ||
         e == "caf" || e == "flac";
}

bool copyFile(const std::string& from, const std::string& to) {
  std::vector<uint8_t> b;
  if (!sconv::readFile(from, &b)) return false;
  const std::string tmp = to + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
  if (std::fclose(f) != 0 || !ok) {
    std::remove(tmp.c_str());
    return false;
  }
  return std::rename(tmp.c_str(), to.c_str()) == 0;
}

std::string stemOf(const std::string& path) {
  std::string n = path;
  const size_t sl = n.find_last_of('/');
  if (sl != std::string::npos) n = n.substr(sl + 1);
  const size_t dot = n.rfind('.');
  if (dot != std::string::npos) n = n.substr(0, dot);
  std::string s;
  for (char c : n) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') s += c;
    else if (c == ' ' && !s.empty() && s.back() != '-') s += '-';
  }
  if (s.size() > 24) s.resize(24);
  return s.empty() ? std::string("sample") : s;
}

}  // namespace

bool Samples::init(const std::string& dir) {
  dir_ = dir;
  ::mkdir(dir_.c_str(), 0755);
  struct stat st;
  if (::stat(dir_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
  for (Slot& s : slots_) s.buf.assign(pg::sampler::kSlotMaxSamples, 0);
  scanNow();
  return true;
}

std::string Samples::fileOf(int slot) const {
  return slot >= 0 && slot < pg::sampler::kPoolSlots ? slots_[slot].file : std::string();
}

bool Samples::tick(uint32_t nowMs) {
  if (int32_t(nowMs - nextPoll_) < 0) return false;
  nextPoll_ = nowMs + kPollMs;
  return scanNow();
}

bool Samples::scanNow() {
  msgs_.clear();
  // what the folder holds now: per slot, the first audio file (by name) with its number
  std::string found[pg::sampler::kPoolSlots];
  if (DIR* d = opendir(dir_.c_str())) {
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
      if (n.empty() || n[0] == '.' || !isAudioName(n)) continue;
      const int slot = sconv::slotOfName(n, nullptr);
      if (slot < 0) continue;
      if (found[slot].empty()) found[slot] = n;
      else msgs_.push_back("slot " + std::to_string(slot + 1) + ": " + n + " ignored (" + found[slot] + " is first)");
    }
  }
  bool changed = false;
  const int64_t now = wallNs();
  for (int i = 0; i < pg::sampler::kPoolSlots; ++i) {
    Slot& s = slots_[i];
    int64_t mt = 0, sz = -1;
    const bool have = !found[i].empty() && statFile(dir_ + "/" + found[i], &mt, &sz);
    if (!have) {
      if (!s.file.empty() || s.loaded) {
        s.file.clear();
        s.mtimeNs = 0;
        s.size = -1;
        s.loaded = false;
        pool_.clear(i);
        msgs_.push_back("slot " + std::to_string(i + 1) + ": empty (plays its built-in sound)");
        changed = true;
      }
      continue;
    }
    if (found[i] == s.file && mt == s.mtimeNs && sz == s.size) continue;
    if (now - mt < kSettleNs) continue;   // still being written: next poll
    s.file = found[i];
    s.mtimeNs = mt;
    s.size = sz;
    load(i);
    changed = true;
  }
  return changed;
}

void Samples::load(int i) {
  Slot& s = slots_[i];
  std::vector<int16_t> x;
  std::string err, stem;
  sconv::slotOfName(s.file, &stem);
  ++reloads_;
  if (!sconv::convertFile(dir_ + "/" + s.file, &x, &err)) {
    s.loaded = false;
    pool_.clear(i);
    msgs_.push_back("slot " + std::to_string(i + 1) + ": " + s.file + ": " + err + " (plays its built-in sound)");
    return;
  }
  pool_.clear(i);   // a chop playing this slot stops (its pool generation changed) before the data does
  std::copy(x.begin(), x.end(), s.buf.begin());
  pool_.set(i, s.buf.data(), uint32_t(x.size()), stem.c_str());
  s.loaded = true;
  char line[160];
  const pg::sampler::SlotHeader* h = pool_.header(i);
  char nm[pg::sampler::kNameLen + 1] = "";
  if (h) pg::sampler::headerName(*h, nm);
  std::snprintf(line, sizeof line, "slot %d: %s %.2f s (%s)", i + 1, nm, double(x.size()) / sconv::kRate,
                s.file.c_str());
  msgs_.push_back(line);
}

bool Samples::drop(const std::string& path, int slot, std::string* msg) {
  if (slot < 0 || slot >= pg::sampler::kPoolSlots) {
    *msg = "no such slot";
    return false;
  }
  char num[4];
  std::snprintf(num, sizeof num, "%02d", slot + 1);
  const std::string dest = dir_ + "/" + num + "-" + stemOf(path) + ".wav";
  bool wav = false;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t h[12] = {};
    wav = std::fread(h, 1, 12, f) == 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4);
    std::fclose(f);
  } else {
    *msg = "cannot read " + path;
    return false;
  }
  const std::string tmp = dest + ".in.wav";
  if (wav) {
    std::vector<int16_t> x;
    std::string err;
    if (!sconv::convertFile(path, &x, &err)) {
      *msg = err;
      return false;
    }
    if (!copyFile(path, tmp)) {
      *msg = "cannot write " + dest;
      return false;
    }
  } else {
    // not a WAV: macOS's afconvert, when it is there
    if (::access("/usr/bin/afconvert", X_OK) != 0) {
      *msg = "not a WAV file";
      return false;
    }
    const std::string cmd = "/usr/bin/afconvert -f WAVE -d LEI24 '" + path + "' '" + tmp + "' 2>/dev/null";
    if (path.find('\'') != std::string::npos || std::system(cmd.c_str()) != 0) {
      std::remove(tmp.c_str());
      *msg = "afconvert could not read it";
      return false;
    }
  }
  // the slot's other files go (one file per slot), then the new one takes its name
  removeSlotFiles(slot, tmp);
  if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
    *msg = "cannot write " + dest;
    return false;
  }
  // reload it now, whatever its time stamp
  slots_[slot].file = dest.substr(dest.find_last_of('/') + 1);
  statFile(dest, &slots_[slot].mtimeNs, &slots_[slot].size);
  const std::string base = slots_[slot].file;
  msgs_.clear();
  load(slot);
  *msg = msgs_.empty() ? base : msgs_.back();
  return slots_[slot].loaded;
}

void Samples::removeSlotFiles(int slot, const std::string& keep) {
  if (DIR* d = opendir(dir_.c_str())) {
    std::vector<std::string> old;
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n[0] != '.' && sconv::slotOfName(n, nullptr) == slot && dir_ + "/" + n != keep) old.push_back(n);
    }
    closedir(d);
    for (const std::string& n : old) std::remove((dir_ + "/" + n).c_str());
  }
}

bool Samples::clearSlot(int slot, std::string* msg) {
  if (slot < 0 || slot >= pg::sampler::kPoolSlots) {
    *msg = "no such slot";
    return false;
  }
  removeSlotFiles(slot, "");
  Slot& s = slots_[slot];
  s.file.clear();
  s.mtimeNs = 0;
  s.size = -1;
  s.loaded = false;
  pool_.clear(slot);
  *msg = "slot " + std::to_string(slot + 1) + ": empty";
  return true;
}

bool Samples::putRecording(int slot, const int16_t* data, uint32_t samples, std::string* msg) {
  if (slot < 0 || slot >= pg::sampler::kPoolSlots || !data || samples < pg::sampler::kMinSamples) {
    *msg = "nothing to keep";
    return false;
  }
  char base[32];
  std::snprintf(base, sizeof base, "%02d-rec%02d.wav", slot + 1, slot + 1);
  const std::string dest = dir_ + "/" + base, tmp = dest + ".in.wav";
  const std::vector<int16_t> x(data, data + samples);
  if (!wav::write(tmp.c_str(), x, sconv::kRate, 1)) {
    std::remove(tmp.c_str());
    *msg = "cannot write " + dest;
    return false;
  }
  removeSlotFiles(slot, tmp);
  if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
    *msg = "cannot write " + dest;
    return false;
  }
  slots_[slot].file = base;
  statFile(dest, &slots_[slot].mtimeNs, &slots_[slot].size);
  msgs_.clear();
  load(slot);
  *msg = msgs_.empty() ? std::string(base) : msgs_.back();
  return slots_[slot].loaded;
}

}  // namespace desk
