#include "rom_mods.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

u32 get32(const std::vector<u8>& rom, size_t at) {
    if (at + 4 > rom.size()) return 0;
    u32 v;
    std::memcpy(&v, &rom[at], 4);
    return v;
}
u16 get16(const std::vector<u8>& rom, size_t at) {
    if (at + 2 > rom.size()) return 0;
    u16 v;
    std::memcpy(&v, &rom[at], 2);
    return v;
}
void put32(std::vector<u8>& rom, size_t at, u32 v) { std::memcpy(&rom[at], &v, 4); }

// The game's file system: path (with '/') -> file id, from the name table (FNT).
void walk(const std::vector<u8>& rom, u32 fnt, u32 dir, const std::string& prefix, std::map<std::string, u16>& out,
          int depth) {
    if (depth > 32) return;
    const size_t entry = fnt + (dir & 0xFFF) * 8;
    size_t at = fnt + get32(rom, entry);
    u16 id = get16(rom, entry + 4);
    while (at < rom.size()) {
        const u8 head = rom[at++];
        if (head == 0) break;
        const size_t length = head & 0x7F;
        if (at + length > rom.size()) break;
        const std::string name(reinterpret_cast<const char*>(&rom[at]), length);
        at += length;
        if (head & 0x80) {
            const u16 sub = get16(rom, at);
            at += 2;
            walk(rom, fnt, sub, prefix + name + "/", out, depth + 1);
        } else {
            out[prefix + name] = id++;
        }
    }
}

std::map<std::string, u16> game_files(const std::vector<u8>& rom) {
    std::map<std::string, u16> files;
    const u32 fnt = get32(rom, 0x40);
    if (fnt == 0 || fnt >= rom.size()) return files;
    walk(rom, fnt, 0xF000, "", files, 0);
    return files;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace

std::vector<ModInfo> find_mods(const std::filesystem::path& folder) {
    std::vector<ModInfo> mods;
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return mods;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_directory()) continue;
        const std::string name = utf8(entry.path().filename());
        if (name.empty() || name[0] == '_') continue;  // _game_files and the like are not mods
        ModInfo mod{name, 0};
        for (auto it = std::filesystem::recursive_directory_iterator(entry.path(), error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file()) ++mod.files;
        }
        mods.push_back(mod);
    }
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) { return a.name < b.name; });
    return mods;
}

int apply_file_mods(std::vector<u8>& rom, const std::filesystem::path& folder, std::string& log) {
    const std::map<std::string, u16> files = game_files(rom);
    std::map<std::string, u16> by_lower;
    for (const auto& [path, id] : files) by_lower[lower(path)] = id;
    const u32 fat = get32(rom, 0x48);
    int replaced = 0;
    for (const ModInfo& mod : find_mods(folder)) {
        const std::filesystem::path root = folder / std::filesystem::u8path(mod.name);
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(root, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (!it->is_regular_file()) continue;
            std::string relative = utf8(std::filesystem::relative(it->path(), root, error));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            const auto found = by_lower.find(lower(relative));
            if (found == by_lower.end()) {
                log += "  " + mod.name + ": " + relative + " is not a file of the game, ignored\n";
                continue;
            }
            std::ifstream in(it->path(), std::ios::binary);
            const std::vector<u8> data((std::istreambuf_iterator<char>(in)), {});
            // The new file goes to the end of the image; the game finds it through its table.
            size_t start = (rom.size() + 0x1FF) & ~size_t(0x1FF);
            rom.resize(start, 0xFF);
            rom.insert(rom.end(), data.begin(), data.end());
            put32(rom, fat + size_t(found->second) * 8, u32(start));
            put32(rom, fat + size_t(found->second) * 8 + 4, u32(start + data.size()));
            log += "  " + mod.name + ": " + relative + " (" + std::to_string(data.size()) + " bytes)\n";
            ++replaced;
        }
    }
    return replaced;
}

int extract_game_files(const std::vector<u8>& rom, const std::filesystem::path& folder) {
    const u32 fat = get32(rom, 0x48);
    const std::filesystem::path out = folder / "_game_files";
    int written = 0;
    for (const auto& [path, id] : game_files(rom)) {
        const u32 start = get32(rom, fat + size_t(id) * 8), end = get32(rom, fat + size_t(id) * 8 + 4);
        if (end < start || end > rom.size()) continue;
        const std::filesystem::path file = out / std::filesystem::u8path(path);
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(&rom[start]), std::streamsize(end - start));
        ++written;
    }
    return written;
}
