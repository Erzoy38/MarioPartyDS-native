cmake_minimum_required(VERSION 3.25)
project(MarioPartyDS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# native/generated/ comes from tools/recomp.py and holds translated game code, so it is never
# committed. SDL3 is the unpacked SDL3-devel-*-VC.zip in native/third_party/SDL3.
file(GLOB GENERATED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(NOT GENERATED)
    message(FATAL_ERROR "native/generated is empty: run python tools/recomp.py first")
endif()

add_library(runtime STATIC runtime/cpu.cpp runtime/memory.cpp runtime/hw.cpp runtime/bios.cpp
            runtime/overlay.cpp runtime/ppu.cpp runtime/machine.cpp)
target_include_directories(runtime PUBLIC runtime)
target_compile_options(runtime PRIVATE /W4 /WX /utf-8)

add_library(game_code STATIC ${GENERATED})
target_link_libraries(game_code PUBLIC runtime)
# Generated blocks are large in number; no optimisation keeps compile times workable for now.
target_compile_options(game_code PRIVATE /W0 /Od /bigobj /utf-8)

# The runtime and the generated code call each other, so the linker sees both twice.
add_executable(MarioPartyDS-headless runtime/headless.cpp)
target_link_libraries(MarioPartyDS-headless PRIVATE runtime game_code runtime)
target_compile_options(MarioPartyDS-headless PRIVATE /W4 /WX /utf-8)

set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3/cmake")
find_package(SDL3 REQUIRED CONFIG)
add_executable(MarioPartyDS WIN32 src/main.cpp)
target_link_libraries(MarioPartyDS PRIVATE runtime game_code runtime SDL3::SDL3)
target_compile_options(MarioPartyDS PRIVATE /W4 /WX /utf-8)
target_compile_definitions(MarioPartyDS PRIVATE _CRT_SECURE_NO_WARNINGS
                           _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
add_custom_command(TARGET MarioPartyDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:MarioPartyDS>)
