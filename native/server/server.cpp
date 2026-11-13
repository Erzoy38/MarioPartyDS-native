// MarioPartyDS-server: lobby with room codes, and the relay for the game once a room has started.
//
// Usage: MarioPartyDS-server [--port N] [--bind ADDRESS] [--code-lifetime SECONDS] [--seed N]
//                             [--input-delay PICTURES] [--idle-timeout SECONDS] [--key FILE] [--allow-plain]
//
// Every connection is encrypted (runtime/secure.h). The server's key lives in --key (default
// MarioPartyDS-server.key next to the program) and is made on the first start; its fingerprint is
// printed, so the host can tell the others. --allow-plain also takes unencrypted connections, for
// the protocol tests only.
//
// --bind 127.0.0.1 accepts players from this PC only (tests; no firewall question). The default
// 0.0.0.0 accepts players from other PCs, which Windows asks about once.
//
// The server holds no game content and runs no game code. It hands out six-character codes,
// keeps up to four players per room, checks every lobby request, and after the start passes the
// game's messages between the host's game and the guests' games (see runtime/lobby_protocol.h).
// One thread, select() over all connections.
//
// A started room outlives broken connections: a player comes back with kResume and the token from
// kStarted, and the others are told (kRelayGone, kRelayBack). The room ends when the host leaves on
// purpose (kLeave) or nobody has been connected for 10 minutes. The server keeps no game state: after
// a restart the host's kResume builds the room again, and the games resend what was lost.
//
// On Windows it is the program MarioPartyDS-server.exe. On Android and iOS the same code runs inside
// the app as a thread (server.h, MPDS_SERVER_LIBRARY), through the game's socket layer and Mbed TLS.
#include "server.h"

#include "lobby_protocol.h"
#include "platform_socket.h"
#include "secure.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace lobby;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

using plat::kNoSocket;
using plat::Socket;

struct Client {
    Socket socket = kNoSocket;
    std::vector<u8> inbox;
    std::vector<u8> outbox;
    std::string name;
    bool hello = false;
    std::string room;  // code, empty when not in a room
    int slot = -1;
    int character = -1;
    bool ready = false;
    u16 ping = 0;
    bool relay = false;  // the room has started: frames are game data
    bool closing = false;
    double heard = 0;  // last bytes from this client
    bool decided = false;                     // encrypted or not, from the first four bytes
    std::vector<u8> first;                    // those bytes, until then
    std::shared_ptr<secure::Channel> channel;  // null: a plain connection (--allow-plain)
};

struct Room {
    std::string code;
    std::array<int, kMaxPlayers> members{-1, -1, -1, -1};  // client ids by slot; slot 0 is the host
    bool started = false;
    double created = 0;
    int players = 0;                        // after the start
    std::array<u64, kMaxPlayers> tokens{};  // after the start: what kResume must show, by slot
    double empty_since = 0;                 // started and nobody connected since then; 0 while somebody is
};

std::map<int, Client> g_clients;
std::map<std::string, Room> g_rooms;
std::set<std::string> g_spent_codes;  // expired or used codes: reported as expired, never handed out again
int g_next_id = 1;
double g_code_lifetime = 30 * 60;
double g_idle_timeout = 20;
double g_empty_room_lifetime = 10 * 60;
int g_input_delay = -1;  // pictures; -1: from the players' pings at the start
std::mt19937 g_random;
std::mt19937_64 g_token_random{std::random_device{}()};  // never from --seed
secure::Identity g_identity;
bool g_allow_plain = false;

void send_frame(Client& client, u8 type, const std::vector<u8>& body) {
    if (client.closing) return;
    if (!client.channel) {
        frame(client.outbox, type, body);
        return;
    }
    std::vector<u8> plain;
    frame(plain, type, body);
    client.channel->send(plain.data(), plain.size(), client.outbox);
}

