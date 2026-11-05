// The ARM9's address space as the game sees it. Phase 2: enough to run the startup code.
#pragma once

#include "cpu.h"

#include <cstddef>
#include <vector>

struct Machine {
    std::vector<u8> main_ram;  // 4 MiB at 0x02000000, mirrored up to 0x03000000
    std::vector<u8> itcm;      // 32 KiB, mirrored below 0x02000000
    std::vector<u8> dtcm;      // 16 KiB at dtcm_base
    std::vector<u8> palette;   // 2 KiB at 0x05000000
    std::vector<u8> oam;       // 2 KiB at 0x07000000
    std::vector<u8> vram;      // 656 KiB; bank mapping comes with the display phase
    std::vector<u8> io;        // 64 KiB of plain storage for registers not handled yet
    const std::vector<u8>* rom = nullptr;  // the player's ROM, read through the card registers
    u32 dtcm_base = 0x027E0000;
    u32 unknown_reads = 0;
    u32 unknown_writes = 0;
};

extern Machine g_machine;

void memory_init();
