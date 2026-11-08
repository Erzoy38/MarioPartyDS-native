// Starts the translated game and runs it block by block. Used by the window and by the test run.
#pragma once

#include "cpu.h"

#include <string>
#include <vector>

struct MachineSettings {
    std::string player_name = "Player";
    std::vector<u8> save;  // contents of the save memory at start; may be empty
    u16 boot_type = 1;     // 1 = started from the card, 2 = started by download play (a guest)
    // The firmware's language, which the game takes as its own: 1 English, 2 French, 3 German,
    // 4 Italian, 5 Spanish (the languages of this release).
    u8 language = 1;
};

// Returns an empty string, or what is wrong with this ROM.
std::string machine_check_rom(const std::vector<u8>& rom);

// The ROM has to stay alive as long as the machine runs.
void machine_boot(const std::vector<u8>& rom, const MachineSettings& settings);

Cpu& machine_cpu();

// Runs one block, including time passing and interrupts. Stops the program if code is missing.
void machine_step();

// For tests: the first time the game picks its next mode after the title it goes to `mode`.
void machine_force_mode(int mode);

// Runs until the next picture is finished.
void machine_run_frame();

// Called before the program ends because of an error. May be null.
using AbortHook = void (*)(const char* reason, u32 address);
void machine_set_abort_hook(AbortHook hook);

// Save memory. The caller decides when to write it to a file.
const std::vector<u8>& machine_save();
bool machine_save_changed();  // true once after each change

// Game tick and scene, for tests and logs.
constexpr u32 kGameTick = 0x02016D50;
constexpr u32 kModeState = 0x020C9660;  // s16 scene; +0xC current mode; +0xD next mode
constexpr u32 kSlotScene = 0x020CEFB4;  // s16 per overlay slot
