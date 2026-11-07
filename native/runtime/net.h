// The game's own wireless play, carried over a network instead of the DS radio.
//
// The game keeps its logic; only the libraries that move data between consoles are replaced:
// WH (connection and the per-picture exchange of every player's 0x26-byte record) and, on the
// host, MB (download play: guests are reported as having received and started the game). Guests
// start directly as download-play guests and read their files from their own ROM.
//
// Every instance blocks in the exchange until all connected players' records for that step have
// arrived, so all of them see the same records in the same order.
#pragma once

#include "cpu.h"

#include <string>

enum class NetRole { Off, Host, Guest };

struct NetSettings {
    NetRole role = NetRole::Off;
    std::string address = "127.0.0.1";
    u16 port = 47000;
    std::string name = "Player";
};

void net_start(const NetSettings& settings);

// Called before each block. Runs a replaced function instead of the game's own and returns true.
bool net_hook(Cpu& c);

// Called at the start of each game tick: delivers connection events to the game.
void net_tick();

struct NetStats {
    u64 steps = 0;
    u32 players = 0;
    u32 checksum = 0;  // over the records of the last step
};
const NetStats& net_stats();
