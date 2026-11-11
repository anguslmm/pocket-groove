// Run-time side of the lead wavetables: pick the generated constant set.
#include "lead_tables.h"

namespace pg {
namespace lead {

const Tables& tablesFor(float sampleRate) { return sampleRate >= 44100.0f ? kTables44k : kTables32k; }

}  // namespace lead
}  // namespace pg
