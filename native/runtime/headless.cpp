// Runs the translated game without a window, up to a stop address, and dumps memory for
// comparison. Reads the player's ROM at run time; nothing from it is built into this file.
//
// Usage: MarioPartyDS-headless <rom.nds> --stop 0x02003000 --dump <folder> [--max-blocks N]
//        [--dump-tick N]... [--watch ADDR]... [--name NAME] [--save FILE (read only)]
//        [--input TICK:WHAT[:FRAMES]]...
#define _CRT_SECURE_NO_WARNINGS  // sscanf reads the command line here, with field widths
#include "cpu.h"
#include "gpu3d.h"
#include "hw.h"
#include "machine.h"
#include "memory.h"
#include "net.h"
#include "ppu.h"
#include "sound.h"

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

// --wav FILE: writes the sound of the whole run, to hear and check it.
std::string g_wav_path;
std::vector<s16> g_wav_samples;
std::vector<s16> g_wav_chunk;

void collect_sound() {
    sound_take_samples(g_wav_chunk);
    g_wav_samples.insert(g_wav_samples.end(), g_wav_chunk.begin(), g_wav_chunk.end());
}

void write_wav() {
    if (g_wav_path.empty()) return;
    std::ofstream out(g_wav_path, std::ios::binary);
    const u32 bytes = u32(g_wav_samples.size() * 2);
    const auto put32 = [&](u32 v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](u16 v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); put32(36 + bytes); out.write("WAVEfmt ", 8); put32(16);
    put16(1); put16(2); put32(kSoundRate); put32(kSoundRate * 4); put16(4); put16(16);
    out.write("data", 4); put32(bytes);
    out.write(reinterpret_cast<const char*>(g_wav_samples.data()), bytes);
    std::printf("wrote %s: %.1f seconds\n", g_wav_path.c_str(), double(g_wav_samples.size()) / 2 / kSoundRate);
}

// --dump-tick N, repeatable: also dumps to <dump>/tickN when game tick N starts.
std::set<u64> g_dump_ticks;

// --frame-range FROM:TO, repeatable: writes only the picture at each game tick in the range, to
// <dump>/frames/N.rgba. For finding the frame that matches a screenshot.
std::vector<std::pair<u64, u64>> g_frame_ranges;

void write_frame_if_wanted(const std::string& dump, u64 tick) {
    for (const auto& [from, to] : g_frame_ranges) {
        if (tick < from || tick > to) continue;
        const std::filesystem::path folder = std::filesystem::path(dump) / "frames";
        std::filesystem::create_directories(folder);
        std::ofstream picture(folder / (std::to_string(tick) + ".rgba"), std::ios::binary);
        picture.write(reinterpret_cast<const char*>(ppu_frame()),
                      std::streamsize(kScreenWidth) * kScreenHeight * 2 * 4);
        return;
    }
}

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
    std::printf("3D lines %llu, lists drawn %llu, last list %u polygons %u vertices, unknown 3D commands %llu\n",
                static_cast<unsigned long long>(ppu_gaps().lines_with_3d),
                static_cast<unsigned long long>(gpu3d_stats().lists_drawn), gpu3d_stats().last_polygons,
                gpu3d_stats().last_vertices, static_cast<unsigned long long>(gpu3d_stats().unknown_commands));
    std::printf("lines captured: %llu\n",
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
// WHAT is a key name, MIC for blowing into the microphone, or touch;X;Y in pixels. Same script, same run: this is how tests play.
constexpr u32 kMicBit = 1u << 16;

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
    } else if (!std::strcmp(what, "MIC")) {
        input.keys = kMicBit;
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
    hw_set_keys(keys & 0xFFF);
    hw_set_mic(keys & kMicBit);
    hw_set_touch(touching, x, y);
}

// --force-mode MODE: the first time the game picks its next mode after the title (mode 0x86, the
// save select) it goes to MODE instead. A shortcut for tests; the state a real menu would have set
// up is missing, so a scene reached this way may behave differently.
int g_force_mode = -1;
constexpr u32 kSetNextMode = 0x0201A3AC;

void apply_forced_mode(Cpu& c) {
    if (g_force_mode >= 0 && c.pc == kSetNextMode && c.r[0] == 0x86) {
        std::printf("forcing mode %#x\n", g_force_mode);
        c.r[0] = u32(g_force_mode);
        g_force_mode = -1;
    }
}

