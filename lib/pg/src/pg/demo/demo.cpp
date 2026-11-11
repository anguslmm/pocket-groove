#include "demo.h"

#include "demo_songs.h"

namespace pg::demo {

namespace {

// The factory songs (demo_acidjam.cpp, demo_basshaus.cpp, demo_warehous.cpp).
// Demo 0 is what a new user hears first.
struct Rom {
  const char* name;
  int tempo;
  void (*build)(const Engine&, Project&);
};
const Rom kRom[] = {
    {"ACIDJAM", 124, buildAcidJam},
    {"BASSHAUS", 126, buildBassHaus},
    {"WAREHOUS", 133, buildWarehous},
};
constexpr int kCount = int(sizeof(kRom) / sizeof(kRom[0]));

}  // namespace

int count() { return kCount; }

const char* name(int i) { return (i >= 0 && i < kCount) ? kRom[i].name : nullptr; }

int tempo(int i) { return (i >= 0 && i < kCount) ? kRom[i].tempo : 0; }

void build(const Engine& engine, Project& p, int i) {
  if (i < 0 || i >= kCount) {
    engine.initProject(p);
    return;
  }
  kRom[i].build(engine, p);
}

}  // namespace pg::demo
