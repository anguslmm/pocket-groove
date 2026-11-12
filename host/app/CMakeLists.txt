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
  find_package(SDL2 CONFIG QUIET)
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
  message(STATUS "pocket-groove desktop app: SDL2 not found, not built (macOS: brew install sdl2)")
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

add_executable(pocket-groove main.cpp audio.cpp display.cpp recorder.cpp)
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
  set_tests_properties(test_desktop_app PROPERTIES TIMEOUT 300
                       ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
  add_dependencies(pocket-groove desktop_audio_ref)
endif()
