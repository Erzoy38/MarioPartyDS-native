cmake_minimum_required(VERSION 3.25)
project(MarioPartyDS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Phase 0: a plain Win32 window to prove the toolchain. It contains no game code.
add_executable(MarioPartyDS WIN32 src/main.cpp)
target_compile_options(MarioPartyDS PRIVATE /W4 /WX /utf-8)
target_compile_definitions(MarioPartyDS PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MarioPartyDS PRIVATE user32 gdi32)

# Phase 2: the translated game without a window. native/generated/ comes from tools/recomp.py
# and holds translated game code, so it is never committed.
file(GLOB GENERATED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(GENERATED)
    add_library(runtime STATIC runtime/cpu.cpp runtime/memory.cpp)
    target_include_directories(runtime PUBLIC runtime)
    target_compile_options(runtime PRIVATE /W4 /WX /utf-8)

    add_library(game_code STATIC ${GENERATED})
    target_link_libraries(game_code PUBLIC runtime)
    # Generated blocks are large in number; no optimisation keeps compile times workable for now.
    target_compile_options(game_code PRIVATE /W0 /Od /bigobj /utf-8)

    add_executable(MarioPartyDS-headless runtime/headless.cpp)
    target_link_libraries(MarioPartyDS-headless PRIVATE game_code runtime)
    target_compile_options(MarioPartyDS-headless PRIVATE /W4 /WX /utf-8)
endif()
