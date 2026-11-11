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
    # u8path is deprecated in C++20 but still the plain way to a UTF-8 path; MSVC is told so by define.
    set(MPDS_WARNINGS -Wall -Wextra -Wno-deprecated-declarations -Wno-missing-field-initializers)
    set(MPDS_GAME_CODE_OPTIONS -w -O1)
endif()

# native/generated/ comes from tools/recomp.py and holds translated game code, so it is never
# committed.
file(GLOB GENERATED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(NOT GENERATED)
    message(FATAL_ERROR "native/generated is empty: run python tools/recomp.py first")
endif()

# The encryption of online games: Windows' own (CNG), elsewhere Mbed TLS. -DMPDS_CRYPTO=mbedtls on
# Windows builds the Mbed TLS side here too, to test it against the Windows server.
if(WIN32)
    set(MPDS_CRYPTO cng CACHE STRING "cng or mbedtls")
else()
    set(MPDS_CRYPTO mbedtls CACHE STRING "mbedtls")
endif()
if(MPDS_CRYPTO STREQUAL "mbedtls")
    enable_language(C)
    set(MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-3.6.4")
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(GEN_FILES OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)  # Mbed TLS's older cmake_minimum_required: keep the static runtime
    add_subdirectory("${MBEDTLS_DIR}" mbedtls EXCLUDE_FROM_ALL)
    set(MPDS_SECURE runtime/secure.cpp runtime/secure_mbedtls.cpp)
    set(MPDS_CRYPTO_LIBS MbedTLS::mbedcrypto)
else()
    set(MPDS_SECURE runtime/secure.cpp runtime/secure_cng.cpp)
    set(MPDS_CRYPTO_LIBS bcrypt)
endif()

add_library(runtime STATIC runtime/cpu.cpp runtime/memory.cpp runtime/hw.cpp runtime/bios.cpp
            runtime/overlay.cpp runtime/ppu.cpp runtime/gpu3d.cpp runtime/sound.cpp runtime/net.cpp runtime/lobby.cpp
            ${MPDS_SECURE} runtime/machine.cpp)
target_include_directories(runtime PUBLIC runtime)
target_compile_options(runtime PRIVATE ${MPDS_WARNINGS})
target_link_libraries(runtime PUBLIC ${MPDS_CRYPTO_LIBS})
if(WIN32)
    target_link_libraries(runtime PUBLIC ws2_32)
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
    add_executable(MarioPartyDS-server server/server.cpp runtime/secure.cpp runtime/secure_cng.cpp)
    target_include_directories(MarioPartyDS-server PRIVATE runtime)
    target_link_libraries(MarioPartyDS-server PRIVATE ws2_32 bcrypt)
    target_compile_options(MarioPartyDS-server PRIVATE ${MPDS_WARNINGS})
endif()

# The game with its window. Shared parts first, then what each platform has of its own.
set(MPDS_APP_SOURCES src/main.cpp src/launcher.cpp src/start_screen.cpp src/ui.cpp src/display.cpp
    src/microphone.cpp src/rom_mods.cpp src/upscale.cpp src/platform.cpp src/touch_controls.cpp)
# Text: Windows' fonts through GDI, elsewhere Noto Sans with stb_truetype. -DMPDS_TEXT=font on
# Windows shows the phones' text here, to compare.
if(WIN32)
    set(MPDS_TEXT gdi CACHE STRING "gdi or font")
    list(APPEND MPDS_APP_SOURCES src/dlss.cpp)
else()
    set(MPDS_TEXT font CACHE STRING "font")
    list(APPEND MPDS_APP_SOURCES src/dlss_none.cpp)
endif()
if(MPDS_TEXT STREQUAL "font")
    list(APPEND MPDS_APP_SOURCES src/text_raster_font.cpp)
else()
    list(APPEND MPDS_APP_SOURCES src/text_raster_win.cpp)
endif()
set(MPDS_FONTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts")

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
    if(MPDS_TEXT STREQUAL "font")
        add_custom_command(TARGET MarioPartyDS POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${MPDS_FONTS_DIR}" $<TARGET_FILE_DIR:MarioPartyDS>/fonts)
    endif()
    set(MENU_BACKGROUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu_background.png")
    if(EXISTS "${MENU_BACKGROUND}")
        add_custom_command(TARGET MarioPartyDS POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MENU_BACKGROUND}" $<TARGET_FILE_DIR:MarioPartyDS>)
    endif()
elseif(ANDROID)
    # SDL3 from its source (third_party/SDL3-src, the same 3.4.20 as on Windows) as libSDL3.so, the game
    # as libmain.so: the two libraries SDL's Java side (SDLActivity) loads. Built by android/ (Gradle).
    enable_language(C)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3-src" SDL3 EXCLUDE_FROM_ALL)
    add_library(main SHARED ${MPDS_APP_SOURCES})
    target_link_libraries(main PRIVATE runtime game_code runtime SDL3::SDL3)
    target_compile_options(main PRIVATE ${MPDS_WARNINGS})
else()
    message(STATUS "The app for iOS comes in phase 6 (MOBILE_PORT.md); building the runtime only")
endif()
