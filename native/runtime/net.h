// The game's own wireless play, carried over a network instead of the DS radio.
//
// The game keeps its logic; only the libraries that move data between consoles are replaced:
// WH (connection and the per-picture exchange of every player's 0x26-byte record) and, on the
// host, MB (download play: guests are reported as having received and started the game). Guests
// start directly as download-play guests and read their files from their own ROM.
//
// Every instance takes the same steps: step N holds every connected player's record of their own
// N-th picture in the exchange, so all of them see the same records in the same order. With an input
// delay of D pictures a player's record is used D pictures after it was made; the first D pictures of
// the exchange carry no step, as the DS radio's own data sharing has no data for its first pictures,
// and the game waits like it does there. The delay hides the time the records travel.
//
// Messages are numbered and kept until the other side confirms them. A broken connection is opened
// again and the missing messages are sent once more, so a short drop or a restarted server costs a
// pause but no step. A player who stays away longer than `give_up_seconds` ends the game for all.
//
// A guest whose program ended (closed, crashed) can come back too: the host keeps every message it
// sent to it, the new program boots and plays the game so far again from them (every instance
// computes the same from the same messages), and it does not send again what the host already has.
// Meanwhile the others wait. The host itself cannot come back this way.
#pragma once

#include "cpu.h"
#include "secure.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NetRole { Off, Host, Guest };

struct NetSettings {
    NetRole role = NetRole::Off;
    std::string address = "127.0.0.1";  // the host, or the server with `relay`
    u16 port = 47000;
    std::string name = "Player";
    // Through MarioPartyDS-server: the lobby's open connection and what was already read from it
    // (LobbyStart). 0 means a direct connection to address:port.
    std::uintptr_t relay = 0;
    std::vector<u8> relay_inbox;
    std::shared_ptr<secure::Channel> relay_channel;  // its encryption (LobbyStart::channel)
    std::vector<u8> relay_outbox;                    // already encrypted, not sent yet
    std::vector<u8> server_key;                      // a new connection to the server must show this key
    // The character chosen in the lobby (lobby::character_name), or -1. When the game asks this
    // player to select a character, net_auto_touch() taps it on the character screen.
    int character = -1;
    // Coming back into the room after a broken connection (lobby::kResume).
    std::string code;
    int slot = 0;
    std::uint64_t token = 0;
    std::array<std::uint64_t, 4> tokens{};  // the host: every slot's
    int players = 0;
    int input_delay = 0;        // pictures
    int give_up_seconds = 120;  // waiting for a player whose connection broke
    bool via_server = false;    // through the server without an open lobby connection (coming back)
    // A guest writes what it needs to come back into `rejoin_file` (removed when the game ends on
    // purpose); `rejoin` starts from such a file (net_load_rejoin) instead of joining anew.
    std::string rejoin_file;
    bool rejoin = false;
};

// Reads a file written through NetSettings::rejoin_file. False if it is missing or broken.
bool net_load_rejoin(const std::string& path, NetSettings& settings);

void net_start(const NetSettings& settings);
// Back to the state at program start, connections closed, for machine_reset.
void net_reset();

// Called before each block. Runs a replaced function instead of the game's own and returns true.
bool net_hook(Cpu& c);

// Called at the start of each game tick: delivers connection events to the game.
void net_tick();

struct NetStats {
    u64 steps = 0;
    u64 blocks = 0;  // port data sent (WH send)
    u32 players = 0;
    u32 checksum = 0;  // over the records of the last step
    // Round trips in ms through the game's own connection: the host to each guest by player
    // number, a guest to the host in [0]. 0 = not measured. Measured where messages are read, which
    // happens once per picture, so they may be up to one picture (17 ms) too long.
    std::array<u32, 4> ping_ms{};
    u32 server_ping_ms = 0;  // through the server: this player's round trip to it
    u64 reconnects = 0;      // connections opened again
    u64 resent = 0;          // messages sent again after a broken connection
};
const NetStats& net_stats();

// Short text for a window title: "P2 von 4, Ping 34 ms".
std::string net_status_line();

// While this player waits for the others, or plays the game so far again after coming back: what
// it waits for, in two lines (empty otherwise).
void net_wait_text(std::string& first, std::string& second);

// A guest that came back and is still playing the game so far again: the window shows no pictures
// meanwhile and does not wait for the picture rate.
bool net_catching_up();

// Called about every 100 ms while this player waits for the others (lockstep, or a guest waiting for
// the host's menu), so a window can handle its events and show that it waits. May be null.
using NetWaitCallback = void (*)();
void net_set_wait_callback(NetWaitCallback callback);

// Called when the game ends for this player because of the network (a player left or stayed away,
// the host is gone), with a sentence for the player. Must not return. Without one the process prints
// the text and ends with exit code 4.
using NetEndCallback = void (*)(const std::string& text);
void net_set_end_callback(NetEndCallback callback);

// This player leaves on purpose. The host ends the game for everybody; a guest tells the host,
// which ends it for the others. Returns once the message is out (at most about a second).
void net_leave();

// A touch the program makes for the player this tick (the lobby's character on the character
// screen). The window and the test run apply it instead of their own touch while it returns true.
bool net_auto_touch(u32& x, u32& y);
