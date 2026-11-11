// The three factory songs, one builder per file (demo_acidjam.cpp,
// demo_basshaus.cpp, demo_warehous.cpp). Each writes a complete project,
// starting from engine.initProject(p). demo.cpp dispatches to them.
#pragma once
#include "../engine.h"
#include "../project.h"

namespace pg::demo {

void buildAcidJam(const Engine& engine, Project& p);    // "ACIDJAM"  acid house, A minor, 124 BPM
void buildBassHaus(const Engine& engine, Project& p);   // "BASSHAUS" bass house, F minor, 126 BPM
void buildWarehous(const Engine& engine, Project& p);   // "WAREHOUS" techno, E minor (phrygian), 133 BPM

}  // namespace pg::demo