// Raw bytes from a client: the first four decide whether the connection is encrypted. False if the
// connection must end.
bool take_bytes(int id, Client& client, const u8* data, size_t size) {
    if (!client.decided) {
        client.first.insert(client.first.end(), data, data + size);
        if (client.first.size() < 4) return true;
        client.decided = true;
        std::vector<u8> first = std::move(client.first);
        if (std::memcmp(first.data(), "MPDS", 4) == 0) {
            client.channel = std::make_shared<secure::Channel>(secure::Channel::Role::Server, &g_identity);
        } else if (!g_allow_plain) {
            std::printf("client %d: not encrypted, refused\n", id);
            return false;
        } else {
            client.inbox.insert(client.inbox.end(), first.begin(), first.end());
            return true;
        }
        data = first.data();
        size = first.size();
        if (!client.channel->receive(data, size, client.inbox, client.outbox)) {
            std::printf("client %d: %s\n", id, client.channel->error().c_str());
            return false;
        }
        return true;
    }
    if (!client.channel) {
        client.inbox.insert(client.inbox.end(), data, data + size);
        return true;
    }
    if (!client.channel->receive(data, size, client.inbox, client.outbox)) {
        std::printf("client %d: %s\n", id, client.channel->error().c_str());
        return false;
    }
    return true;
}

void send_error(Client& client, Error error) {
    Writer w;
    w.u8(error);
    send_frame(client, kError, w.bytes);
    std::printf("  -> %s: error %d (%s)\n", client.name.c_str(), int(error), error_text(error));
}

int count_members(const Room& room) {
    int n = 0;
    for (int id : room.members) n += id >= 0;
    return n;
}

void send_room(Room& room) {
    for (int slot = 0; slot < kMaxPlayers; ++slot) {
        if (room.members[slot] < 0) continue;
        Client& to = g_clients[room.members[slot]];
        Writer w;
        w.raw(room.code.data(), kCodeLength);
        w.u8(u32(slot));
        for (int other = 0; other < kMaxPlayers; ++other) {
            const int id = room.members[other];
            if (id < 0) {
                w.u8(0);
                w.str("");
                w.u8(0xFF);
                w.u8(0);
                w.u16(0);
                continue;
            }
            const Client& member = g_clients[id];
            w.u8(1);
            w.str(member.name);
            w.u8(u32(member.character));
            w.u8(member.ready ? 1 : 0);
            w.u16(member.ping);
        }
        send_frame(to, kRoom, w.bytes);
    }
}

std::string new_code() {
    std::uniform_int_distribution<int> pick(0, int(sizeof kCodeAlphabet) - 2);
    for (;;) {
        std::string code;
        for (int i = 0; i < kCodeLength; ++i) code += kCodeAlphabet[pick(g_random)];
        if (!g_rooms.count(code) && !g_spent_codes.count(code)) return code;
    }
}

bool character_free(const Room& room, int character, int except_id) {
    for (int id : room.members) {
        if (id >= 0 && id != except_id && g_clients[id].character == character) return false;
    }
    return true;
}

void close_room(Room& room, Error reason) {
    std::printf("room %s closed: %s\n", room.code.c_str(), error_text(reason));
    for (int id : room.members) {
        if (id < 0) continue;
        Client& member = g_clients[id];
        member.room.clear();
        member.slot = -1;
        member.ready = false;
        if (member.relay) {
            member.closing = true;  // a running game cannot go back to the lobby
        } else {
            Writer w;
            w.u8(reason);
            send_frame(member, kLeft, w.bytes);
        }
    }
    g_spent_codes.insert(room.code);
    g_rooms.erase(room.code);
}

// Takes a client out of its room, in the lobby or in a running game.
void leave_room(Client& client, int id, Error reason) {
    if (client.room.empty()) return;
    auto it = g_rooms.find(client.room);
    if (it == g_rooms.end()) {
        client.room.clear();
        return;
    }
    Room& room = it->second;
    const int slot = client.slot;
    std::printf("room %s: %s leaves slot %d\n", room.code.c_str(), client.name.c_str(), slot);
    if (room.started) {
        // The game goes on; the slot waits for its player (kResume) and the others are told.
        room.members[slot] = -1;
        client.room.clear();
        client.slot = -1;
        for (int other : room.members) {
            if (other < 0) continue;
            Writer w;
            w.u8(u32(slot));
            send_frame(g_clients[other], kRelayGone, w.bytes);
        }
        if (count_members(room) == 0) room.empty_since = now_seconds();
        return;
    }
    if (slot == 0) {
        close_room(room, kHostLeft);
        return;
    }
    room.members[slot] = -1;
    client.room.clear();
    client.slot = -1;
    client.ready = false;
    if (!client.closing && !client.relay) {
        Writer w;
        w.u8(reason);
        send_frame(client, kLeft, w.bytes);
    }
    send_room(room);
    (void)id;
}