void watch_progress(const Cpu& c) {
    static s16 last_scene = -1;
    if (c.pc == kGameTick) apply_script(g_game_ticks + 1);
    if (!g_watched.empty()) {
        const auto hit = g_watched.find(c.pc);
        if (hit != g_watched.end() && hit->second++ == 0) {
            std::printf("watched %08x first reached at game tick %llu\n", c.pc, static_cast<unsigned long long>(g_game_ticks));
        }
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a process stopped from outside still leaves its log
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom.nds> --stop ADDR --dump FOLDER [--max-blocks N]\n", argv[0]);
        return 2;
    }
    u32 stop = 0x02003000;
    u64 max_blocks = 2000000000ULL;
    std::string dump;
    MachineSettings settings;
    NetSettings net;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--stop")) stop = u32(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
        else if (!std::strcmp(argv[i], "--max-blocks")) max_blocks = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--watch")) g_watched[u32(std::strtoul(argv[i + 1], nullptr, 0))] = 0;
        else if (!std::strcmp(argv[i], "--dump-tick")) g_dump_ticks.insert(std::strtoull(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--frame-range")) {
            unsigned long long from = 0, to = 0;
            if (std::sscanf(argv[i + 1], "%llu:%llu", &from, &to) == 2) g_frame_ranges.push_back({from, to});
        }
        else if (!std::strcmp(argv[i], "--wav")) g_wav_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--net-host")) { net.role = NetRole::Host; net.port = u16(std::atoi(argv[i + 1])); }
        else if (!std::strcmp(argv[i], "--net-guest")) { net.role = NetRole::Guest; net.port = u16(std::atoi(argv[i + 1])); }
        else if (!std::strcmp(argv[i], "--force-mode")) g_force_mode = int(std::strtol(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--name")) settings.player_name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--boot-type")) settings.boot_type = u16(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--save")) settings.save = read_file(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--input")) add_input(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--repeat")) {
            // FROM:TO:STEP:WHAT[:FRAMES], the same as many --input
            unsigned long long from = 0, to = 0, step = 1;
            int used = 0;
            if (std::sscanf(argv[i + 1], "%llu:%llu:%llu:%n", &from, &to, &step, &used) == 3 && step > 0) {
                for (unsigned long long tick = from; tick <= to; tick += step) {
                    add_input((std::to_string(tick) + ":" + (argv[i + 1] + used)).c_str());
                }
            }
        }
    }

    const std::vector<u8> rom = read_file(argv[1]);
    const std::string problem = machine_check_rom(rom);
    if (!problem.empty()) {
        std::fprintf(stderr, "%s\n", problem.c_str());
        return 2;
    }
    machine_set_abort_hook([](const char*, u32) {
        print_progress();
        print_hot_blocks();
    });
    machine_boot(rom, settings);
    net.name = settings.player_name;
    net_start(net);

    Cpu& c = machine_cpu();
    u64 blocks = 0;
    while (c.pc != stop) {
        if (blocks >= max_blocks) {
            std::printf("stopped: block limit, %llu frames, line %u, halted %d\n",
                        static_cast<unsigned long long>(g_hw.frames), g_hw.line, g_hw.halted);
            print_progress();
            print_watched();
            std::printf("sound: %llu notes, %u players and %u channels active, %llu commands not built\n",
                        static_cast<unsigned long long>(sound_stats().notes), sound_stats().active_players,
                        sound_stats().active_channels, static_cast<unsigned long long>(sound_stats().unknown_commands));
            write_wav();
            std::printf("net: %llu steps, %u players, last checksum %08x\n",
                        static_cast<unsigned long long>(net_stats().steps), net_stats().players, net_stats().checksum);
            std::printf("pc=%08x cpsr=%08x lr=%08x sp=%08x stack:", c.pc, c.cpsr(), c.r[14], c.r[13]);
            for (u32 i = 0, shown = 0; i < 128 && shown < 12; ++i) {
                const u32 value = rd32(c.r[13] + i * 4);
                if (value >= 0x02000000 && value < 0x021A5000 && (value & 3) == 0) {
                    std::printf(" %08x", value);
                    ++shown;
                }
            }
            std::printf("\n");
            print_hot_blocks();
            return 4;
        }
        ++blocks;
        apply_forced_mode(c);
        watch_progress(c);
        if (!g_wav_path.empty() && (blocks & 0xFFFF) == 0) collect_sound();
        if (c.pc == kGameTick && g_dump_ticks.count(g_game_ticks) != 0) {
            write_all_dumps(dump + "/tick" + std::to_string(g_game_ticks));
            std::printf("dumped at game tick %llu\n", static_cast<unsigned long long>(g_game_ticks));
            gpu3d_print_last_list();
        }
        if (c.pc == kGameTick) write_frame_if_wanted(dump, g_game_ticks);
        if ((blocks & 0xFFF) == 0) ++g_hot[c.pc];
        machine_step();
    }
    std::printf("reached stop address, pc=%08x\n", c.pc);
    if (!dump.empty()) write_all_dumps(dump);
    return 0;
}
