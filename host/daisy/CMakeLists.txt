# Host tests for the Daisy Seed target's portable code (daisy/core, daisy/app;
# docs/DAISY.md). The firmware itself is built by daisy/Makefile with
# arm-none-eabi-gcc; everything here is the same source compiled for the host.
#   test_daisy_*.cpp   unit and system tests, registered with ctest (label daisy)
#   daisy_sim          the firmware's App on the host, its serial port on stdio or a
#                      pseudo-terminal (tools/pg-daisy and tools/pg talk to it)
set(PGD_ROOT ${PG_ROOT}/daisy)
file(GLOB PGD_CORE_SRCS CONFIGURE_DEPENDS ${PGD_ROOT}/core/*.cpp ${PGD_ROOT}/app/*.cpp)
add_library(pgd_core STATIC ${PGD_CORE_SRCS})
target_include_directories(pgd_core PUBLIC ${PG_SRC} ${PGD_ROOT})
# daisy/app/bench.cpp plays the busy pattern of host/tools/bench_instrument (host/common/grooves.h).
target_include_directories(pgd_core PRIVATE ${PG_ROOT}/host)
target_compile_options(pgd_core PRIVATE ${PG_FLAGS} -Wshadow)
# The pg component libraries reference each other: listed twice so the linker
# resolves them in any order (as host/CMakeLists.txt does for the tools).
set(PGD_LINK pgd_core ${PG_LIBS} ${PG_LIBS})

file(GLOB pgd_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_daisy_*.cpp)
foreach(src ${pgd_tests})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE ${PGD_LINK})
  target_include_directories(${name} PRIVATE ${PG_SRC} ${PGD_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${name} PRIVATE ${PG_FLAGS})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(${name} PROPERTIES LABELS daisy TIMEOUT 300)
endforeach()

add_executable(daisy_sim daisy_sim.cpp)
target_link_libraries(daisy_sim PRIVATE ${PGD_LINK})
target_include_directories(daisy_sim PRIVATE ${PG_SRC} ${PGD_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(daisy_sim PRIVATE ${PG_FLAGS})

# The computer side of stage 0 (tools/pg-daisy: keys, report, screen) and tools/pg
# (projects, samples) against the firmware's App in daisy_sim, on a pseudo-terminal.
# Needs Python 3 with pyserial; skipped (exit 77) without it.
if(Python3_Interpreter_FOUND AND NOT WIN32)
  add_test(NAME test_pg_daisy
           COMMAND ${Python3_EXECUTABLE} ${PG_ROOT}/tools/test_pg_daisy.py --sim $<TARGET_FILE:daisy_sim>
           WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(test_pg_daisy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS daisy)
endif()

# The host's count for the on-device bench (daisy/app/bench.h): the same scenes under
# callgrind, the core as the Daisy builds it (PG_TAPE=0: pg_notape), bench.cpp compiled
# here again with it. Writes daisy/app/bench_ref.h and daisy/itcm_profile.txt.
add_executable(daisy_bench_ref daisy_bench_ref.cpp ${PGD_ROOT}/app/bench.cpp)
target_link_libraries(daisy_bench_ref PRIVATE pg_notape)
target_include_directories(daisy_bench_ref PRIVATE ${PG_SRC} ${PGD_ROOT} ${PG_ROOT}/host)
target_compile_options(daisy_bench_ref PRIVATE ${PG_FLAGS})
