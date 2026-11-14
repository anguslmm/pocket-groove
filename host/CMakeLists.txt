# Desktop build of the portable core: per-component static libs so one
# component failing to compile doesn't block the others.
#   cmake -S host -B build/host -DCMAKE_BUILD_TYPE=Release && cmake --build build/host -j
# Sources:  lib/pg/src/pg/<component>/*.cpp   -> library pg_<component>
# Tests:    host/tests/test_<component>_*.cpp -> executable, linked to pg_<component>, registered with ctest
# Tools:    host/tools/<name>.cpp             -> executable linked to every component
# System tests: host/itests/itest_*.cpp       -> executable linked to every component, registered with ctest
cmake_minimum_required(VERSION 3.16)
project(pocket_groove_host CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(PG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(PG_SRC ${PG_ROOT}/lib/pg/src)
# Match the firmware's constraints: no exceptions/RTTI, and flag accidental doubles.
# Host-only engine hooks for offline stem rendering (lib/pg/src/pg/engine/stem_tap.h).
# The firmware never defines it, so the device build compiles none of it.
add_compile_definitions(PG_STEM_TAP=1)
set(PG_FLAGS -Wall -Wextra -Wdouble-promotion -Wfloat-conversion -fno-exceptions -fno-rtti)
enable_testing()

file(GLOB PG_COMPONENT_DIRS LIST_DIRECTORIES true ${PG_SRC}/pg/*)
set(PG_LIBS "")
foreach(dir ${PG_COMPONENT_DIRS})
  if(IS_DIRECTORY ${dir})
    get_filename_component(comp ${dir} NAME)
    file(GLOB_RECURSE srcs CONFIGURE_DEPENDS ${dir}/*.cpp)
    if(srcs)
      add_library(pg_${comp} STATIC ${srcs})
      target_include_directories(pg_${comp} PUBLIC ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_options(pg_${comp} PRIVATE ${PG_FLAGS})
      list(APPEND PG_LIBS pg_${comp})
    endif()
  endif()
endforeach()

# The engine derives an instrument's parameters with pg::macro (evaluate, the
# built-in library): its tests link the macro component too.
if(TARGET pg_engine AND TARGET pg_macro)
  target_link_libraries(pg_engine PUBLIC pg_macro)
endif()
# the voice kinds' parameter tables (pg/sampler/sampler_params.h) are read by the macro module
if(TARGET pg_macro AND TARGET pg_sampler)
  target_link_libraries(pg_macro PUBLIC pg_sampler)
endif()
# the melodic sampler (pg/sampler/melodic.cpp) expands chords with the lead's table
if(TARGET pg_sampler AND TARGET pg_lead)
  target_link_libraries(pg_sampler PUBLIC pg_lead)
endif()
# ... and by the transfer protocol (the sample pool's write side, pg/sampler/flash_pool.h)
if(TARGET pg_io AND TARGET pg_sampler)
  target_link_libraries(pg_io PUBLIC pg_sampler)
endif()

file(GLOB test_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
foreach(src ${test_srcs})
  get_filename_component(name ${src} NAME_WE)
  string(REGEX REPLACE "^test_([a-z0-9]+)_.*$" "\\1" comp ${name})
  if(TARGET pg_${comp})
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE pg_${comp})
    target_include_directories(${name} PRIVATE ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE ${PG_FLAGS})
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${PG_ROOT})
  endif()
endforeach()

file(GLOB tool_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp)
foreach(src ${tool_srcs})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  # Link twice so inter-component references resolve regardless of order.
  target_link_libraries(${name} PRIVATE ${PG_LIBS} ${PG_LIBS})
  target_include_directories(${name} PRIVATE ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${name} PRIVATE ${PG_FLAGS})
endforeach()

# Lookup-table generator (tools/gen/gen_tables.cpp): rewrites the generated
# *_tables_data.cpp files under lib/pg/src/pg from their reference formulas.
# Built, never run by the build:  build/host/gen_tables lib/pg/src
# It compiles the formulas itself (not the component libs, which contain the
# generated data), so it builds even when that data is missing or stale.
add_executable(gen_tables ${PG_ROOT}/tools/gen/gen_tables.cpp
               ${PG_SRC}/pg/lead/lead_tables_ref.cpp ${PG_SRC}/pg/bass/bass_tables_ref.cpp)
target_include_directories(gen_tables PRIVATE ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gen_tables PRIVATE ${PG_FLAGS})

file(GLOB itest_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/itests/itest_*.cpp)
foreach(src ${itest_srcs})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE ${PG_LIBS} ${PG_LIBS})
  target_include_directories(${name} PRIVATE ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${name} PRIVATE ${PG_FLAGS})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${PG_ROOT})
endforeach()

# itest_bounce runs the bounce tool: build it first.
if(TARGET itest_bounce AND TARGET bounce)
  add_dependencies(itest_bounce bounce)
endif()

# The owner's assessment sheet must list exactly the instrument's parameters, with the
# ranges and defaults the code has (host/tools/param_reference.cpp, --check-assessment).
if(TARGET param_reference)
  add_test(NAME check_assessment
           COMMAND param_reference --check-assessment docs/ASSESSMENT.md
           WORKING_DIRECTORY ${PG_ROOT})
  # ... and so must the macro lab's list of parameter names (docs/MACROLAB.md).
  add_test(NAME check_macrolab
           COMMAND param_reference --check-macrolab docs/MACROLAB.md
           WORKING_DIRECTORY ${PG_ROOT})
endif()

# Every macro set in macros/ must make a sane sound everywhere in its macro space:
# no parse error, no silent or clipping point, no NaN / Inf, no engine fault
# (host/tools/macro_audit.cpp, docs/MACROLAB.md "Auditing a set"). And the audit
# must catch the broken sets in host/tests/macro_bad/ (each one must fail).
if(TARGET macro_audit)
  add_test(NAME macro_audit COMMAND macro_audit --hard macros WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(macro_audit PROPERTIES TIMEOUT 600)
  # The sampler sets (voice perc sampler) again, on a test sample in pool slot 1 instead
  # of the built-in sound an empty slot plays (macro_audit --test-sample).
  add_test(NAME macro_audit_test_sample COMMAND macro_audit --hard --test-sample macros/perc
           WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(macro_audit_test_sample PROPERTIES TIMEOUT 600)
  set(why_silent "HARD  silent at GONE=0.00")
  set(why_clips "HARD  clips .stem peak above 0 dBFS. at HOT=1.00")
  set(why_typo "typo.pgm:5: unknown parameter 'cutx' for bass")
  # The built-in instrument table (lib/pg/src/pg/macro/builtin_data.cpp) must be what
  # host/tools/macro_builtin makes of macros/ now; it also fails on id collisions.
  add_test(NAME check_builtin COMMAND macro_builtin --check WORKING_DIRECTORY ${PG_ROOT})
  # ... and the sampler's built-in sounds (lib/pg/src/pg/sampler/rom_samples_data.cpp) what
  # host/tools/gen_rom_samples makes now (within 2 LSB: libm differs between hosts).
  if(TARGET gen_rom_samples)
    add_test(NAME check_rom_samples COMMAND gen_rom_samples --check WORKING_DIRECTORY ${PG_ROOT})
  endif()
  foreach(bad silent clips typo)
    add_test(NAME macro_audit_catches_${bad}
             COMMAND macro_audit --hard host/tests/macro_bad/${bad}.pgm
             WORKING_DIRECTORY ${PG_ROOT})
    # the right reason, and the exit status (the summary line says 1 failed)
    set_tests_properties(macro_audit_catches_${bad} PROPERTIES
                         PASS_REGULAR_EXPRESSION "${why_${bad}}.*1 with hard failures")
  endforeach()
endif()

# Macro motion (stage B, engine.h "MACRO MOTION"): a held-key macro sweep through the
# engine must be as smooth as one continuous ramp (bass ACID, lead HANDS, kick BOOM in a
# roll); host/tools/zipper_probe.cpp --engine, WAVs in build/zipper/.
if(TARGET zipper_probe)
  add_test(NAME zipper_motion COMMAND zipper_probe --engine --check build/zipper WORKING_DIRECTORY ${PG_ROOT})
endif()

# The USB transfer client (tools/pg.py) against the device's protocol module
# (pg::io::Xfer in host/tools/xfer_sim, on a pseudo-terminal). Needs Python 3
# with pyserial; skipped (exit 77) without pyserial.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET xfer_sim AND NOT WIN32)
  add_test(NAME test_pg_client
           COMMAND ${Python3_EXECUTABLE} ${PG_ROOT}/tools/test_pg.py --sim $<TARGET_FILE:xfer_sim>
           WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(test_pg_client PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
# The sample converters: C++ (host/common/sample_convert.h, the desktop's) and Python
# (tools/pg_sample.py, tools/pg sample put) agree on shared test vectors.
if(Python3_Interpreter_FOUND AND TARGET sample_convert)
  add_test(NAME test_sample_convert
           COMMAND ${Python3_EXECUTABLE} ${PG_ROOT}/tools/test_sample_convert.py $<TARGET_FILE:sample_convert>
           WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(test_sample_convert PROPERTIES TIMEOUT 300)
endif()

# The hardware spike's portable parts (src/hwspike, env:hw-spike, docs/hw-spike.md).
add_subdirectory(hwspike)

# The desktop app (host/app, docs/DESKTOP.md): built only when SDL2 is found.
option(PG_DESKTOP "Build the desktop app pocket-groove (needs SDL2)" ON)
if(PG_DESKTOP)
  add_subdirectory(app)
endif()
