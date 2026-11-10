cmake_minimum_required(VERSION 3.25)
project(MarioPartyDS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The C++ runtime is linked into the programs, so they run on PCs without the Visual C++
# Redistributable; next to MarioPartyDS.exe only SDL3.dll is needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows is built with MSVC; Android (NDK) and iOS (Xcode) with Clang. See MOBILE_PORT.md.
if(MSVC)
    set(MPDS_WARNINGS /W4 /WX /utf-8)
    # Generated blocks are large in number; no optimisation keeps compile times workable for now.
    set(MPDS_GAME_CODE_OPTIONS /W0 /Od /bigobj /utf-8)
else()
    set(MPDS_WARNINGS -Wall -Wextra)
    set(MPDS_GAME_CODE_OPTIONS -w -O1)
endif()

# native/generated/ comes from tools/recomp.py and holds translated game code, so it is never
# committed.
file(GLOB GENERATED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(NOT GENERATED)
    message(FATAL_ERROR "native/generated is empty: run python tools/recomp.py first")
endif()

# The encryption of online games: Windows' own (CNG), elsewhere Mbed TLS (MOBILE_PORT.md, phase 2).
if(WIN32)
    set(MPDS_SECURE runtime/secure.cpp)
else()
    set(MPDS_SECURE runtime/secure_mbedtls.cpp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${MPDS_SECURE}")
        message(FATAL_ERROR "${MPDS_SECURE} is missing: the encryption for this platform comes in phase 2")
    endif()
endif()

add_library(runtime STATIC runtime/cpu.cpp runtime/memory.cpp runtime/hw.cpp runtime/bios.cpp
            runtime/overlay.cpp runtime/ppu.cpp runtime/gpu3d.cpp runtime/sound.cpp runtime/net.cpp runtime/lobby.cpp
            ${MPDS_SECURE} runtime/machine.cpp)
target_include_directories(runtime PUBLIC runtime)
target_compile_options(runtime PRIVATE ${MPDS_WARNINGS})
if(WIN32)
    target_link_libraries(runtime PUBLIC ws2_32 bcrypt)
endif()

add_library(game_code STATIC ${GENERATED})
target_link_libraries(game_code PUBLIC runtime)
target_compile_options(game_code PRIVATE ${MPDS_GAME_CODE_OPTIONS})

# The runtime and the generated code call each other, so the linker sees both twice.
add_executable(MarioPartyDS-headless runtime/headless.cpp src/rom_mods.cpp)
target_include_directories(MarioPartyDS-headless PRIVATE src)
target_compile_definitions(MarioPartyDS-headless PRIVATE _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
target_link_libraries(MarioPartyDS-headless PRIVATE runtime game_code runtime)
target_compile_options(MarioPartyDS-headless PRIVATE ${MPDS_WARNINGS})

# The lobby server holds no game code and no game content. It runs on Windows only.
if(WIN32)
    add_executable(MarioPartyDS-server server/server.cpp runtime/secure.cpp)
    target_include_directories(MarioPartyDS-server PRIVATE runtime)
    target_link_libraries(MarioPartyDS-server PRIVATE ws2_32 bcrypt)
    target_compile_options(MarioPartyDS-server PRIVATE ${MPDS_WARNINGS})
endif()

# The game with its window. Shared parts first, then what each platform has of its own.
set(MPDS_APP_SOURCES src/main.cpp src/launcher.cpp src/start_screen.cpp src/ui.cpp src/display.cpp
    src/microphone.cpp src/rom_mods.cpp src/upscale.cpp src/platform.cpp)
if(WIN32)
    list(APPEND MPDS_APP_SOURCES src/dlss.cpp src/text_raster_win.cpp)
else()
    list(APPEND MPDS_APP_SOURCES src/dlss_none.cpp src/text_raster_font.cpp)
endif()

if(WIN32)
    # SDL3 is the unpacked SDL3-devel-*-VC.zip in native/third_party/SDL3.
    set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3/cmake")
    find_package(SDL3 REQUIRED CONFIG)
    add_executable(MarioPartyDS WIN32 ${MPDS_APP_SOURCES})
    target_link_libraries(MarioPartyDS PRIVATE runtime game_code runtime SDL3::SDL3)
    target_compile_options(MarioPartyDS PRIVATE ${MPDS_WARNINGS})
    target_compile_definitions(MarioPartyDS PRIVATE _CRT_SECURE_NO_WARNINGS
                               _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    add_custom_command(TARGET MarioPartyDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:MarioPartyDS>)
    # NVIDIA DLSS SDK (NGX), in native/third_party/dlss (not committed; see its LICENSE.txt): headers, the
    # static-runtime library, and nvngx_dlss.dll next to the program.
    set(DLSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dlss")
    target_include_directories(MarioPartyDS PRIVATE "${DLSS_DIR}/include")
    target_link_libraries(MarioPartyDS PRIVATE "${DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" d3d11)
    add_custom_command(TARGET MarioPartyDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLSS_DIR}/bin/nvngx_dlss.dll" $<TARGET_FILE_DIR:MarioPartyDS>)

    # The start screen's picture. It shows the game's characters, so it stays local (native/assets/ is
    # not committed); without it the start screen draws plain stripes.
    set(MENU_BACKGROUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu_background.png")
    if(EXISTS "${MENU_BACKGROUND}")
        add_custom_command(TARGET MarioPartyDS POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MENU_BACKGROUND}" $<TARGET_FILE_DIR:MarioPartyDS>)
    endif()
else()
    message(FATAL_ERROR "The app for Android and iOS is set up in phases 3 and 6 (MOBILE_PORT.md)")
endif()
