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

#include <cstdint>
#include <string>
#include <vector>

enum class NetRole { Off, Host, Guest };

struct NetSettings {
    NetRole role = NetRole::Off;
    std::string address = "127.0.0.1";
    u16 port = 47000;
    std::string name = "Player";
    // Through MarioPartyDS-server: the lobby's open connection and what was already read from it
    // (LobbyStart). 0 means a direct connection to address:port.
    std::uintptr_t relay = 0;
    std::vector<u8> relay_inbox;
    // The character chosen in the lobby (lobby::character_name), or -1. When the game asks this
    // player to select a character, net_auto_touch() taps it on the character screen.
    int character = -1;
};

void net_start(const NetSettings& settings);

// Called before each block. Runs a replaced function instead of the game's own and returns true.
bool net_hook(Cpu& c);

// Called at the start of each game tick: delivers connection events to the game.
void net_tick();

struct NetStats {
    u64 steps = 0;
    u64 blocks = 0;  // port data sent (WH send)
    u32 players = 0;
    u32 checksum = 0;  // over the records of the last step
};
const NetStats& net_stats();

// Called about every 100 ms while this player waits for the others (lockstep, or a guest waiting for
// the host's menu), so a window can handle its events and show that it waits. May be null.
using NetWaitCallback = void (*)();
void net_set_wait_callback(NetWaitCallback callback);

// A touch the program makes for the player this tick (the lobby's character on the character
// screen). The window and the test run apply it instead of their own touch while it returns true.
bool net_auto_touch(u32& x, u32& y);
