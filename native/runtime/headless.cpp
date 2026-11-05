// Runs the translated game without a window, up to a stop address, and dumps memory for
// comparison. Reads the player's ROM at run time; nothing from it is built into this file.
//
// Usage: MarioPartyDS-headless <rom.nds> --stop 0x02003000 --dump <folder> [--max-blocks N]
//        [--dump-tick N]... [--watch ADDR]... [--name NAME] [--save FILE (read only)]
//        [--input TICK:WHAT[:FRAMES]]...
#define _CRT_SECURE_NO_WARNINGS  // sscanf reads the command line here, with field widths
#include "cpu.h"
#include "hw.h"
#include "machine.h"
#include "memory.h"
#include "ppu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

void write_dump(const std::filesystem::path& folder, const char* name, const std::vector<u8>& data) {
    std::ofstream out(folder / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

void write_all_dumps(const std::string& path) {
    std::filesystem::create_directories(path);
    const std::filesystem::path folder(path);
    write_dump(folder, "main_ram.bin", g_machine.main_ram);
    write_dump(folder, "itcm.bin", g_machine.itcm);
    write_dump(folder, "dtcm.bin", g_machine.dtcm);
    write_dump(folder, "palette.bin", g_machine.palette);
    write_dump(folder, "oam.bin", g_machine.oam);
    std::ofstream picture(folder / "frame.rgba", std::ios::binary);
    picture.write(reinterpret_cast<const char*>(ppu_frame()),
                  std::streamsize(kScreenWidth) * kScreenHeight * 2 * 4);
}

// --dump-tick N, repeatable: also dumps to <dump>/tickN when game tick N starts.
std::set<u64> g_dump_ticks;

// Sampled block addresses, to show where a stuck run spends its time.
std::map<u32, u32> g_hot;

void print_hot_blocks() {
    std::vector<std::pair<u32, u32>> hits;
    for (const auto& [pc, count] : g_hot) hits.push_back({count, pc});
    std::sort(hits.rbegin(), hits.rend());
    std::printf("most sampled blocks:");
    for (std::size_t i = 0; i < hits.size() && i < 10; ++i) {
        std::printf(" %08x(%u)", hits[i].second, hits[i].first);
    }
    std::printf("\n");
}

u64 g_game_ticks = 0;
u64 g_scene_changes = 0;

// How far the game itself has come, as opposed to how long the machine has run.
void print_progress() {
    std::printf("game ticks %llu, scene changes %llu, scene %d, mode %u, next mode %u, slots %d %d\n",
                static_cast<unsigned long long>(g_game_ticks),
                static_cast<unsigned long long>(g_scene_changes), s16(rd16(kModeState)),
                rd8(kModeState + 0xC), rd8(kModeState + 0xD), s16(rd16(kSlotScene)),
                s16(rd16(kSlotScene + 2)));
    std::printf("lines without 3D or capture drawn: 3D %llu, capture %llu\n",
                static_cast<unsigned long long>(ppu_gaps().lines_with_3d),
                static_cast<unsigned long long>(ppu_gaps().lines_with_capture));
}

// --watch ADDR, repeatable: counts how often a block start is reached. For finding a stuck spot.
std::map<u32, u64> g_watched;

void print_watched() {
    for (const auto& [address, count] : g_watched) {
        std::printf("watched %08x: %llu\n", address, static_cast<unsigned long long>(count));
    }
}

// --input TICK:WHAT[:FRAMES], repeatable, from game tick TICK on for FRAMES ticks (default 6).
// WHAT is a key name, or touch;X;Y in pixels. Same script, same run: this is how tests play.
struct ScriptedInput {
    u64 tick;
    u32 keys;
    bool touch;
    u32 x, y;
    u64 frames;
};
std::vector<ScriptedInput> g_script;

void add_input(const char* text) {
    static const char* const kNames[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT",
                                         "UP", "DOWN", "R", "L", "X", "Y"};
    char what[32] = {};
    unsigned long long tick = 0, frames = 6;
    if (std::sscanf(text, "%llu:%31[^:]:%llu", &tick, what, &frames) < 2) {
        std::fprintf(stderr, "bad --input %s\n", text);
        std::exit(2);
    }
    ScriptedInput input{tick, 0, false, 0, 0, frames};
    if (std::sscanf(what, "touch;%u;%u", &input.x, &input.y) == 2) {
        input.touch = true;
    } else {
        for (u32 i = 0; i < 12; ++i) {
            if (!std::strcmp(what, kNames[i])) input.keys = 1u << i;
        }
        if (input.keys == 0) {
            std::fprintf(stderr, "bad key in --input %s\n", text);
            std::exit(2);
        }
    }
    g_script.push_back(input);
}

void apply_script(u64 tick) {
    u32 keys = 0;
    bool touching = false;
    u32 x = 0, y = 0;
    for (const ScriptedInput& input : g_script) {
        if (tick < input.tick || tick >= input.tick + input.frames) continue;
        keys |= input.keys;
        if (input.touch) {
            touching = true;
            x = input.x;
            y = input.y;
        }
    }
    hw_set_keys(keys);
    hw_set_touch(touching, x, y);
}

void watch_progress(const Cpu& c) {
    static s16 last_scene = -1;
    if (c.pc == kGameTick) apply_script(g_game_ticks + 1);
    if (!g_watched.empty()) {
        const auto hit = g_watched.find(c.pc);
        if (hit != g_watched.end()) ++hit->second;
    }
    if (c.pc == kGameTick) ++g_game_ticks;
    const s16 scene = s16(rd16(kModeState));
    if (scene != last_scene) {
        last_scene = scene;
        ++g_scene_changes;
        std::printf("scene %d at game tick %llu\n", scene,
                    static_cast<unsigned long long>(g_game_ticks));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom.nds> --stop ADDR --dump FOLDER [--max-blocks N]\n", argv[0]);
        return 2;
    }
    u32 stop = 0x02003000;
    u64 max_blocks = 2000000000ULL;
    std::string dump;
    MachineSettings settings;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--stop")) stop = u32(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
        else if (!std::strcmp(argv[i], "--max-blocks")) max_blocks = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--watch")) g_watched[u32(std::strtoul(argv[i + 1], nullptr, 0))] = 0;
        else if (!std::strcmp(argv[i], "--dump-tick")) g_dump_ticks.insert(std::strtoull(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--name")) settings.player_name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--save")) settings.save = read_file(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--input")) add_input(argv[i + 1]);
    }

    const std::vector<u8> rom = read_file(argv[1]);
    const std::string problem = machine_check_rom(rom);
    if (!problem.empty()) {
        std::fprintf(stderr, "%s\n", problem.c_str());
        return 2;
    }
    machine_boot(rom, settings);

    Cpu& c = machine_cpu();
    u64 blocks = 0;
    while (c.pc != stop) {
        if (blocks >= max_blocks) {
            std::printf("stopped: block limit, %llu frames, line %u, halted %d\n",
                        static_cast<unsigned long long>(g_hw.frames), g_hw.line, g_hw.halted);
            print_progress();
            print_watched();
            std::printf("pc=%08x cpsr=%08x\n", c.pc, c.cpsr());
            print_hot_blocks();
            return 4;
        }
        ++blocks;
        watch_progress(c);
        if (c.pc == kGameTick && g_dump_ticks.count(g_game_ticks) != 0) {
            write_all_dumps(dump + "/tick" + std::to_string(g_game_ticks));
            std::printf("dumped at game tick %llu\n", static_cast<unsigned long long>(g_game_ticks));
        }
        if ((blocks & 0xFFF) == 0) ++g_hot[c.pc];
        machine_step();
    }
    std::printf("reached stop address, pc=%08x\n", c.pc);
    if (!dump.empty()) write_all_dumps(dump);
    return 0;
}
