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

# The hardware spike's portable parts (src/hwspike, env:hw-spike, docs/hw-spike.md).
add_subdirectory(hwspike)

# The desktop app (host/app, docs/DESKTOP.md): built only when SDL2 is found.
option(PG_DESKTOP "Build the desktop app pocket-groove (needs SDL2)" ON)
if(PG_DESKTOP)
  add_subdirectory(app)
endif()
