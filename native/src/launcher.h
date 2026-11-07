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
};

AppSettings load_settings(const std::filesystem::path& file);
void save_settings(const std::filesystem::path& file, const AppSettings& settings);

struct LaunchChoice {
    enum class Kind { Quit, Local, Online } kind = Kind::Quit;
    LobbyStart start;  // Online: the started room
};

// Runs the menu until the player starts a game or closes the window.
LaunchChoice run_launcher(SDL_Window* window, SDL_Renderer* renderer, AppSettings& settings,
                          const std::filesystem::path& settings_file);

// A box with two lines in the middle of the window, over the game (logical 256 x 384).
void draw_question(SDL_Renderer* renderer, const std::string& first, const std::string& second);

// Splits "host:port"; the port defaults to lobby::kDefaultPort.
void split_server(const std::string& server, std::string& address, std::uint16_t& port);
