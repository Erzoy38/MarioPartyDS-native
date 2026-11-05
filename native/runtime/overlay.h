// Overlays share addresses in RAM, so a translated block only belongs to an address while its
// overlay is loaded. The game loads and unloads them through two functions; those calls decide.
#pragma once

#include "cpu.h"

struct OverlayBlock {
    u32 key;     // address | thumb
    BlockFn fn;
    u32 first;   // first instruction, compared with RAM before the block runs
};

struct OverlayInfo {
    u32 id, start, end;
    const OverlayBlock* blocks;
    u32 count;
};

extern const OverlayInfo kOverlays[];  // generated
extern const u32 kOverlayCount;        // generated

// Lowest address of any overlay and the end of the area, from the game's memory layout.
constexpr u32 kOverlayAreaStart = 0x021638A0;
constexpr u32 kOverlayAreaEnd = 0x021A5000;

// Call before each block: notices the game's load and unload calls.
void overlay_watch_calls(const Cpu& c);

inline bool in_overlay_area(u32 pc) { return pc >= kOverlayAreaStart && pc < kOverlayAreaEnd; }

// Finds the block of the loaded overlay covering the address. *overlay_id becomes -1 if no
// loaded overlay covers it. Aborts if the code in RAM is not what was translated.
BlockFn overlay_find_block(u32 pc, u32 thumb, int* overlay_id);
