// Timing, interrupts and registers with behaviour. Everything advances by counted steps, not
// real time, so a run is exactly repeatable. Network play depends on that.
#pragma once

#include "cpu.h"

#include <vector>

// Blocks per scanline: a stand-in until the timing is compared with melonDS.
constexpr u32 kBlocksPerLine = 200;
constexpr u32 kLinesPerFrame = 263;
constexpr u32 kFirstVBlankLine = 192;

namespace irq {
constexpr u32 kVBlank = 1u << 0;
constexpr u32 kHBlank = 1u << 1;
constexpr u32 kVCount = 1u << 2;
constexpr u32 kTimer0 = 1u << 3;
constexpr u32 kIpcSync = 1u << 16;
constexpr u32 kFifoSendEmpty = 1u << 17;
constexpr u32 kFifoRecv = 1u << 18;
}

struct HwState {
    u32 line = 0;
    u32 line_blocks = 0;
    u64 frames = 0;
    bool halted = false;
    u64 reads_unknown_io = 0;
};
extern HwState g_hw;

// Called between blocks. Returns true if an interrupt was taken.
void hw_step(Cpu& c);
bool hw_read(u32 addr, u32 size, u32& value);   // true if handled
bool hw_write(u32 addr, u32 size, u32 value);   // true if handled
void hw_raise(u32 flags);

// What the player does. Keys use the DS order: A, B, Select, Start, Right, Left, Up, Down, R, L,
// X, Y from bit 0, set while pressed. Touch positions are screen pixels.
void hw_set_keys(u32 pressed);
void hw_set_touch(bool touching, u32 x, u32 y);
void hw_set_mic(bool blowing);  // blowing into the microphone, as one on/off input
void hw_set_mic_level(float level);  // how loud the microphone hears: 0 silent to 1 full
// Save memory: set at start, read to write it to a file. The flag clears when read.
void hw_set_save(const std::vector<u8>& contents);
const std::vector<u8>& hw_save();
bool hw_save_changed();
inline constexpr bool kHwDeterministic = true;

// Pseudo code standing in for the ARM9 BIOS at 0xFFFF0000.
BlockFn bios_block(u32 pc);
void bios_swi(Cpu& c, u32 number);
