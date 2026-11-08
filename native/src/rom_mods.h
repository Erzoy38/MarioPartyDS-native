// File mods: files in mods\<mod name>\ replace the game's files of the same path, the way DS mods
// usually work (models, textures, pictures, sounds of a character). The ROM file itself is never
// changed; the replacement happens in memory when the game starts. No SDL here, so the headless
// program can use it too.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The mods in `folder`: its subfolders (not starting with "_"), each with its number of files.
struct ModInfo {
    std::string name;
    int files = 0;
};
std::vector<ModInfo> find_mods(const std::filesystem::path& folder);

// Replaces the game's files with those of every mod in `folder`, mods in name order (a later one
// wins). Returns how many files were replaced; `log` lists what happened.
int apply_file_mods(std::vector<std::uint8_t>& rom, const std::filesystem::path& folder, std::string& log);

// Writes every file of the game to `folder`\_game_files\, as the starting point for a mod.
// Returns how many files were written.
int extract_game_files(const std::vector<std::uint8_t>& rom, const std::filesystem::path& folder);