void start_room(Room& room) {
    // Players keep their order, without gaps: the host is 0, the guests 1, 2, 3.
    std::array<int, kMaxPlayers> order{-1, -1, -1, -1};
    int count = 0;
    for (int id : room.members) {
        if (id >= 0) order[count++] = id;
    }
    room.members = order;
    room.started = true;
    room.players = count;
    for (int slot = 0; slot < count; ++slot) room.tokens[size_t(slot)] = g_token_random() | 1;
    // Input delay: the way from a guest through the server to the host and back, in pictures of
    // 1/60 s, plus one for unevenness. Pings are the players' last round trips to the server.
    int delay = g_input_delay;
    if (delay < 0) {
        int slowest = 0;
        for (int slot = 1; slot < count; ++slot) slowest = std::max<int>(slowest, g_clients[order[slot]].ping);
        const int path_ms = g_clients[order[0]].ping + slowest;
        delay = std::clamp(1 + (path_ms * 60 + 999) / 1000, 1, 15);
    }
    std::printf("room %s starts with %d players, input delay %d\n", room.code.c_str(), count, delay);
    for (int slot = 0; slot < count; ++slot) {
        Client& member = g_clients[order[slot]];
        member.slot = slot;
        Writer w;
        w.u8(u32(slot));
        w.u8(u32(count));
        for (int i = 0; i < kMaxPlayers; ++i) w.u8(order[i] >= 0 ? u32(g_clients[order[i]].character) : 0xFF);
        w.u8(u32(delay));
        w.u64(room.tokens[size_t(slot)]);
        if (slot == 0) {
            for (int i = 0; i < count; ++i) w.u64(room.tokens[size_t(i)]);
        }
        send_frame(member, kStarted, w.bytes);
        member.relay = true;
    }
}

void handle_relay(Client& client, u8 type, const std::vector<u8>& body) {
    auto it = g_rooms.find(client.room);
    if (it == g_rooms.end()) {
        client.closing = true;
        return;
    }
    Room& room = it->second;
    if (type == kPing) {  // the connection's own round trip; never passed on
        Reader r(body);
        const u64 time = r.u64();
        client.ping = u16(r.u16());
        Writer w;
        w.u64(time);
        send_frame(client, kPong, w.bytes);
        return;
    }
    if (type == kLeave) {
        if (client.slot == 0) close_room(room, kHostLeft);
        else client.closing = true;
        return;
    }
    if (client.slot == 0) {
        if (type != kRelayTo || body.size() < 2 || body[0] >= kMaxPlayers || room.members[body[0]] < 0) return;
        Client& to = g_clients[room.members[body[0]]];
        send_frame(to, body[1], std::vector<u8>(body.begin() + 2, body.end()));
        return;
    }
    if (room.members[0] < 0) return;
    Writer w;
    w.u8(u32(client.slot));
    w.u8(type);
    w.raw(body.data(), body.size());
    send_frame(g_clients[room.members[0]], kRelayFrom, w.bytes);
}

