// The menu before the game: local game, online play (create or join a room), settings.
// Drawn with SDL only; no game content is used here.
#pragma once

#include <SDL3/SDL.h>

#include "lobby.h"

#include <filesystem>
#include <string>

struct AppSettings {
    std::string name = "Spieler";
    std::string server = "127.0.0.1";  // ADDRESS[:PORT] of MarioPartyDS-server
    int scale = 2;
    bool fullscreen = false;
    int character = 0;  // lobby::character_name
    std::string rom;
    int language = 1;     // the game's language: 1 English, 2 French, 3 German, 4 Italian, 5 Spanish
    bool smooth = false;  // the DS screens smoothed when the window is larger, instead of sharp pixels
    std::string resolution;  // "1920x1080": window size or fullscreen mode; empty: automatic
    int layout = 0;          // ScreenLayout: stacked, side by side, hybrid
    int aspect = 0;          // ScreenAspect: 4:3, 16:9, fill the window
    int render_scale = 1;    // the 3D picture at 1 to 4 times the DS's resolution
    bool hd_textures = false;    // replacement textures from the texture pack
    bool dump_textures = false;  // writes the game's textures into textures\dump
    int volume = 100;              // percent
    std::string mic_device;        // a recording device's name; "" Windows' default; "off" keys only
    int mic_sensitivity = 50;      // percent: how loud counts as blowing
    bool cheat_minigames = false;  // every minigame unlocked
    bool cheat_boards = false;     // every board unlocked
    bool file_mods = false;        // files in mods\ replace the game's
    int frame_rate = 60;           // pictures shown per second: 30, 60, or 120 / 144 / 240 (synced to the display)
    int upscale = 0;               // 0 off, 1 smooth edges of the 2D pictures (Scale2x / Scale3x), 2 FSR
    int dlss = 0;                  // DlssMode: off, DLAA, Quality, Balanced, Performance, Ultra Performance
};

AppSettings load_settings(const std::filesystem::path& file);
void save_settings(const std::filesystem::path& file, const AppSettings& settings);

struct LaunchChoice {
    enum class Kind { Quit, Back, Online } kind = Kind::Quit;
    LobbyStart start;  // Online: the started room
};

// Online play (create or join a room on the lobby server) until a room starts (Online), the player
// goes back to the start screen (Back) or closes the window (Quit). Drawn in the DS's 256 x 384.
LaunchChoice run_launcher(SDL_Window* window, SDL_Renderer* renderer, AppSettings& settings,
                          const std::filesystem::path& settings_file);

// The file of server keys seen before, next to the settings.
std::filesystem::path known_servers_file(const std::filesystem::path& settings_file);

// A box with two lines in the middle of the window, over the game (logical 256 x 384).
void draw_question(SDL_Renderer* renderer, const std::string& first, const std::string& second);

// A longer text in a box, broken into lines between words; `last` (may be empty) below it.
void draw_message(SDL_Renderer* renderer, const std::string& message, const std::string& last);

// Splits "host:port"; the port defaults to lobby::kDefaultPort.
void split_server(const std::string& server, std::string& address, std::uint16_t& port);
