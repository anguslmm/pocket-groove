// Factory demo projects, compiled into the firmware ("ROM" rows in PROJECT).
// Placeholder musical content (UX.md section 14); a composer replaces it later.
#pragma once
#include "../engine.h"
#include "../project.h"

namespace pg::demo {

int count();                     // number of ROM demos
const char* name(int i);         // <= 8 chars, A-Z 0-9 - _ ; nullptr if out of range
int tempo(int i);                // BPM, for the PROJECT list (no need to build the project)
// Writes a complete, valid project (starting from Engine::initProject) into p.
// Not real-time: call with the engine suspended (or before it runs).
void build(const Engine& engine, Project& p, int i);

}  // namespace pg::demo
