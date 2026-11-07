// The player's side of the lobby: connects to MarioPartyDS-server, creates or joins a room and
// waits for the start. Never blocks for long: the window calls poll() every picture.
#pragma once

#include "lobby_protocol.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct LobbySeat {
    bool present = false;
    std::string name;
    int character = -1;
    bool ready = false;
    int ping_ms = 0;
};

struct LobbyStart {
    int slot = 0;     // 0 is the host
    int players = 0;
    std::array<int, lobby::kMaxPlayers> characters{-1, -1, -1, -1};
    std::uintptr_t socket = 0;     // the connection, now carrying the game (blocking mode)
    std::vector<std::uint8_t> inbox;  // bytes that came after the start message
    int input_delay = 1;              // pictures, chosen by the server from the pings
    std::uint64_t token = 0;          // shows the server who comes back after a broken connection
    std::array<std::uint64_t, lobby::kMaxPlayers> tokens{};  // the host: everybody's, for a restarted server
    std::string code;                 // the room
    std::string address;              // the server, for coming back
    std::uint16_t port = 0;
};

class LobbyClient {
public:
    enum class Phase { Offline, Connecting, Menu, InRoom, Started, Failed };

    ~LobbyClient();

    // Starts connecting; poll() finishes it. `address` is a name or IPv4 address.
    void connect(const std::string& address, std::uint16_t port, const std::string& name);
    void create(int character);
    void join(const std::string& code, int character);
    void pick(int character);
    void set_ready(bool ready);
    void start();
    void leave();  // back to the menu, still connected
    void disconnect();

    // Reads what has arrived and sends a ping each second.
    void poll();

    Phase phase() const { return phase_; }
    // The last refusal or failure, as text for the player; cleared by the next request.
    const std::string& message() const { return message_; }
    int last_error() const { return last_error_; }
    const std::string& code() const { return code_; }
    int own_slot() const { return own_slot_; }
    const std::array<LobbySeat, lobby::kMaxPlayers>& seats() const { return seats_; }
    int ping_ms() const { return ping_ms_; }
    std::uint64_t room_updates() const { return room_updates_; }

    // After Phase::Started: hands the connection over to the game. Call once.
    LobbyStart take_start();

private:
    void send(std::uint8_t type, const std::vector<std::uint8_t>& body);
    void fail(const std::string& text);
    void handle(std::uint8_t type, const std::vector<std::uint8_t>& body);

    Phase phase_ = Phase::Offline;
    std::uintptr_t socket_ = ~std::uintptr_t(0);
    std::string name_;
    std::string address_;
    std::uint16_t port_ = 0;
    std::vector<std::uint8_t> inbox_;
    std::vector<std::uint8_t> outbox_;
    std::string message_;
    int last_error_ = 0;
    std::string code_;
    int own_slot_ = -1;
    std::array<LobbySeat, lobby::kMaxPlayers> seats_{};
    int ping_ms_ = 0;
    std::array<int, 5> recent_pings_{};
    std::size_t ping_count_ = 0;
    double connect_started_ = 0;
    double last_ping_ = 0;
    std::uint64_t room_updates_ = 0;
    LobbyStart start_{};
};
