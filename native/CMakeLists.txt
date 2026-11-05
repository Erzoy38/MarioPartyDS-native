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
