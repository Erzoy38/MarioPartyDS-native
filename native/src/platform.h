// What differs between Windows, Android and iOS outside the game itself: folders, files that come
// with the program, and the graphics driver. On Windows everything stays as it was.
#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace platform {

// Where the player's own folders are (textures, mods). Windows: next to the program.
// Android: the app's own storage (SDL_GetPrefPath), since the program's folder cannot be written.
// iOS: the app's Documents folder, which the Files app shows ("On My iPhone > Mario Party DS"); the
// ROM, textures and mods are put there, and save, settings and log are kept there too.
std::filesystem::path user_folder();

// The system's file picker for the ROM; the answer comes to `callback` as from SDL_ShowOpenFileDialog
// (a list with the path, an empty list when cancelled, null on an error). Windows and Android:
// SDL_ShowOpenFileDialog with `filters`. iOS: the document picker (ios_file_picker.mm), as SDL 3.4
// has none there; it shows all files.
void show_rom_picker(SDL_Window* window, SDL_DialogFileCallback callback, void* userdata,
                     const SDL_DialogFileFilter* filters, int filter_count);

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

// Phones and tablets: the game cannot be started twice in one process, so "back to the menu" starts
// the app again. Android: MainActivity.restartApp (a fresh process). False where that is not possible
// (iOS cannot start itself; Windows starts the program itself, see main.cpp).
bool restart_app();

// The game's main() has returned. iOS keeps an app running after that with nothing on screen, so
// there it ends the process; elsewhere nothing happens (the program ends as before).
void app_ended(int code);

// iPhone and iPad.
constexpr bool kIOS =
#if defined(__APPLE__) && TARGET_OS_IPHONE
    true;
#else
    false;
#endif

// Phones and tablets: a touch screen is the main input, there is one window, no own server.
constexpr bool kMobile =
#if defined(__ANDROID__)
    true;
#else
    kIOS;
#endif

}  // namespace platform