void handle(int id, u8 type, const std::vector<u8>& body) {
    Client& client = g_clients[id];
    if (client.relay) {
        handle_relay(client, type, body);
        return;
    }
    Reader r(body);
    if (type == kPing) {
        const u64 time = r.u64();
        const u32 rtt = r.u16();
        if (!r.done()) return send_error(client, kBadMessage);
        client.ping = u16(rtt);
        Writer w;
        w.u64(time);
        send_frame(client, kPong, w.bytes);
        return;
    }
    if (type == kHello) {
        const u32 version = r.u16();
        const std::string name = r.str();
        if (!r.done()) return send_error(client, kBadMessage);
        if (version != kVersion) return send_error(client, kBadVersion);
        bool printable = !name.empty() && name.size() <= size_t(kMaxName);
        for (char c : name) printable = printable && c >= 0x20 && c < 0x7F;
        if (!printable) return send_error(client, kBadName);
        client.name = name;
        client.hello = true;
        std::printf("client %d is %s\n", id, name.c_str());
        return;
    }
    if (!client.hello) return send_error(client, kBadMessage);
    switch (type) {
        case kCreate: {
            const int character = int(r.u8());
            if (!r.done() || character >= kCharacters) return send_error(client, kBadMessage);
            if (!client.room.empty()) leave_room(client, id, kLeftRoom);
            Room room;
            room.code = new_code();
            room.created = now_seconds();
            room.members[0] = id;
            client.room = room.code;
            client.slot = 0;
            client.character = character;
            client.ready = true;  // the host starts when he is ready
            std::printf("room %s created by %s (%s)\n", room.code.c_str(), client.name.c_str(), character_name(character));
            send_room(g_rooms[room.code] = room);
            return;
        }
        case kJoin: {
            std::string code = r.fixed(kCodeLength);
            const int character = int(r.u8());
            if (!r.done() || character >= kCharacters) return send_error(client, kBadMessage);
            for (char& c : code) c = char(std::toupper(static_cast<unsigned char>(c)));
            auto it = g_rooms.find(code);
            if (it == g_rooms.end()) return send_error(client, g_spent_codes.count(code) ? kExpired : kUnknownCode);
            Room& room = it->second;
            if (room.started) return send_error(client, kAlreadyStarted);
            if (count_members(room) >= kMaxPlayers) return send_error(client, kRoomFull);
            if (!character_free(room, character, id)) return send_error(client, kCharacterTaken);
            if (!client.room.empty()) leave_room(client, id, kLeftRoom);
            int slot = 1;
            while (room.members[slot] >= 0) ++slot;
            room.members[slot] = id;
            client.room = code;
            client.slot = slot;
            client.character = character;
            client.ready = false;
            std::printf("room %s: %s joins slot %d (%s)\n", code.c_str(), client.name.c_str(), slot, character_name(character));
            send_room(room);
            return;
        }
        case kResume: {
            std::string code = r.fixed(kCodeLength);
            const int slot = int(r.u8());
            const u64 token = r.u64();
            std::array<u64, kMaxPlayers> tokens{};
            int count = 0;
            if (slot == 0 && r.ok && !r.done()) {
                count = int(r.u8());
                for (int i = 0; i < count && i < kMaxPlayers; ++i) tokens[size_t(i)] = r.u64();
            }
            if (!r.done() || slot >= kMaxPlayers || count > kMaxPlayers) return send_error(client, kBadMessage);
            for (char& c : code) c = char(std::toupper(static_cast<unsigned char>(c)));
            auto it = g_rooms.find(code);
            if (it == g_rooms.end()) {
                // Unknown here: after a restart the host builds it again; guests wait for that.
                if (g_spent_codes.count(code)) return send_error(client, kExpired);
                if (slot != 0 || count < 2 || tokens[0] != token) return send_error(client, kTryAgain);
                Room room;
                room.code = code;
                room.created = now_seconds();
                room.started = true;
                room.players = count;
                room.tokens = tokens;
                it = g_rooms.emplace(code, room).first;
                std::printf("room %s built again by its host (%d players)\n", code.c_str(), count);
            }
            Room& room = it->second;
            if (!room.started) return send_error(client, kAlreadyStarted);
            if (slot >= room.players || room.tokens[size_t(slot)] != token) return send_error(client, kBadToken);
            if (!client.room.empty()) leave_room(client, id, kLeftRoom);
            const int old = room.members[size_t(slot)];
            if (old >= 0 && old != id) {  // the broken connection, not noticed yet: replaced
                Client& stale = g_clients[old];
                stale.room.clear();
                stale.slot = -1;
                stale.closing = true;
            }
            room.members[size_t(slot)] = id;
            room.empty_since = 0;
            client.room = code;
            client.slot = slot;
            client.relay = true;
            std::printf("room %s: %s is back in slot %d\n", code.c_str(), client.name.c_str(), slot);
            Writer w;
            w.u8(u32(slot));
            send_frame(client, kResumed, w.bytes);
            for (int other : room.members) {
                if (other < 0 || other == id) continue;
                Writer back;
                back.u8(u32(slot));
                send_frame(g_clients[other], kRelayBack, back.bytes);
            }
            return;
        }
        default: break;
    }
    auto it = g_rooms.find(client.room);
    if (it == g_rooms.end()) return send_error(client, kNotInRoom);
    Room& room = it->second;
    switch (type) {
        case kPick: {
            const int character = int(r.u8());
            if (!r.done() || character >= kCharacters) return send_error(client, kBadMessage);
            if (!character_free(room, character, id)) return send_error(client, kCharacterTaken);
            client.character = character;
            std::printf("room %s: %s picks %s\n", room.code.c_str(), client.name.c_str(), character_name(character));
            send_room(room);
            return;
        }
        case kReady: {
            const bool ready = r.u8() != 0;
            if (!r.done()) return send_error(client, kBadMessage);
            client.ready = ready || client.slot == 0;
            send_room(room);
            return;
        }
        case kStart: {
            if (!r.done()) return send_error(client, kBadMessage);
            if (client.slot != 0) return send_error(client, kNotHost);
            if (count_members(room) < 2) return send_error(client, kNotEnoughPlayers);
            for (int member : room.members) {
                if (member >= 0 && !g_clients[member].ready) return send_error(client, kNotReady);
            }
            start_room(room);
            return;
        }
        case kLeave:
            if (!r.done()) return send_error(client, kBadMessage);
            leave_room(client, id, kLeftRoom);
            return;
        default:
            return send_error(client, kBadMessage);
    }
}

