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
if(NOT MSVC)
    # Clang fuses multiply and add on arm64; the 3D rasterizer then rounds differently from the
    # Windows build and phones show slightly different pixels in 3D scenes. Kept apart, as on Windows.
    set_source_files_properties(runtime/gpu3d.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
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
# Phones and tablets: the lobby server inside the app as a thread (launcher.cpp, "Server hier
# starten"). On Windows it stays the program MarioPartyDS-server.exe.
if(NOT WIN32)
    list(APPEND MPDS_APP_SOURCES server/server.cpp)
    set_source_files_properties(server/server.cpp PROPERTIES COMPILE_DEFINITIONS MPDS_SERVER_LIBRARY)
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
    target_include_directories(main PRIVATE server)
    target_link_libraries(main PRIVATE runtime game_code runtime SDL3::SDL3)
    target_compile_options(main PRIVATE ${MPDS_WARNINGS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iPhone and iPad, built on a Mac by ios/build.sh (cmake -G Xcode, then xcodebuild). SDL3 from the
    # same source as on Android, as a static library inside the app; drawing through Metal.
    enable_language(C OBJC OBJCXX)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3-src" SDL3 EXCLUDE_FROM_ALL)

    set(MPDS_IOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ios")
    set(MPDS_IOS_BUNDLE_ID "io.github.erzoy38.mariopartyds" CACHE STRING "The app's bundle identifier")
    set(MPDS_IOS_TEAM "" CACHE STRING "Apple developer team ID (10 characters) for signing")
    set(MPDS_IOS_VERSION "0.4.1" CACHE STRING "Version shown to the player (CFBundleShortVersionString)")
    set(MPDS_IOS_BUILD "2" CACHE STRING "Build number (CFBundleVersion)")
    configure_file("${MPDS_IOS_DIR}/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

    # Into the app: the icon (asset catalog), the fonts under fonts/, the start screen's picture if it
    # is here (it stays local, see the Windows part). platform::bundled_file looks in the bundle.
    set(MPDS_IOS_ASSETS "${MPDS_IOS_DIR}/Assets.xcassets")
    file(GLOB MPDS_IOS_FONTS "${MPDS_FONTS_DIR}/*.ttf" "${MPDS_FONTS_DIR}/OFL.txt")
    set_source_files_properties(${MPDS_IOS_FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION "fonts")
    set(MPDS_IOS_RESOURCES "${MPDS_IOS_ASSETS}")
    set(MENU_BACKGROUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu_background.png")
    if(EXISTS "${MENU_BACKGROUND}")
        list(APPEND MPDS_IOS_RESOURCES "${MENU_BACKGROUND}")
    endif()

    # The ROM picker: SDL 3.4 has no file dialog on iOS, so the system's document picker (UIKit).
    add_executable(MarioPartyDS MACOSX_BUNDLE ${MPDS_APP_SOURCES} src/ios_file_picker.mm
                   ${MPDS_IOS_RESOURCES} ${MPDS_IOS_FONTS})
    target_include_directories(MarioPartyDS PRIVATE server)
    target_link_libraries(MarioPartyDS PRIVATE runtime game_code runtime SDL3::SDL3-static
                          "-framework UIKit" "-framework UniformTypeIdentifiers")
    target_compile_options(MarioPartyDS PRIVATE ${MPDS_WARNINGS})
    set_target_properties(MarioPartyDS PROPERTIES
        OUTPUT_NAME "MarioPartyDS"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        RESOURCE "${MPDS_IOS_RESOURCES}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MPDS_IOS_BUNDLE_ID}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MPDS_IOS_TEAM}"
        XCODE_ATTRIBUTE_MARKETING_VERSION "${MPDS_IOS_VERSION}"
        XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "${MPDS_IOS_BUILD}"
        XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym"
        XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES")
else()
    message(STATUS "No app for this system (Windows, Android and iOS have one); building the runtime only")
endif()
