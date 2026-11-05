// Runs the translated game without a window, up to a stop address, and dumps memory for
// comparison. Reads the player's ROM at run time; nothing from it is built into this file.
//
// Usage: MarioPartyDS-headless <rom.nds> --stop 0x02003000 --dump <folder> [--max-blocks N]
#include "cpu.h"
#include "hw.h"
#include "memory.h"
#include "overlay.h"

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

Cpu g_cpu;

std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

u32 header_u32(const std::vector<u8>& rom, std::size_t offset) {
    u32 value;
    std::memcpy(&value, &rom[offset], 4);
    return value;
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

// How far the game itself has come, as opposed to how long the machine has run.
constexpr u32 kGameTick = 0x02016D50;
constexpr u32 kModeState = 0x020C9660;  // s16 scene; +0xC current mode; +0xD next mode
constexpr u32 kSlotScene = 0x020CEFB4;  // s16 per overlay slot
u64 g_game_ticks = 0;
u64 g_scene_changes = 0;

void print_progress() {
    std::printf("game ticks %llu, scene changes %llu, scene %d, mode %u, next mode %u, slots %d %d\n",
                static_cast<unsigned long long>(g_game_ticks),
                static_cast<unsigned long long>(g_scene_changes), s16(rd16(kModeState)),
                rd8(kModeState + 0xC), rd8(kModeState + 0xD), s16(rd16(kSlotScene)),
                s16(rd16(kSlotScene + 2)));
}

// --watch ADDR, repeatable: counts how often a block start is reached. For finding a stuck spot.
std::map<u32, u64> g_watched;

void print_watched() {
    for (const auto& [address, count] : g_watched) {
        std::printf("watched %08x: %llu\n", address, static_cast<unsigned long long>(count));
    }
}

void watch_progress(const Cpu& c) {
    static s16 last_scene = -1;
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

void print_state(const Cpu& c, u64 blocks) {
    std::printf("after %llu blocks: pc=%08x cpsr=%08x\n", static_cast<unsigned long long>(blocks),
                c.pc, c.cpsr());
    for (int i = 0; i < 16; ++i) {
        std::printf("r%-2d=%08x%s", i, c.r[i], (i % 4 == 3) ? "\n" : " ");
    }
    std::printf("unknown memory: reads %u, writes %u\n", g_machine.unknown_reads,
                g_machine.unknown_writes);
    std::printf("code addresses on the stack:");
    int shown = 0;
    for (u32 i = 0; i < 256 && shown < 16; ++i) {
        const u32 value = rd32(c.r[13] + i * 4);
        if ((value >= 0x02000000 && value < 0x021A5000) || (value >= 0x01FF8000 && value < 0x01FFD300)) {
            std::printf(" %08x", value);
            ++shown;
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

}  // namespace

[[noreturn]] void runtime_abort(const char* reason, u32 addr) {
    std::printf("ABORT: %s at %08x\n", reason, addr);
    print_state(g_cpu, 0);
    std::exit(3);
}

void software_interrupt(Cpu& c, u32 number) { bios_swi(c, number); }

void cp15_write(Cpu&, u32 op1, u32 crn, u32 crm, u32 op2, u32 value) {
    // Caches and protection do not matter here; the TCM base and the idle wait do.
    if (op1 == 0 && crn == 9 && crm == 1 && op2 == 0) {
        g_machine.dtcm_base = value & 0xFFFFF000u;
    }
    if (op1 == 0 && crn == 7 && crm == 0 && op2 == 4) {  // wait for interrupt
        g_hw.halted = true;
    }
}

u32 cp15_read(Cpu&, u32, u32, u32, u32) { return 0; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom.nds> --stop ADDR --dump FOLDER [--max-blocks N]\n", argv[0]);
        return 2;
    }
    u32 stop = 0x02003000;
    u64 max_blocks = 2000000000ULL;
    std::string dump;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--stop")) stop = u32(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
        else if (!std::strcmp(argv[i], "--max-blocks")) max_blocks = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--watch")) g_watched[u32(std::strtoul(argv[i + 1], nullptr, 0))] = 0;
        else if (!std::strcmp(argv[i], "--dump-tick")) g_dump_ticks.insert(std::strtoull(argv[i + 1], nullptr, 0));
    }

    const std::vector<u8> rom = read_file(argv[1]);
    memory_init();
    g_machine.rom = &rom;
    const u32 rom_offset = header_u32(rom, 0x20);
    const u32 entry = header_u32(rom, 0x24);
    const u32 ram_address = header_u32(rom, 0x28);
    const u32 size = header_u32(rom, 0x2C);
    for (u32 i = 0; i < size; ++i) wr8(ram_address + i, rom[rom_offset + i]);
    for (u32 i = 0; i < 0x170; ++i) wr8(0x027FFE00u + i, rom[i]);
    wr16(0x027FFC40u, 1);  // boot type: started from a card
    wr32(0x027FFF8Cu, 0xFFFFFFFFu);  // ARM7 services ready; the stand-in serves every channel
    // X, Y (bits 10-11) and debug (bit 13) come through the ARM7; high means released. Bit 15 is
    // the closed lid, which 0x01FFC354 treats as no input.
    wr16(0x027FFFA8u, 0x2C00);
    // Boot data the firmware leaves behind, as seen in melonDS. The game compares the chip id
    // here with the card's to detect a removed card.
    for (u32 base : {0x027FF800u, 0x027FFC00u}) {
        wr32(base, 0x00001FC2u);
        wr32(base + 4, 0x00001FC2u);
    }

    Cpu& c = g_cpu;
    c.pc = entry;
    u64 blocks = 0;
    while (c.pc != stop) {
        if (blocks >= max_blocks) {
            std::printf("stopped: block limit, %llu frames, line %u, halted %d\n",
                        static_cast<unsigned long long>(g_hw.frames), g_hw.line, g_hw.halted);
            print_progress();
            print_watched();
            print_state(c, blocks);
            print_hot_blocks();
            return 4;
        }
        hw_step(c);
        ++blocks;
        if (g_hw.halted) continue;  // time passes until an interrupt wakes the CPU
        overlay_watch_calls(c);
        watch_progress(c);
        if (c.pc == kGameTick && g_dump_ticks.count(g_game_ticks) != 0) {
            write_all_dumps(dump + "/tick" + std::to_string(g_game_ticks));
            std::printf("dumped at game tick %llu\n", static_cast<unsigned long long>(g_game_ticks));
        }
        BlockFn block = bios_block(c.pc);
        int overlay_id = -1;
        if (block == nullptr) {
            if (in_overlay_area(c.pc)) {
                block = overlay_find_block(c.pc, c.thumb, &overlay_id);
                if (overlay_id < 0) runtime_abort("no overlay loaded at this address", c.pc);
            } else {
                block = find_block(c.pc | c.thumb);
            }
        }
        if (block == nullptr) {
            if (overlay_id >= 0) std::printf("MISSING %08x %d ov%d\n", c.pc, c.thumb, overlay_id);
            else std::printf("MISSING %08x %d\n", c.pc, c.thumb);
            runtime_abort(c.thumb ? "no translated Thumb code" : "no translated code", c.pc);
        }
        if ((blocks & 0xFFF) == 0) ++g_hot[c.pc];
        block(c);
    }
    std::printf("reached stop address\n");
    print_state(c, blocks);

    if (!dump.empty()) write_all_dumps(dump);
    return 0;
}
