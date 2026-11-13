// The built-in instrument table, generated into builtin_data.cpp from the
// .pgm files under macros/ (macros/builtin.txt gives the order) by
//   build/host/macro_builtin macros lib/pg/src/pg/macro/builtin_data.cpp
// The ctest check_builtin fails when the checked-in file is stale. Const,
// constant-initialised: it lives in flash on the device and is used in place.
#pragma once
#include "macro.h"

namespace pg {
namespace macro {
namespace detail {

extern const MacroSet kBuiltin[];   // track order, then the list order of macros/builtin.txt
extern const int kBuiltinCount;

}  // namespace detail
}  // namespace macro
}  // namespace pg