void drop(int id) {
    Client& client = g_clients[id];
    std::printf("client %d (%s) gone\n", id, client.name.c_str());
    client.closing = true;
    leave_room(client, id, kLeftRoom);
    plat::close_socket(client.socket);
    g_clients.erase(id);
}

void expire_rooms() {
    const double now = now_seconds();
    std::vector<std::string> old;
    for (auto& [code, room] : g_rooms) {
        if (!room.started && now - room.created > g_code_lifetime) old.push_back(code);
        if (room.started && room.empty_since > 0 && now - room.empty_since > g_empty_room_lifetime) old.push_back(code);
    }
    for (const std::string& code : old) close_room(g_rooms[code], kExpired);
    // A connection that has said nothing for a while is dead (players ping every second).
    for (auto& [id, client] : g_clients) {
        if (!client.closing && client.heard > 0 && now - client.heard > g_idle_timeout) {
            std::printf("client %d (%s) silent for %.0f s\n", id, client.name.c_str(), now - client.heard);
            client.closing = true;
            client.outbox.clear();
        }
    }
}

// Everything closed and forgotten, so that server_main can run again in the same process.
void close_all(Socket listener) {
    for (auto& [id, client] : g_clients) plat::close_socket(client.socket);
    if (listener != kNoSocket) plat::close_socket(listener);
    g_clients.clear();
    g_rooms.clear();
    g_spent_codes.clear();
    g_next_id = 1;
    g_identity.reset();
}

}  // namespace

#ifndef MPDS_SERVER_LIBRARY
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return server_main(argc, argv, nullptr);
}
#endif

int server_main(int argc, char** argv, const std::atomic<bool>* stop) {
    u16 port = kDefaultPort;
    u32 seed = std::random_device{}();
    const char* bind_address = "0.0.0.0";
    std::string key_file;
    for (int i = 1; i < argc; i += 2) {
        if (!std::strcmp(argv[i], "--allow-plain")) {  // the only option without a value
            g_allow_plain = true;
            --i;
            continue;
        }
        if (i + 1 >= argc) break;
        if (!std::strcmp(argv[i], "--port")) port = u16(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--bind")) bind_address = argv[i + 1];
        else if (!std::strcmp(argv[i], "--code-lifetime")) g_code_lifetime = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--seed")) seed = u32(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--input-delay")) g_input_delay = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--idle-timeout")) g_idle_timeout = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--key")) key_file = argv[i + 1];
    }
    g_random.seed(seed);
    if (key_file.empty()) {
#ifdef _WIN32
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        key_file = std::string(path);
        key_file = key_file.substr(0, key_file.find_last_of("\\/") + 1) + "MarioPartyDS-server.key";
#else
        key_file = "MarioPartyDS-server.key";  // the app always passes --key
#endif
    }
    std::string key_error;
    if (!g_identity.load_or_create(key_file, key_error)) {
        std::printf("server key: %s\n", key_error.c_str());
        return 1;
    }
    std::printf("server key %s, fingerprint %s%s\n", key_file.c_str(), secure::fingerprint(g_identity.public_key()).c_str(),
                g_allow_plain ? ", also unencrypted connections (tests)" : "");
    plat::net_ready();
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifndef _WIN32
    // The app may start the server again soon after it stopped: the port is free at once.
    plat::set_option(listener, SOL_SOCKET, SO_REUSEADDR, true);
