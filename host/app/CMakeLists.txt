# The desktop app (docs/DESKTOP.md): target pocket-groove, needs SDL2.
# Optional: without SDL2 everything else in host/ configures and builds as before.
#   macOS:  brew install sdl2        Debian/Ubuntu:  apt install libsdl2-dev
#   or opt in to building SDL2 from source:  -DPG_FETCH_SDL2=ON
option(PG_FETCH_SDL2 "Download and build SDL2 from source for the desktop app" OFF)

set(PG_SDL_TARGET "")
if(PG_FETCH_SDL2)
  include(FetchContent)
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.32.10
                       GIT_SHALLOW TRUE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL2)
  set(PG_SDL_TARGET SDL2::SDL2-static)
else()
  # Homebrew: `brew install sdl2` installs sdl2-compat (the SDL2 API on SDL3; Formula/s/sdl2-compat.rb,
  # "sdl2" is an alias). It ships <prefix>/lib/cmake/SDL2/SDL2Config.cmake with the imported
  # target SDL2::SDL2. CMake finds it through PATH (/opt/homebrew/bin -> /opt/homebrew); the
  # PATHS cover a CMake that is not on that PATH (Apple silicon, then Intel prefixes).
  find_package(SDL2 CONFIG QUIET PATHS /opt/homebrew /usr/local)
  if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)   # a FindSDL2.cmake module, if the system has one
  endif()
  if(TARGET SDL2::SDL2)
    set(PG_SDL_TARGET SDL2::SDL2)
  elseif(TARGET SDL2::SDL2-static)
    set(PG_SDL_TARGET SDL2::SDL2-static)
  elseif(SDL2_FOUND AND SDL2_LIBRARIES)
    add_library(pg_sdl2 INTERFACE)
    target_include_directories(pg_sdl2 SYSTEM INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(pg_sdl2 INTERFACE ${SDL2_LIBRARIES})
    set(PG_SDL_TARGET pg_sdl2)
  endif()
endif()

if(NOT PG_SDL_TARGET)
  message(STATUS "pocket-groove desktop app: SDL2 not found, not built (macOS: brew install sdl2; Debian/Ubuntu: apt install libsdl2-dev)")
  return()
endif()
if(PG_FETCH_SDL2)
  message(STATUS "pocket-groove desktop app: SDL2 built from source (${PG_SDL_TARGET})")
else()
  message(STATUS "pocket-groove desktop app: SDL2 ${SDL2_VERSION} (${PG_SDL_TARGET})")
endif()

set(DESK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
# The pieces the unit tests also link (no window, no audio device).
add_library(pg_desk STATIC keymap.cpp input.cpp script.cpp)
target_include_directories(pg_desk PUBLIC ${DESK_DIR} ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pg_desk PUBLIC ${PG_SDL_TARGET})
target_compile_options(pg_desk PRIVATE ${PG_FLAGS})

add_executable(pocket-groove main.cpp audio.cpp display.cpp recorder.cpp listen.cpp strip.cpp stems.cpp lab.cpp
               samples.cpp mic.cpp mic_sdl.cpp)
target_link_libraries(pocket-groove PRIVATE pg_desk ${PG_LIBS} ${PG_LIBS} ${PG_SDL_TARGET})
target_compile_options(pocket-groove PRIVATE ${PG_FLAGS})
# Next to the other host binaries (build/host/pocket-groove).
set_target_properties(pocket-groove PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---- tests ---------------------------------------------------------------------------
add_executable(test_desktop_keymap tests/test_desktop_keymap.cpp)
target_link_libraries(test_desktop_keymap PRIVATE pg_desk ${PG_LIBS} ${PG_LIBS})
target_compile_options(test_desktop_keymap PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_keymap PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_keymap COMMAND test_desktop_keymap WORKING_DIRECTORY ${PG_ROOT})

add_executable(test_desktop_files tests/test_desktop_files.cpp)
target_link_libraries(test_desktop_files PRIVATE pg_desk ${PG_LIBS} ${PG_LIBS})
target_compile_options(test_desktop_files PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_files PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_files COMMAND test_desktop_files ${CMAKE_BINARY_DIR}/desktop_test_files
         WORKING_DIRECTORY ${PG_ROOT})

# The macro lab's logic against a fake host (lab.cpp needs no SDL calls).
add_executable(test_desktop_lab tests/test_desktop_lab.cpp lab.cpp)
target_include_directories(test_desktop_lab PRIVATE ${DESK_DIR} ${PG_SRC})
target_link_libraries(test_desktop_lab PRIVATE ${PG_LIBS} ${PG_LIBS})
target_compile_options(test_desktop_lab PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_lab PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_lab COMMAND test_desktop_lab ${CMAKE_BINARY_DIR}/desktop_test_lab_unit
         WORKING_DIRECTORY ${PG_ROOT})

# The desktop's sample folder (samples.cpp: no SDL): load, reload on change, remove, drop.
add_executable(test_desktop_samples tests/test_desktop_samples.cpp samples.cpp)
target_include_directories(test_desktop_samples PRIVATE ${DESK_DIR} ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_desktop_samples PRIVATE ${PG_LIBS} ${PG_LIBS})
target_compile_options(test_desktop_samples PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_samples PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_samples COMMAND test_desktop_samples ${CMAKE_BINARY_DIR}/desktop_test_samples
         WORKING_DIRECTORY ${PG_ROOT})

# The microphone's desktop path (mic.cpp: no SDL) with a synthetic source.
add_executable(test_desktop_mic tests/test_desktop_mic.cpp mic.cpp samples.cpp)
target_include_directories(test_desktop_mic PRIVATE ${DESK_DIR} ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_desktop_mic PRIVATE ${PG_LIBS} ${PG_LIBS})
target_compile_options(test_desktop_mic PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_mic PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_mic COMMAND test_desktop_mic ${CMAKE_BINARY_DIR}/desktop_test_mic WORKING_DIRECTORY ${PG_ROOT})

# Resampling's desktop path (host_resample.h + samples.cpp: no SDL, no input device).
find_package(Threads REQUIRED)
add_executable(test_desktop_resample tests/test_desktop_resample.cpp samples.cpp)
target_include_directories(test_desktop_resample PRIVATE ${DESK_DIR} ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_desktop_resample PRIVATE ${PG_LIBS} ${PG_LIBS} Threads::Threads)
target_compile_options(test_desktop_resample PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_resample PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_resample COMMAND test_desktop_resample ${CMAKE_BINARY_DIR}/desktop_test_resample
         WORKING_DIRECTORY ${PG_ROOT})

# Listen's rate match against simulated clocks (header-only, no SDL needed by the test itself).
add_executable(test_desktop_listen tests/test_desktop_listen.cpp)
target_include_directories(test_desktop_listen PRIVATE ${DESK_DIR})
target_compile_options(test_desktop_listen PRIVATE ${PG_FLAGS})
set_target_properties(test_desktop_listen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME test_desktop_listen COMMAND test_desktop_listen WORKING_DIRECTORY ${PG_ROOT})

# The tone in a WAV (Listen's recordings and dumps): frequency, level, THD+N (not a test by itself).
add_executable(listen_analyze tests/listen_analyze.cpp)
target_compile_options(listen_analyze PRIVATE ${PG_FLAGS})
set_target_properties(listen_analyze PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(pocket-groove listen_analyze)   # tools/pg-desktop builds it too (docs/DESKTOP.md troubleshooting)

# Reference renderer for the live-audio comparison (not a test by itself).
add_executable(desktop_audio_ref tests/desktop_audio_ref.cpp)
target_link_libraries(desktop_audio_ref PRIVATE ${PG_LIBS} ${PG_LIBS})
target_include_directories(desktop_audio_ref PRIVATE ${PG_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(desktop_audio_ref PRIVATE ${PG_FLAGS})
set_target_properties(desktop_audio_ref PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The app itself, headless (SDL's dummy video and audio drivers), against
# ui_shot, the offline engine and the transfer simulator.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET ui_shot AND TARGET xfer_sim)
  add_test(NAME test_desktop_app
           COMMAND ${Python3_EXECUTABLE} ${DESK_DIR}/tests/desktop_check.py --bin ${CMAKE_BINARY_DIR}
                   --work ${CMAKE_BINARY_DIR}/desktop_test
           WORKING_DIRECTORY ${PG_ROOT})
  set_tests_properties(test_desktop_app PROPERTIES TIMEOUT 900
                       ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
  add_dependencies(pocket-groove desktop_audio_ref)
endif()
