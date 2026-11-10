// What differs between Windows, Android and iOS outside the game itself: folders, files that come
// with the program, and the graphics driver. On Windows everything stays as it was.
#pragma once

#include <filesystem>
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace platform {

// Where the player's own folders are (textures, mods). Windows: next to the program.
// Android/iOS: the app's own storage (SDL_GetPrefPath), since the program's folder cannot be written.
std::filesystem::path user_folder();

// A file that comes with the program (menu_background.png, fonts), for SDL_IOFromFile/SDL_LoadPNG.
// Windows and iOS: in the program's folder or bundle. Android: inside the APK, by relative name.
std::string bundled_file(const std::string& name);

// The ROM the player chose, made lasting. Windows: the file stays where it is. Android/iOS: the
// picker's address (content://, or a file only lent to the app) works only for a while, so the ROM is
// copied once into the app's own storage and that copy's path is returned. Empty, with `error`, if
// it cannot be read or kept.
std::string keep_rom(const std::string& chosen, std::string& error);

// The SDL render driver to try first; if it fails, SDL chooses. Windows: Direct3D 11 (DLSS needs it),
// Android: Vulkan, iOS: Metal.
const char* preferred_render_driver();

// Phones and tablets: a touch screen is the main input, there is one window, no own server.
constexpr bool kMobile =
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    true;
#else
    false;
#endif

}  // namespace platform
