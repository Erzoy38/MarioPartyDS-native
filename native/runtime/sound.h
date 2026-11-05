// The sound driver the ARM7 runs on the DS, rebuilt natively: the game sends commands, a sequencer
// plays its tracks on 16 channels, and a mixer makes samples.
//
// Everything the game can read back is integer work driven by counted time. The samples only go
// out to the speakers, so they cannot make players' games diverge.
#pragma once

#include "cpu.h"

#include <vector>

constexpr u32 kSoundRate = 32768;

void sound_init(const std::vector<u8>& rom);

// One word arrived on the sound channel: the address of a command chain, or 0.
void sound_receive(u32 data);

// Called once per display line.
void sound_line();

// Takes the stereo samples made since the last call.
void sound_take_samples(std::vector<s16>& out);

struct SoundStats {
    u64 notes = 0;
    u64 unknown_commands = 0;
    u32 active_channels = 0;
    u32 active_players = 0;
};
const SoundStats& sound_stats();
