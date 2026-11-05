// Runs the translated game without a window, up to a stop address, and dumps memory for
// comparison. Reads the player's ROM at run time; nothing from it is built into this file.
//
// Usage: MarioPartyDS-headless <rom.nds> --stop 0x02003000 --dump <folder> [--max-blocks N]
#include "cpu.h"
#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

void print_state(const Cpu& c, u64 blocks) {
    std::printf("after %llu blocks: pc=%08x cpsr=%08x\n", static_cast<unsigned long long>(blocks),
                c.pc, c.cpsr());
    for (int i = 0; i < 16; ++i) {
        std::printf("r%-2d=%08x%s", i, c.r[i], (i % 4 == 3) ? "\n" : " ");
    }
    std::printf("unknown memory: reads %u, writes %u\n", g_machine.unknown_reads,
                g_machine.unknown_writes);
    std::fflush(stdout);
}

}  // namespace

[[noreturn]] void runtime_abort(const char* reason, u32 addr) {
    std::printf("ABORT: %s at %08x\n", reason, addr);
    print_state(g_cpu, 0);
    std::exit(3);
}

void software_interrupt(Cpu&, u32 number) {
    std::printf("software interrupt %#x\n", number);
    runtime_abort("software interrupt not implemented", g_cpu.pc);
}

void cp15_write(Cpu&, u32 op1, u32 crn, u32 crm, u32 op2, u32 value) {
    // Caches and protection do not matter here; the TCM base does.
    if (op1 == 0 && crn == 9 && crm == 1 && op2 == 0) {
        g_machine.dtcm_base = value & 0xFFFFF000u;
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
    }

    const std::vector<u8> rom = read_file(argv[1]);
    memory_init();
    const u32 rom_offset = header_u32(rom, 0x20);
    const u32 entry = header_u32(rom, 0x24);
    const u32 ram_address = header_u32(rom, 0x28);
    const u32 size = header_u32(rom, 0x2C);
    for (u32 i = 0; i < size; ++i) wr8(ram_address + i, rom[rom_offset + i]);
    for (u32 i = 0; i < 0x170; ++i) wr8(0x027FFE00u + i, rom[i]);
    wr16(0x027FFC40u, 1);  // boot type: started from a card

    Cpu& c = g_cpu;
    c.pc = entry;
    u64 blocks = 0;
    while (c.pc != stop) {
        if (blocks >= max_blocks) {
            std::printf("stopped: block limit\n");
            print_state(c, blocks);
            return 4;
        }
        const BlockFn block = find_block(c.pc | c.thumb);
        if (block == nullptr) {
            runtime_abort(c.thumb ? "no translated Thumb code" : "no translated code", c.pc);
        }
        block(c);
        ++blocks;
    }
    std::printf("reached stop address\n");
    print_state(c, blocks);

    if (!dump.empty()) {
        std::filesystem::create_directories(dump);
        const std::filesystem::path folder(dump);
        write_dump(folder, "main_ram.bin", g_machine.main_ram);
        write_dump(folder, "itcm.bin", g_machine.itcm);
        write_dump(folder, "dtcm.bin", g_machine.dtcm);
        write_dump(folder, "palette.bin", g_machine.palette);
        write_dump(folder, "oam.bin", g_machine.oam);
    }
    return 0;
}