#endif
    sockaddr_in where{};
    where.sin_family = AF_INET;
    where.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address, &where.sin_addr) != 1) {
        std::printf("bad --bind address %s\n", bind_address);
        close_all(listener);
        return 1;
    }
    if (bind(listener, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0 || listen(listener, 16) != 0) {
        std::printf("cannot listen on %s port %u\n", bind_address, port);
        close_all(listener);
        return 1;
    }
    std::printf("server on %s port %u, codes last %.0f s\n", bind_address, port, g_code_lifetime);

    while (stop == nullptr || !stop->load()) {
        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(listener, &readable);
        Socket highest = listener;
        for (auto& [id, client] : g_clients) {
            FD_SET(client.socket, &readable);
            if (!client.outbox.empty()) FD_SET(client.socket, &writable);
            highest = std::max(highest, client.socket);
        }
        timeval wait{0, 200000};
        select(plat::select_width(highest), &readable, &writable, nullptr, &wait);

        if (FD_ISSET(listener, &readable)) {
            Socket s = accept(listener, nullptr, nullptr);
            if (s != kNoSocket && g_clients.size() < 60) {
                plat::set_blocking(s, false);
                plat::set_option(s, IPPROTO_TCP, TCP_NODELAY, true);
                plat::no_sigpipe(s);
                const int id = g_next_id++;
                g_clients[id].socket = s;
                g_clients[id].heard = now_seconds();
                std::printf("client %d connected\n", id);
            } else if (s != kNoSocket) {
                plat::close_socket(s);
            }
        }

        std::vector<int> gone;
        for (auto& [id, client] : g_clients) {
            if (FD_ISSET(client.socket, &readable)) {
                char buffer[16384];
                const int n = plat::receive_some(client.socket, buffer, int(sizeof buffer));
                if (n <= 0) {
                    gone.push_back(id);
                    continue;
                }
                client.heard = now_seconds();
                if (!take_bytes(id, client, reinterpret_cast<const u8*>(buffer), size_t(n))) gone.push_back(id);
            }
        }
        for (int id : gone) drop(id);
        gone.clear();

        // Messages, in arrival order per client.
        std::vector<int> ids;
        for (auto& [id, client] : g_clients) ids.push_back(id);
        for (int id : ids) {
            for (;;) {
                auto it = g_clients.find(id);
                if (it == g_clients.end()) break;
                u8 type;
                std::vector<u8> body;
                bool bad = false;
                if (!unframe(it->second.inbox, type, body, bad)) {
                    if (bad) gone.push_back(id);
                    break;
                }
                handle(id, type, body);
            }
        }
        for (int id : gone) {
            if (g_clients.count(id)) drop(id);
        }
        gone.clear();

        for (auto& [id, client] : g_clients) {
            while (!client.outbox.empty()) {
                const int n = plat::send_some(client.socket, client.outbox.data(),
                                              int(std::min<size_t>(client.outbox.size(), 1 << 16)));
                if (n <= 0) {
                    if (!plat::would_block()) gone.push_back(id);
                    break;
                }
                client.outbox.erase(client.outbox.begin(), client.outbox.begin() + n);
            }
            if (client.closing && client.outbox.empty()) gone.push_back(id);
        }
        for (int id : gone) {
            if (g_clients.count(id)) drop(id);
        }
        expire_rooms();
    }
    std::printf("server stopped\n");
    close_all(listener);
    return 0;
}
