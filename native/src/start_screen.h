// The first screen of the program, before any ROM is loaded: start (or choose the ROM image),
// settings, online play, quit. Landscape, drawn in a logical 1024 x 576.
#pragma once

#include <SDL3/SDL.h>

#include "launcher.h"

#include <filesystem>
#include <functional>
#include <string>

constexpr int kStartWidth = 1024;
constexpr int kStartHeight = 576;

struct StartChoice {
    enum class Kind { Quit, Local, Online, Rejoin } kind = Kind::Quit;
};

// Runs until the player starts something or closes the window. `rom_path` is the ROM to start (may
// be empty; set when one is chosen). `check_rom` returns what is wrong with a ROM file, or nothing.
// `rejoin_room` (may be empty) is a game this program left without ending it. `notice` (may be
// empty) is shown at the bottom, for example why the last game ended.
StartChoice run_start_screen(SDL_Window* window, SDL_Renderer* renderer, AppSettings& settings,
                             const std::filesystem::path& settings_file, std::string& rom_path,
                             const std::function<std::string(const std::string&)>& check_rom,
                             const std::string& rejoin_room, const std::string& notice);
