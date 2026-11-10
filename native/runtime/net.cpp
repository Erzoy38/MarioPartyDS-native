#include "net.h"

#include "lobby_protocol.h"

#include "machine.h"
#include "memory.h"

#include "platform_c.h"
#include "platform_socket.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <vector>

namespace {

// --- Addresses in the game, all read from its code (see MULTIPLAYER_ROADMAP.md, phase 8) ---
constexpr u32 kWhState = 0x020D1AE0;      // +0 own aid, +0xE connected bitmap, +0x1C system state
constexpr u32 kNetContext = 0x020D45A0;   // +0 wireless powered (s8)
constexpr u32 kMbState = 0x020C9234;      // +8 state, +0xA.. +0x14 bitmaps per download stage
constexpr u32 kMbCallback = 0x02017920;   // the game's download event handler (aid, event, info)
constexpr u32 kRecordSize = 0x26;
constexpr u32 kBootType = 0x027FFC40;     // 1 card, 2 download play
constexpr u32 kCardEnabled = 0x0215B540;  // card access allowed (CARD_Enable)
constexpr u32 kParentBssDesc = 0x027FFC42;
constexpr u32 kDownloadParameter = 0x027FFBE0;
constexpr u32 kGgid = 0x00000390;  // the multiplayer host's group id (0x02002030); 0x3FFF15 leads to the debug menu
constexpr u32 kLanguage = 0x020C9210;     // game language, set in game_init
// Free space in main RAM that the game does not use: the ARM7's area, which nothing runs in here.
constexpr u32 kScratch = 0x023F8000;
constexpr u32 kSharedRecords = kScratch + 0x100;  // 4 records, handed out by the shared-data getter
constexpr u32 kUserInfo = kScratch + 0x200;       // a guest's name, as MB hands it to the game
constexpr u32 kBlockBuffer = kScratch + 0x400;    // data sent through a port, as the receiver sees it
constexpr u32 kBlockLimit = 0x1000;

enum WhState : u32 { kStop = 0, kIdle = 1, kBusy = 3, kDataSharing = 5, kMeasure = 7 };
enum MbEvent : u32 {
    kConnected = 2, kRequestAccepted = 5, kSendComplete = 7, kBootStartable = 9, kRequested = 10,
    kEnd = 12, kWaitToSend = 14,
};

NetSettings g_settings;
NetWaitCallback g_wait_callback = nullptr;
NetEndCallback g_end_callback = nullptr;
u32 g_own_aid = 0;  // guest: the player number the host gave
NetStats g_stats;
bool g_started = false;
bool g_relay_mode = false;  // through MarioPartyDS-server

// --- Messages between the games ---
enum Message : u8 {
    // Numbered, inside kData; sent again after a broken connection.
    kHello = 1,     // guest -> host: name
    kWelcome = 2,   // host -> guest: u8 aid, u8 language, u64 token (for coming back on a direct connection)
    kJoined = 3,    // host -> guest: u16 connected bitmap
    kRecord = 4,    // guest -> host: the guest's record of one picture
    kStep = 5,      // host -> guest: u16 bitmap, u64 step, 4 records
    kBlock = 6,     // port data: u64 host step at sending (0 from a guest), the bytes
    kBye = 7,       // guest -> host: leaves the game on purpose
    kGameOver = 8,  // host -> guest: u8 reason, u8 player, str name
    // Not numbered.
    kData = 40,  // u32 number, u32 last number received, u8 type, body
    kSync = 41,  // u32 last number received, u8 reply; on a new direct connection also u8 aid, u64 token
    kPing = 42,  // u64 time in ms, u32 last number received
    kPong = 43,  // u64 time as sent
};
enum EndReason : u8 { kHostLeft = 1, kPlayerLeft = 2, kPlayerLost = 3, kHostLost = 4, kRoomGone = 5 };

constexpr u64 kPingEvery = 1000;
constexpr u64 kSilence = 6000;     // nothing heard for this long: the connection is dead
constexpr u64 kRetryEvery = 1000;  // between attempts to connect again
constexpr u64 kConnectTimeout = 3000;
constexpr size_t kOutLimit = size_t(8) << 20;

u64 now_ms() { return plat::steady_ms(); }

using plat::kNoSocket;
using plat::Socket;

// --- Connections: non-blocking sockets with their own buffers ---
struct Conn {
    Socket s = kNoSocket;
    std::vector<u8> in, out;  // `in` decrypted, `out` as it goes on the wire
    u64 heard = 0;
    std::shared_ptr<secure::Channel> channel;  // to the server: encrypted (secure.h)
    bool key_checked = false;
    bool open() const { return s != kNoSocket; }
};

void make_nonblocking(Socket s) {
    plat::set_blocking(s, false);
    plat::set_option(s, IPPROTO_TCP, TCP_NODELAY, true);
    plat::no_sigpipe(s);
}

void conn_close(Conn& c) {
    if (c.open()) plat::close_socket(c.s);
    c.s = kNoSocket;
    c.in.clear();
    c.out.clear();
    c.channel.reset();
    c.key_checked = false;
}

bool conn_flush(Conn& c) {
    while (c.open() && !c.out.empty()) {
        const int n = plat::send_some(c.s, c.out.data(), int(std::min<size_t>(c.out.size(), 1 << 16)));
        if (n <= 0) {
            if (plat::would_block()) break;
            return false;
        }
        c.out.erase(c.out.begin(), c.out.begin() + n);
    }
    return c.out.size() < kOutLimit;
}

bool conn_send(Conn& c, u8 type, const std::vector<u8>& body) {
    if (!c.open()) return false;
    if (c.channel) {
        std::vector<u8> plain;
        lobby::frame(plain, type, body);
        c.channel->send(plain.data(), plain.size(), c.out);
    } else {
        lobby::frame(c.out, type, body);
    }
    return conn_flush(c);
}

// Takes in what has arrived. False if the connection has ended.
bool conn_read(Conn& c) {
    for (;;) {
        char buffer[16384];
        const int n = plat::receive_some(c.s, buffer, int(sizeof buffer));
        if (n > 0) {
            c.heard = now_ms();
            if (!c.channel) {
                c.in.insert(c.in.end(), buffer, buffer + n);
            } else if (!c.channel->receive(reinterpret_cast<const u8*>(buffer), size_t(n), c.in, c.out)) {
                std::printf("net: encryption: %s\n", c.channel->error().c_str());
                return false;
            }
            continue;
        }
        if (n == 0) return false;
        return plat::would_block();
    }
}

// The next whole frame, if there is one. A broken frame closes the connection.
bool conn_next(Conn& c, u8& type, std::vector<u8>& body) {
    bool bad = false;
    if (lobby::unframe(c.in, type, body, bad)) return true;
    if (bad) {
        std::printf("net: broken data, connection closed\n");
        conn_close(c);
    }
    return false;
}

// --- Links: numbered messages between the host and one guest ---
struct Link {
    u32 aid = 0;
    u32 next = 1;      // number of the next message sent
    u32 received = 0;  // last number taken in, in order
    std::deque<std::pair<u32, std::vector<u8>>> unconfirmed;  // guest: number, kData body
    std::vector<std::vector<u8>> history;                     // host: every kData body sent, by number - 1
    u32 resend_next = 0;                                      // host: sending again from here; 0 none
    std::deque<std::pair<u8, std::vector<u8>>> messages;      // taken in, not handled yet
    u64 heard = 0;       // anything from the other game
    u64 pinged = 0;
    u64 down_since = 0;  // 0 while the other game is there
};

struct Guest {
    Conn conn;  // direct connection; through the server all guests share g_main
    Link link;
    std::string name;
    u32 aid = 0;
    u64 token = 0;           // direct connection: shows who comes back
    bool welcomed = false;
    bool gone = false;       // through the server: it says this guest's connection ended
    int mb_stage = 0;        // how far the download-play events have come
    bool connected = false;  // takes part in the record exchange
    bool rejoining = false;  // came back with a new program and plays the game so far again
    std::deque<std::vector<u8>> records;
};
std::map<u32, Guest> g_guests;  // host, by aid

// The guest's connection to the host or the server, and the host's to the server.
Conn g_main;
bool g_main_attached = false;  // carries the game: through the server only after kResumed
bool g_ever_connected = false;
Socket g_connecting = kNoSocket;
u64 g_connect_began = 0, g_last_attempt = 0, g_server_pinged = 0;
u64 g_main_down_since = 0;
bool g_left_on_purpose = false;

// Guest side.
Link g_link;               // to the host
u64 g_token = 0;           // for coming back
bool g_host_gone = false;  // through the server: it says the host's connection ended
bool g_welcomed = false;
u8 g_language = 0;
int g_joined = -1;         // bitmap from kJoined, not yet handed to the game
std::deque<std::vector<u8>> g_steps;
std::deque<std::pair<u64, std::vector<u8>>> g_tagged_blocks;  // host step, bytes
u64 g_records_sent = 0, g_steps_taken = 0;
bool g_rejoin_waiting = false;  // came back with a new program: sends nothing until the host says what it has
u32 g_skip_upto = 0;            // messages up to this number the host already has from the old program

// Host side.
Socket g_listen = kNoSocket;
std::vector<Conn> g_pending;  // new direct connections, not yet known
std::deque<std::vector<u8>> g_own_records;
std::mt19937_64 g_token_random{std::random_device{}()};

// Port data (WH send) that has arrived and waits for the game's receive callback: sender aid, bytes.
struct Block {
    u32 aid;
    std::vector<u8> data;
};
std::deque<Block> g_blocks;
std::vector<u32> g_send_done;  // callbacks of sends, called on the next tick

std::string g_wait_first, g_wait_second;

void main_lost(const char* why);
void guest_conn_lost(struct Guest& guest, const char* why);

// What a new program needs to come back into this game (NetSettings::rejoin_file).
void write_rejoin_file() {
    if (g_settings.rejoin_file.empty()) return;
    const std::string temporary = g_settings.rejoin_file + ".new";
    FILE* f = plat::open_file(temporary.c_str(), "w");
    if (f == nullptr) return;
    std::string key;
    for (u8 b : g_settings.server_key) {
        char two[3];
        std::snprintf(two, sizeof two, "%02x", b);
        key += two;
    }
    std::fprintf(f, "via_server=%d\naddress=%s\nport=%u\ncode=%s\nslot=%u\ntoken=%llu\nplayers=%d\n"
                    "input_delay=%d\ngive_up=%d\ncharacter=%d\nname=%s\nserver_key=%s\n",
                 g_relay_mode ? 1 : 0, g_settings.address.c_str(), g_settings.port, g_settings.code.c_str(),
                 g_own_aid, static_cast<unsigned long long>(g_token), g_settings.players, g_settings.input_delay,
                 g_settings.give_up_seconds, g_settings.character, g_settings.name.c_str(), key.c_str());
    std::fclose(f);
    std::remove(g_settings.rejoin_file.c_str());
    std::rename(temporary.c_str(), g_settings.rejoin_file.c_str());
}

void remove_rejoin_file() {
    if (!g_settings.rejoin_file.empty()) std::remove(g_settings.rejoin_file.c_str());
}

bool is_host() { return g_settings.role == NetRole::Host; }

void transmit(Link& link, u8 type, const std::vector<u8>& body) {
    if (!is_host()) {
        if (g_main_attached && !conn_send(g_main, type, body)) main_lost("cannot send");
        return;
    }
    if (g_relay_mode) {
        if (!g_main_attached) return;
        std::vector<u8> wrapped{u8(link.aid), type};
        wrapped.insert(wrapped.end(), body.begin(), body.end());
        if (!conn_send(g_main, lobby::kRelayTo, wrapped)) main_lost("cannot send");
        return;
    }
    auto it = g_guests.find(link.aid);
    if (it != g_guests.end() && it->second.conn.open() && !conn_send(it->second.conn, type, body)) {
        guest_conn_lost(it->second, "cannot send");
    }
}

void link_send(Link& link, u8 type, const std::vector<u8>& body) {
    const u32 number = link.next++;
    // A guest that came back makes its old messages again while it plays the game so far again, in
    // the same order; the host has them already.
    if (!is_host() && number <= g_skip_upto) return;
    lobby::Writer w;
    w.u32(number);
    w.u32(link.received);
    w.u8(type);
    w.raw(body.data(), body.size());
    if (is_host()) {
        link.history.push_back(w.bytes);
        if (link.resend_next != 0) return;  // goes out in order behind the messages sent again
    } else {
        link.unconfirmed.push_back({number, w.bytes});
        if (g_rejoin_waiting) return;
    }
    transmit(link, kData, w.bytes);
}

void confirmed(Link& link, u32 number) {
    while (!link.unconfirmed.empty() && link.unconfirmed.front().first <= number) link.unconfirmed.pop_front();
}

// kSync: 0 asks the other side for what is missing, 1 answers that, 2 asks for everything (a new
// program coming back).
enum SyncKind : u8 { kSyncAsk = 0, kSyncAnswer = 1, kSyncFresh = 2 };

void send_sync(Link& link, u8 kind) {
    lobby::Writer w;
    w.u32(link.received);
    w.u8(kind);
    transmit(link, kSync, w.bytes);
}

u32& ping_slot(const Link& link) { return g_stats.ping_ms[is_host() ? link.aid & 3 : 0]; }

void link_receive(Link& link, u8 type, const std::vector<u8>& body) {
    lobby::Reader r(body);
    switch (type) {
        case kData: {
            const u32 number = r.u32();
            const u32 last = r.u32();
            const u8 inner = u8(r.u8());
            if (!r.ok) return;
            link.heard = now_ms();
            confirmed(link, last);
            if (number == link.received + 1) {
                link.received = number;
                link.messages.push_back({inner, std::vector<u8>(body.begin() + 9, body.end())});
            }
            return;
        }
        case kSync: {
            const u32 last = r.u32();
            const u8 kind = u8(r.u8());
            if (!r.ok) return;
            link.heard = now_ms();
            if (is_host()) {
                // Sent again from the history, a piece per pump (continue_resend), so a long one
                // does not fill the connection at once.
                const u32 from = kind == kSyncFresh ? 1 : last + 1;
                link.resend_next = from < link.next ? from : 0;
                if (kind == kSyncFresh) {
                    auto it = g_guests.find(link.aid);
                    if (it != g_guests.end()) it->second.rejoining = true;
                }
                std::printf("net: player %u %s: sending %u messages again\n", link.aid + 1,
                            kind == kSyncFresh ? "comes back with a new program" : "is in step again",
                            link.resend_next == 0 ? 0 : link.next - link.resend_next);
                if (kind != kSyncAnswer) send_sync(link, kSyncAnswer);
                return;
            }
            if (g_rejoin_waiting && kind == kSyncAnswer) {
                g_skip_upto = last;
                g_rejoin_waiting = false;
                while (!link.unconfirmed.empty() && link.unconfirmed.front().first <= last) link.unconfirmed.pop_front();
                std::printf("net: back with a new program; the host has %u messages from before\n", last);
            }
            confirmed(link, last);
            for (const auto& [number, payload] : link.unconfirmed) {
                transmit(link, kData, payload);
                ++g_stats.resent;
            }
            if (kind == kSyncAsk) send_sync(link, kSyncAnswer);
            return;
        }
        case kPing: {
            const u64 time = r.u64();
            const u32 last = r.u32();
            if (!r.ok) return;
            link.heard = now_ms();
            confirmed(link, last);
            lobby::Writer w;
            w.u64(time);
            transmit(link, kPong, w.bytes);
            return;
        }
        case kPong: {
            const u64 time = r.u64();
            if (!r.ok) return;
            link.heard = now_ms();
            ping_slot(link) = u32(std::max<u64>(1, now_ms() - time));
            return;
        }
        default: return;
    }
}

void link_ping(Link& link) {
    const u64 now = now_ms();
    if (now - link.pinged < kPingEvery) return;
    link.pinged = now;
    lobby::Writer w;
    w.u64(now);
    w.u32(link.received);
    transmit(link, kPing, w.bytes);
}

// --- The end of the game for this player ---
std::string player_text(u32 aid, const std::string& name) {
    std::string text = "P" + std::to_string(aid + 1);
    if (!name.empty()) text += " (" + name + ")";
    return text;
}

std::string end_text(int reason, u32 aid, const std::string& name) {
    switch (reason) {
        case kHostLeft: return "Der Host hat das Spiel beendet.";
        case kPlayerLeft: return player_text(aid, name) + " hat das Spiel verlassen. Die Partie ist beendet.";
        case kPlayerLost: return player_text(aid, name) + " ist nicht zurückgekommen. Die Partie ist beendet.";
        case kHostLost: return "Die Verbindung zum Host ist abgebrochen. Die Partie ist beendet.";
        case kRoomGone: return "Den Raum gibt es auf dem Server nicht mehr. Die Partie ist beendet.";
        default: return "Die Partie ist beendet.";
    }
}

void flush_everything(u64 limit_ms) {
    const u64 begin = now_ms();
    for (;;) {
        bool waiting = false;
        if (g_main.open() && !conn_flush(g_main)) conn_close(g_main);
        waiting = waiting || (g_main.open() && !g_main.out.empty());
        for (auto& [aid, guest] : g_guests) {
            if (guest.conn.open() && !conn_flush(guest.conn)) conn_close(guest.conn);
            waiting = waiting || (guest.conn.open() && !guest.conn.out.empty());
        }
        if (!waiting || now_ms() - begin > limit_ms) return;
        plat::sleep_ms(5);
    }
}

[[noreturn]] void finish_game(const std::string& text) {
    static bool ending = false;
    if (!ending) {
        ending = true;
        std::printf("net: game over: %s (step %llu)\n", text.c_str(), static_cast<unsigned long long>(g_stats.steps));
        std::fflush(stdout);
        remove_rejoin_file();
        if (g_end_callback != nullptr) g_end_callback(text);
    }
    std::exit(4);
}

// The host tells everybody that the game is over.
void announce_end(int reason, u32 aid, const std::string& name) {
    for (auto& [other, guest] : g_guests) {
        if (!guest.welcomed) continue;
        lobby::Writer w;
        w.u8(u32(reason));
        w.u8(aid);
        w.str(name);
        link_send(guest.link, kGameOver, w.bytes);
    }
    if (g_relay_mode && g_main.open()) conn_send(g_main, lobby::kLeave, {});
    flush_everything(1000);
}

[[noreturn]] void end_game(int reason, u32 aid, const std::string& name) {
    if (is_host()) announce_end(reason, aid, name);
    finish_game(end_text(reason, aid, name));
}

// --- Connecting again ---
void main_lost(const char* why) {
    if (!g_main.open() && !g_main_attached) return;
    std::printf("net: connection to the %s lost (%s) at step %llu\n", g_relay_mode ? "server" : "host", why,
                static_cast<unsigned long long>(g_stats.steps));
    conn_close(g_main);
    g_main_attached = false;
}

void guest_conn_lost(Guest& guest, const char* why) {
    std::printf("net: connection to player %u lost (%s) at step %llu\n", guest.aid + 1, why,
                static_cast<unsigned long long>(g_stats.steps));
    conn_close(guest.conn);
}

void start_connect() {
    const u64 now = now_ms();
    if (g_connecting != kNoSocket || now - g_last_attempt < kRetryEvery) return;
    g_last_attempt = now;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(g_settings.address.c_str(), std::to_string(g_settings.port).c_str(), &hints, &found) != 0
        || found == nullptr) {
        return;
    }
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    make_nonblocking(s);
    const int result = connect(s, found->ai_addr, plat::SockLen(found->ai_addrlen));
    freeaddrinfo(found);
    if (result != 0 && !plat::would_block()) {
        plat::close_socket(s);
        return;
    }
    g_connecting = s;
    g_connect_began = now;
}

// A connection opened again: through the server, back into the room; directly, back to the host.
void on_connected() {
    ++g_stats.reconnects;
    std::printf("net: connected again at step %llu\n", static_cast<unsigned long long>(g_stats.steps));
    if (g_relay_mode) {
        lobby::Writer hello;
        hello.u16(lobby::kVersion);
        hello.str(g_settings.name.substr(0, size_t(lobby::kMaxName)));
        conn_send(g_main, lobby::kHello, hello.bytes);
        lobby::Writer w;
        std::string code = g_settings.code;
        code.resize(size_t(lobby::kCodeLength), ' ');
        w.raw(code.data(), code.size());
        w.u8(u32(g_settings.slot));
        w.u64(g_settings.token);
        if (is_host()) {
            w.u8(u32(g_settings.players));
            for (int i = 0; i < g_settings.players && i < 4; ++i) w.u64(g_settings.tokens[size_t(i)]);
        }
        conn_send(g_main, lobby::kResume, w.bytes);
        return;  // attached when kResumed comes
    }
    g_main_attached = true;
    lobby::Writer w;
    w.u32(g_link.received);
    w.u8(g_rejoin_waiting ? kSyncFresh : kSyncAsk);
    w.u8(g_own_aid);
    w.u64(g_token);
    conn_send(g_main, kSync, w.bytes);
}

void check_connect() {
    if (g_connecting == kNoSocket) return;
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(g_connecting, &writable);
    FD_SET(g_connecting, &failed);
    timeval zero{0, 0};
    select(plat::select_width(g_connecting), nullptr, &writable, &failed, &zero);
    const int state = plat::connect_state(g_connecting, writable, failed);
    if (state < 0 || (state == 0 && now_ms() - g_connect_began > kConnectTimeout)) {
        plat::close_socket(g_connecting);
        g_connecting = kNoSocket;
        return;
    }
    if (state == 0) return;
    conn_close(g_main);
    g_main.s = g_connecting;
    g_main.heard = now_ms();
    g_connecting = kNoSocket;
    if (g_relay_mode) {  // the server: encrypted, the hello and kResume wait for the handshake
        g_main.channel = std::make_shared<secure::Channel>(secure::Channel::Role::Client);
        g_main.out = g_main.channel->start();
    }
    if (!g_ever_connected) {  // the first connection of a direct guest
        g_ever_connected = true;
        g_main_attached = true;
        return;
    }
    on_connected();
}

Guest& guest_for(u32 aid) {
    Guest& guest = g_guests[aid];
    if (guest.link.heard == 0) {
        guest.aid = aid;
        guest.link.aid = aid;
        guest.link.heard = now_ms();
    }
    return guest;
}

// What the server says on the connection that carries the game through it.
void handle_server_frame(u8 type, const std::vector<u8>& body) {
    lobby::Reader r(body);
    switch (type) {
        case lobby::kResumed:
            g_main_attached = true;
            std::printf("net: back in room %s\n", g_settings.code.c_str());
            if (is_host()) {
                for (auto& [aid, guest] : g_guests) send_sync(guest.link, kSyncAsk);
            } else {
                send_sync(g_link, g_rejoin_waiting ? kSyncFresh : kSyncAsk);
            }
            return;
        case lobby::kError: {
            const int error = int(r.u8());
            std::printf("net: server says: %s\n", lobby::error_text(error));
            conn_close(g_main);
            g_main_attached = false;
            if (error == lobby::kExpired || error == lobby::kBadToken) {
                if (is_host()) end_game(kRoomGone, 0, "");
                finish_game(end_text(kRoomGone, 0, ""));
            }
            return;  // kTryAgain: the room waits for its host; the next attempt comes in a second
        }
        case lobby::kPong: {
            const u64 time = r.u64();
            if (r.ok) g_stats.server_ping_ms = u32(std::max<u64>(1, now_ms() - time));
            return;
        }
        case lobby::kRelayGone:
        case lobby::kRelayBack: {
            const u32 slot = r.u8();
            if (!r.ok || slot > 3) return;
            const bool gone = type == lobby::kRelayGone;
            std::printf("net: server says player %u %s\n", slot + 1, gone ? "is gone" : "is back");
            if (is_host()) {
                auto it = g_guests.find(slot);
                if (it != g_guests.end()) it->second.gone = gone;
            } else if (slot == 0) {
                g_host_gone = gone;
            }
            return;
        }
        case lobby::kRelayFrom: {
            if (!is_host() || body.size() < 2 || body[0] < 1 || body[0] > 3) return;
            Guest& guest = guest_for(body[0]);
            guest.gone = false;
            link_receive(guest.link, body[1], std::vector<u8>(body.begin() + 2, body.end()));
            return;
        }
        default:
            if (!is_host()) link_receive(g_link, type, body);  // the host's frames, passed on
            return;
    }
}

void drain();

void service_main() {
    check_connect();
    if (!g_main.open()) {
        start_connect();
        return;
    }
    // What came before a close is still read: often the last words, such as the end of the game.
    const bool alive = conn_read(g_main) && conn_flush(g_main);
    if (g_main.channel && g_main.channel->ready() && !g_main.key_checked) {
        g_main.key_checked = true;
        if (!g_settings.server_key.empty() && g_main.channel->server_key() != g_settings.server_key) {
            conn_close(g_main);
            finish_game("Der Server hat jetzt einen anderen Schlüssel als beim Start der Partie. "
                        "Jemand könnte sich dazwischen schalten; die Partie ist beendet.");
        }
    }
    u8 type;
    std::vector<u8> body;
    while (g_main.open() && conn_next(g_main, type, body)) {
        if (g_relay_mode) handle_server_frame(type, body);
        else link_receive(g_link, type, body);
    }
    if (!alive) {
        drain();
        main_lost("closed");
    }
}

// The host on direct connections: new players, players coming back, and their messages.
void service_direct_host() {
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(g_listen, &set);
        timeval zero{0, 0};
        if (select(plat::select_width(g_listen), &set, nullptr, nullptr, &zero) <= 0) break;
        Socket s = accept(g_listen, nullptr, nullptr);
        if (s == kNoSocket) break;
        make_nonblocking(s);
        Conn conn;
        conn.s = s;
        conn.heard = now_ms();
        g_pending.push_back(std::move(conn));
    }
    for (size_t i = 0; i < g_pending.size();) {
        Conn& conn = g_pending[i];
        u8 type = 0;
        std::vector<u8> body;
        const bool alive = conn_read(conn);
        const bool whole = alive && conn_next(conn, type, body);
        if (!whole) {
            if (!alive || !conn.open() || now_ms() - conn.heard > kSilence) {
                conn_close(conn);
                g_pending.erase(g_pending.begin() + std::ptrdiff_t(i));
            } else {
                ++i;
            }
            continue;
        }
        Conn taken = std::move(conn);
        g_pending.erase(g_pending.begin() + std::ptrdiff_t(i));
        lobby::Reader r(body);
        if (type == kData) {  // a new player: its first message is the hello
            u32 aid = 1;
            while (g_guests.count(aid)) ++aid;
            if (aid > 3) {
                conn_close(taken);
                continue;
            }
            Guest& guest = guest_for(aid);
            guest.conn = std::move(taken);
            link_receive(guest.link, type, body);
        } else if (type == kSync) {  // a player coming back
            r.u32();
            r.u8();
            const u32 aid = r.u8();
            const u64 token = r.u64();
            auto it = g_guests.find(aid);
            if (!r.ok || it == g_guests.end() || it->second.token != token) {
                std::printf("net: an unknown player tried to come back\n");
                conn_close(taken);
                continue;
            }
            conn_close(it->second.conn);
            it->second.conn = std::move(taken);
            ++g_stats.reconnects;
            link_receive(it->second.link, type, std::vector<u8>(body.begin(), body.begin() + 5));
        } else {
            conn_close(taken);
        }
    }
    for (auto& [aid, guest] : g_guests) {
        if (!guest.conn.open()) continue;
        const bool alive = conn_read(guest.conn) && conn_flush(guest.conn);
        u8 type;
        std::vector<u8> body;
        while (guest.conn.open() && conn_next(guest.conn, type, body)) link_receive(guest.link, type, body);
        if (!alive) guest_conn_lost(guest, "closed");
    }
}

void wait_sockets(int wait_ms) {
    fd_set readable, writable, failed;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    int count = 0;
    Socket highest = 0;
    auto add = [&](const Conn& conn) {
        FD_SET(conn.s, &readable);
        if (!conn.out.empty()) FD_SET(conn.s, &writable);
        highest = std::max(highest, conn.s);
        ++count;
    };
    if (g_main.open()) add(g_main);
    if (g_connecting != kNoSocket) {
        FD_SET(g_connecting, &writable);
        FD_SET(g_connecting, &failed);
        highest = std::max(highest, g_connecting);
        ++count;
    }
    if (g_listen != kNoSocket) {
        FD_SET(g_listen, &readable);
        highest = std::max(highest, g_listen);
        ++count;
    }
    for (const Conn& conn : g_pending) add(conn);
    for (const auto& [aid, guest] : g_guests) {
        if (guest.conn.open()) add(guest.conn);
    }
    if (count == 0) {
        if (wait_ms > 0) plat::sleep_ms(unsigned(wait_ms));
        return;
    }
    timeval timeout{0, long(wait_ms) * 1000};
    select(plat::select_width(highest), &readable, &writable, &failed, &timeout);
}

void take_in_guest(Guest& guest, const std::vector<u8>& hello);

// Messages that have come in, handed to the game's side of this code.
void drain() {
    if (is_host()) {
        int end_reason = 0;
        u32 end_aid = 0;
        for (auto& [aid, guest] : g_guests) {
            if (!guest.link.messages.empty() && guest.rejoining) {
                guest.rejoining = false;
                std::printf("net: player %u has caught up\n", aid + 1);
            }
            while (!guest.link.messages.empty()) {
                auto [type, body] = std::move(guest.link.messages.front());
                guest.link.messages.pop_front();
                if (type == kHello) {
                    if (!guest.welcomed) take_in_guest(guest, body);
                } else if (type == kRecord) {
                    guest.records.push_back(std::move(body));
                } else if (type == kBlock && body.size() >= 8) {
                    g_blocks.push_back({aid, std::vector<u8>(body.begin() + 8, body.end())});
                } else if (type == kBye) {
                    end_reason = kPlayerLeft;
                    end_aid = aid;
                }
            }
        }
        if (end_reason != 0) end_game(end_reason, end_aid, g_guests[end_aid].name);
        return;
    }
    while (!g_link.messages.empty()) {
        auto [type, body] = std::move(g_link.messages.front());
        g_link.messages.pop_front();
        lobby::Reader r(body);
        switch (type) {
            case kWelcome:
                g_own_aid = r.u8();
                g_language = u8(r.u8());
                g_token = r.u64();
                if (g_relay_mode) g_token = g_settings.token;
                g_welcomed = r.ok;
                break;
            case kJoined:
                g_joined = int(r.u16());
                break;
            case kStep:
                g_steps.push_back(std::move(body));
                break;
            case kBlock:
                if (body.size() >= 8) {
                    const u64 step = r.u64();
                    g_tagged_blocks.push_back({step, std::vector<u8>(body.begin() + 8, body.end())});
                }
                break;
            case kGameOver: {
                const int reason = int(r.u8());
                const u32 aid = r.u8();
                const std::string name = r.str();
                conn_close(g_main);
                finish_game(end_text(reason, aid, name));
            }
            default: break;
        }
    }
}

bool guest_reachable(const Guest& guest) {
    const bool attached = g_relay_mode ? g_main_attached && !guest.gone : guest.conn.open();
    return attached && now_ms() - guest.link.heard < kSilence;
}

bool host_reachable() { return g_main_attached && !g_host_gone && now_ms() - g_link.heard < kSilence; }

// The host sends again what a guest is missing, a piece at a time.
void continue_resend(Guest& guest) {
    Link& link = guest.link;
    while (link.resend_next != 0) {
        if (link.resend_next >= link.next) {
            link.resend_next = 0;
            break;
        }
        if (g_relay_mode ? !g_main_attached : !guest.conn.open()) break;
        if ((g_relay_mode ? g_main.out.size() : guest.conn.out.size()) > (size_t(256) << 10)) break;
        transmit(link, kData, link.history[link.resend_next - 1]);
        ++g_stats.resent;
        ++link.resend_next;
    }
}

// Pings, dead connections, and players who stay away too long.
void keep_alive() {
    const u64 now = now_ms();
    const u64 give_up = u64(std::max(1, g_settings.give_up_seconds)) * 1000;
    if (g_main.open() && now - g_main.heard > kSilence) main_lost("silent");
    if (g_relay_mode || !is_host()) {
        if (g_main_attached) g_main_down_since = 0;
        else if (g_main_down_since == 0) g_main_down_since = now;
    }
    if (g_relay_mode && g_main_attached && now - g_server_pinged >= kPingEvery) {
        g_server_pinged = now;
        lobby::Writer w;
        w.u64(now);
        w.u16(std::min<u32>(g_stats.server_ping_ms, 65535));
        if (!conn_send(g_main, lobby::kPing, w.bytes)) main_lost("cannot send");
    }
    if (!is_host()) {
        if (g_main_attached) link_ping(g_link);
        if (host_reachable()) {
            if (g_link.down_since != 0) std::printf("net: the host is there again\n");
            g_link.down_since = 0;
        } else if (g_link.down_since == 0) {
            g_link.down_since = now;
            std::printf("net: the host cannot be reached (step %llu)\n", static_cast<unsigned long long>(g_stats.steps));
        } else if (now - g_link.down_since > give_up) {
            conn_close(g_main);
            finish_game(end_text(kHostLost, 0, ""));
        }
        return;
    }
    u32 lost = 0;
    for (auto& [aid, guest] : g_guests) {
        if (!g_relay_mode && guest.conn.open() && now - guest.conn.heard > kSilence) guest_conn_lost(guest, "silent");
        link_ping(guest.link);
        continue_resend(guest);
        if (!guest.welcomed) continue;
        if (guest_reachable(guest)) {
            if (guest.link.down_since != 0) std::printf("net: player %u is there again\n", aid + 1);
            guest.link.down_since = 0;
        } else if (guest.link.down_since == 0) {
            guest.link.down_since = now;
            std::printf("net: player %u cannot be reached (step %llu)\n", aid + 1, static_cast<unsigned long long>(g_stats.steps));
        } else if (now - guest.link.down_since > give_up && lost == 0) {
            lost = aid;
        }
    }
    if (lost != 0) end_game(kPlayerLost, lost, g_guests[lost].name);
}

void pump(int wait_ms) {
    wait_sockets(wait_ms);
    if (!is_host() || g_relay_mode) service_main();
    else service_direct_host();
    keep_alive();
    drain();
}

int seconds_left(u64 since) {
    if (since == 0) return g_settings.give_up_seconds;
    const u64 waited = (now_ms() - since) / 1000;
    return std::max(0, g_settings.give_up_seconds - int(waited));
}

void describe_wait() {
    g_wait_second.clear();
    if ((g_relay_mode || !is_host()) && !g_main_attached) {
        g_wait_first = g_relay_mode ? "Verbindung zum Server unterbrochen" : "Verbindung zum Host unterbrochen";
        g_wait_second = "Verbinde neu ... noch " + std::to_string(seconds_left(g_main_down_since)) + " s";
        return;
    }
    if (!is_host()) {
        if (host_reachable()) {
            g_wait_first = "Warte auf die anderen Spieler ...";
        } else {
            g_wait_first = "Der Host antwortet nicht";
            g_wait_second = "Warte ... noch " + std::to_string(seconds_left(g_link.down_since)) + " s";
        }
        return;
    }
    for (auto& [aid, guest] : g_guests) {
        if (!guest.connected || !guest.records.empty()) continue;
        if (guest.rejoining && guest_reachable(guest)) {
            g_wait_first = player_text(aid, guest.name) + " kommt zurück";
            g_wait_second = "und holt die Partie nach ...";
        } else if (guest_reachable(guest)) {
            g_wait_first = "Warte auf " + player_text(aid, guest.name) + " ...";
        } else {
            g_wait_first = player_text(aid, guest.name) + ": Verbindung unterbrochen";
            g_wait_second = "Warte auf die Rückkehr ... noch " + std::to_string(seconds_left(guest.link.down_since)) + " s";
        }
        return;
    }
    g_wait_first = "Warte auf die anderen Spieler ...";
}

// Takes in messages until `ready` says yes; the window is called about every 100 ms meanwhile.
template <class Ready>
void wait_for(Ready ready) {
    u64 last_call = now_ms();
    for (;;) {
        if (ready()) break;  // before waiting: what is needed is often there already
        pump(10);
        if (ready()) break;
        if (now_ms() - last_call >= 100) {
            last_call = now_ms();
            describe_wait();
            if (g_wait_callback != nullptr) g_wait_callback();
        }
    }
    g_wait_first.clear();
    g_wait_second.clear();
}

// --- Calling the game's own code from here ---
constexpr u32 kReturnMark = 0xFFFF0F00;

void call_game(u32 address, u32 r0, u32 r1, u32 r2) {
    Cpu& c = machine_cpu();
    const Cpu saved = c;
    c.r[0] = r0;
    c.r[1] = r1;
    c.r[2] = r2;
    c.r[14] = kReturnMark;
    c.pc = address;
    c.thumb = 0;
    for (u64 n = 1; c.pc != kReturnMark; ++n) {
        machine_step();
        if ((n & 0x3FFFFFF) == 0) {
            std::printf("net: call of %08x still running after %llu blocks: pc=%08x lr=%08x cpsr=%08x\n", address,
                        static_cast<unsigned long long>(n), c.pc, c.r[14], c.cpsr());
        }
    }
    const Cpu after = c;
    c = saved;
    (void)after;
}

void finish(Cpu& c, u32 result) {
    c.r[0] = result;
    c.branch_exchange(c.r[14]);
}

u32 wh(u32 offset) { return rd32(kWhState + offset); }
void set_wh_state(u32 state) { wr32(kWhState + 0x1C, state); }
void set_bitmap(u16 bitmap) { wr16(kWhState + 0xE, bitmap); }

u16 connected_bitmap() {
    u16 bitmap = 1;
    for (const auto& [aid, guest] : g_guests) {
        if (guest.connected) bitmap |= u16(1u << aid);
    }
    return bitmap;
}

// --- Host: download play reported to the game through its own event handler ---
bool g_mb_running = false;
bool g_mb_ending = false;

void write_user_info(const Guest& guest) {
    for (u32 i = 0; i < 0x16; ++i) wr8(kUserInfo + i, 0);
    wr8(kUserInfo, u8(guest.aid << 4));
    const size_t length = std::min<size_t>(guest.name.size(), 10);
    wr8(kUserInfo + 1, u8(length));
    for (size_t i = 0; i < length; ++i) wr16(kUserInfo + 2 + u32(i) * 2, u8(guest.name[i]));
}

void mb_event(const Guest& guest, u32 event) {
    // The connect event carries a station address at +0xA; any distinct one will do.
    for (u32 i = 0; i < 0x10; ++i) wr8(kScratch + i, 0);
    for (u32 i = 0; i < 6; ++i) wr8(kScratch + 0xA + i, u8(0x10 + guest.aid + i));
    std::printf("net: download event %u for guest %u\n", event, guest.aid);
    call_game(kMbCallback, guest.aid, event, kScratch);
}

void take_in_guest(Guest& guest, const std::vector<u8>& hello) {
    guest.name.assign(hello.begin(), hello.end());
    guest.welcomed = true;
    guest.token = g_token_random() | 1;
    // What a download boot leaves to the guest: its aid and the host's download parameter, whose
    // first byte is the game language the host chose (0x020C9210, read back at 0x0201A8B8).
    lobby::Writer w;
    w.u8(guest.aid);
    w.u8(rd8(kLanguage));
    w.u64(guest.token);
    link_send(guest.link, kWelcome, w.bytes);
    std::printf("net: guest %u \"%s\" is here\n", guest.aid, guest.name.c_str());
}

// A download boot leaves the parent's BssDesc at 0x027FFC42 (read by 0x020756DC: the GGID is the
// first four bytes of the SSID, the TGID the next two) and a 32-byte download parameter at 0x027FFBE0.
void write_download_boot(u8 language) {
    for (u32 i = 0; i < 0x3C; ++i) wr8(kParentBssDesc + i, 0);
    wr16(kParentBssDesc, 0x3C / 2);                                         // length in halfwords
    for (u32 i = 0; i < 6; ++i) wr8(kParentBssDesc + 4 + i, u8(0x10 + i));  // the host's address
    wr16(kParentBssDesc + 0xA, 8);                                          // SSID length
    wr16(kParentBssDesc + 0xC, u16(kGgid & 0xFFFF));  // halfword-aligned only, so not one wr32
    wr16(kParentBssDesc + 0xE, u16(kGgid >> 16));
    wr16(kParentBssDesc + 0x10, 1);                                         // TGID
    wr16(kParentBssDesc + 0x36, 1);                                         // channel
    for (u32 i = 0; i < 0x20; ++i) wr8(kDownloadParameter + i, 0);
    wr8(kDownloadParameter, language);
}

// --- Native replacements ---

bool host_hook(Cpu& c) {
    switch (c.pc) {
        case 0x02078B48: finish(c, 0); return true;   // MB_Init
        case 0x02078D9C: finish(c, 0); return true;   // MB parameters
        case 0x02074304: finish(c, 0); return true;   // registers the event handler (it is fixed)
        case 0x020790EC:                              // MB_StartParent
            g_mb_running = true;
            finish(c, 0);
            return true;
        case 0x02075778: finish(c, 1); return true;   // the guest program: nothing to send here
        case 0x020757F8: finish(c, 1); return true;
        case 0x02075CCC: finish(c, 1); return true;
        case 0x0207432C: {                            // a guest's user info
            const auto hit = g_guests.find(c.r[0]);
            if (hit == g_guests.end() || !hit->second.welcomed) {
                finish(c, 0);
            } else {
                write_user_info(hit->second);
                finish(c, kUserInfo);
            }
            return true;
        }
        case 0x02074404: {                            // the game answers a guest: 0 kick, 1 accept, 2 send, 3 boot
            std::printf("net: game answers guest %u with %u\n", c.r[0], c.r[1]);
            auto hit = g_guests.find(c.r[0]);
            if (hit != g_guests.end()) {
                Guest& guest = hit->second;
                if (c.r[1] == 1) guest.mb_stage = 3;
                else if (c.r[1] == 2) guest.mb_stage = 5;
                else if (c.r[1] == 3) guest.mb_stage = 7;
            }
            finish(c, 1);
            return true;
        }
        case 0x020743B0: finish(c, 1); return true;   // a guest can be booted
        case 0x02079224: finish(c, 0); return true;   // disconnect a guest: they stay for now
        case 0x020791E8:                              // MB_End
            g_mb_ending = true;
            finish(c, 0);
            return true;
        default: return false;
    }
}

u32 g_fs_start_return = 0;  // where the guest's FS start-up returns to; the boot type is 1 till then

void take_joined(int bitmap) {
    set_bitmap(u16(bitmap));
    wr16(kWhState, u16(g_own_aid));  // WH_Initialize cleared it
    set_wh_state(kDataSharing);
    g_records_sent = 0;
    g_steps_taken = 0;
    std::printf("net: taken in, players %#x, input delay %d\n", bitmap, g_settings.input_delay);
}

bool wh_hook(Cpu& c) {
    switch (c.pc) {
        case 0x0200932C:  // file system start-up, which a download guest leaves to WFS
            if (g_settings.role != NetRole::Guest) return false;
            // Run the card start-up instead: the guest has the ROM. The SDK's FS start-up also skips
            // the ROM archive on a download boot (0x020692A0), so it sees a card boot until this
            // function returns. Entered as usual (push {r3,lr}), on where a card start goes on.
            g_fs_start_return = c.r[14];
            wr16(kBootType, 1);
            wr32(kCardEnabled, 1);  // CARD_Init leaves card access off on a download boot (0x0206E5E4)
            c.r[13] -= 8;
            wr32(c.r[13], c.r[3]);
            wr32(c.r[13] + 4, c.r[14]);
            c.pc = 0x0200934C;
            return true;
        case 0x02026B94:  // switch the radio on
            wr8(kNetContext, 1);
            finish(c, 0);
            return true;
        case 0x02026C04:  // and off
            wr8(kNetContext, 0);
            finish(c, 0);
            return true;
        case 0x02024954:  // WH_Initialize
            wr16(kWhState, 0);
            set_bitmap(0);
            set_wh_state(kIdle);
            finish(c, 1);
            return true;
        case 0x020245D0:  // start measuring channels
            set_wh_state(kMeasure);
            finish(c, 1);
            return true;
        case 0x02024818:  // measured: channel 1
            set_wh_state(kIdle);
            finish(c, 1);
            return true;
        // WFS, the file system over the radio: guests read their own ROM here, so it is ready at
        // once and sends nothing.
        case 0x02022EFC:  // state: 2 = ready
            finish(c, 2);
            return true;
        case 0x02022CF8:  // start
        case 0x02022F18:  // host side set-up
        case 0x02022F70:  // options
        case 0x02022DA0:  // end
            finish(c, 0);
            return true;
        case 0x020229E0:  // data sharing set-up, host
        case 0x02022BF8:  // and guest
            finish(c, 1);
            return true;
        case 0x02024A9C:  // host opens the session
            wr16(kWhState, 0);
            set_bitmap(connected_bitmap());
            set_wh_state(kDataSharing);
            g_own_records.clear();
            finish(c, 1);
            return true;
        case 0x020238B0: {  // guest connects: waits here until the host has taken it in
            // In the original a guest only starts once the host has finished the download, so the
            // game's own timeout must not run while the host is still in its menu.
            // net_tick may have taken the guest in already, while the state was busy.
            if (wh(0x1C) != kDataSharing) {
                wait_for([] { return g_joined >= 0; });
                take_joined(g_joined);
                g_joined = -1;
            }
            finish(c, 1);
            return true;
        }
        case 0x02024E08:  // WH_End
            set_wh_state(kIdle);
            finish(c, 1);
            return true;
        case 0x02024F4C:  // WH_Finalize
            set_wh_state(kStop);
            finish(c, 1);
            return true;
        case 0x02024D24: {  // shared data of one player
            const u32 aid = c.r[0];
            const u16 bitmap = rd16(kWhState + 0xE);
            finish(c, (aid < 4 && (bitmap & (1u << aid))) ? kSharedRecords + aid * kRecordSize : 0);
            return true;
        }
        case 0x02024CC4:  // sets the receive callback of the data port (r0, 0 = none)
            wr32(kWhState + 0x14, c.r[0]);
            finish(c, 0);
            return true;
        case 0x02024410: {  // sends r1 bytes at r0 through the port; r2 is called once it is out
            lobby::Writer w;
            w.u64(is_host() ? g_stats.steps : 0);
            for (u32 i = 0; i < c.r[1]; ++i) w.u8(rd8(c.r[0] + i));
            if (is_host()) {
                for (auto& [aid, guest] : g_guests) {
                    if (guest.connected) link_send(guest.link, kBlock, w.bytes);
                }
            } else {
                link_send(g_link, kBlock, w.bytes);
            }
            ++g_stats.blocks;
            std::printf("net: sends %u bytes at step %llu\n", c.r[1], static_cast<unsigned long long>(g_stats.steps));
            // WM reports the send later; the game marks the send as running only after this call.
            if (c.r[2] != 0) g_send_done.push_back(c.r[2]);
            finish(c, 1);
            return true;
        }
        default: return false;
    }
}

// `step` is the host's step number, so all players count alike (the host may step alone first).
void store_step(const std::vector<u8>& records, u16 bitmap, u64 step) {
    for (u32 i = 0; i < 4 * kRecordSize; ++i) wr8(kSharedRecords + i, records[i]);
    set_bitmap(bitmap);
    u32 sum = 0x811C9DC5u;
    for (u8 b : records) sum = (sum ^ b) * 0x01000193u;
    g_stats.checksum = sum;
    g_stats.steps = step;
}

// The exchange: every connected player's record, the same for everybody. A player's record of its
// N-th picture in the exchange goes into its N-th step; with an input delay of D the step comes D
// pictures later, and the first D pictures have none. The game does not run its tick on a picture
// without shared data (0x01FFC4D4), as with the DS radio while data sharing starts.
bool step_hook(Cpu& c) {
    if (c.pc != 0x02024D44) return false;
    const u32 send = c.r[0];
    std::vector<u8> own(kRecordSize);
    for (u32 i = 0; i < kRecordSize; ++i) own[i] = rd8(send + i);
    const size_t delay = size_t(std::max(0, g_settings.input_delay));
    pump(0);
    if (is_host()) {
        g_own_records.push_back(std::move(own));
        if (g_own_records.size() <= delay) {
            finish(c, 0);
            return true;
        }
        wait_for([] {
            for (const auto& [aid, guest] : g_guests) {
                if (guest.connected && guest.records.empty()) return false;
            }
            return true;
        });
        std::vector<u8> records(4 * kRecordSize, 0);
        std::copy(g_own_records.front().begin(), g_own_records.front().end(), records.begin());
        g_own_records.pop_front();
        const u16 bitmap = connected_bitmap();
        for (auto& [aid, guest] : g_guests) {
            if (!guest.connected) continue;
            std::copy(guest.records.front().begin(), guest.records.front().end(), records.begin() + aid * kRecordSize);
            guest.records.pop_front();
        }
        const u64 step = g_stats.steps + 1;
        std::vector<u8> packet(10 + records.size());
        std::memcpy(packet.data(), &bitmap, 2);
        std::memcpy(packet.data() + 2, &step, 8);
        std::copy(records.begin(), records.end(), packet.begin() + 10);
        for (auto& [aid, guest] : g_guests) {
            if (guest.connected) link_send(guest.link, kStep, packet);
        }
        store_step(records, bitmap, step);
    } else {
        link_send(g_link, kRecord, own);
        ++g_records_sent;
        if (g_records_sent - g_steps_taken <= delay) {
            finish(c, 0);
            return true;
        }
        wait_for([] { return !g_steps.empty(); });
        const std::vector<u8> body = std::move(g_steps.front());
        g_steps.pop_front();
        ++g_steps_taken;
        u16 bitmap;
        std::memcpy(&bitmap, body.data(), 2);
        u64 step;
        std::memcpy(&step, body.data() + 2, 8);
        store_step(std::vector<u8>(body.begin() + 10, body.end()), bitmap, step);
    }
    g_stats.players = u32(std::popcount(rd16(kWhState + 0xE)));
    finish(c, 1);
    return true;
}

}  // namespace

void net_start(const NetSettings& settings) {
    g_settings = settings;
    if (settings.role == NetRole::Off) return;
    plat::net_ready();
    g_relay_mode = settings.relay != 0;
    if (settings.rejoin && settings.role == NetRole::Guest) {
        // A new program coming back: connects itself (service_main), sends nothing until the host
        // has said what it has, and plays the game so far again from the host's messages.
        g_relay_mode = settings.via_server;
        g_ever_connected = true;
        g_rejoin_waiting = true;
        g_own_aid = u32(settings.slot);
        g_token = settings.token;
        std::printf("net: comes back as player %d%s, room %s\n", settings.slot + 1,
                    g_relay_mode ? " through the server" : "", settings.code.c_str());
    } else if (g_relay_mode) {
        // Through the server: the lobby's connection, already open (lobby.h).
        g_main.s = plat::from_handle(settings.relay);
        make_nonblocking(g_main.s);
        g_main.in = settings.relay_inbox;
        g_main.channel = settings.relay_channel;
        g_main.out = settings.relay_outbox;
        g_main.key_checked = true;
        g_main.heard = now_ms();
        g_main_attached = true;
        g_ever_connected = true;
        g_token = settings.token;
        std::printf("net: %s through the server, room %s, input delay %d\n", is_host() ? "host" : "guest",
                    settings.code.c_str(), settings.input_delay);
    } else if (settings.role == NetRole::Host) {
        sockaddr_in where{};
        where.sin_family = AF_INET;
        where.sin_port = htons(settings.port);
        inet_pton(AF_INET, settings.address.c_str(), &where.sin_addr);
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        plat::set_option(g_listen, SOL_SOCKET, SO_REUSEADDR, true);
        if (bind(g_listen, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0 || listen(g_listen, 4) != 0) {
            runtime_abort("network: cannot listen", settings.port);
        }
        std::printf("net: host on port %u, input delay %d\n", settings.port, settings.input_delay);
    } else {
        for (int attempt = 0; !g_main.open(); ++attempt) {
            if (attempt > 100) runtime_abort("network: cannot reach the host", settings.port);
            g_last_attempt = 0;
            start_connect();
            plat::sleep_ms(100);
            check_connect();
        }
    }
    if (settings.role == NetRole::Guest) {
        g_link.heard = now_ms();
        link_send(g_link, kHello, std::vector<u8>(settings.name.begin(), settings.name.end()));
        wait_for([] { return g_welcomed; });
        wr16(kWhState, u16(g_own_aid));
        write_download_boot(g_language);
        std::printf("net: guest, player %u, language %u\n", g_own_aid, g_language);
        write_rejoin_file();
    }
    g_started = true;
}

namespace {

// The character chosen in the lobby is tapped on the game's own character screen, so the game
// keeps its own choice logic. The screen is recognised by its prompt, which the game fetches through
// get_message (0x020187F4, r0 = file << 16 | message) when it asks this player. Message ids are the
// same in all languages; files by index from the path table at 0x02098BA0.
struct CharacterPrompt {
    u32 file;
    u32 host;  // "Select a character."
    u32 p2;    // "P2, select a character."; P3 and P4 follow
    u32 guests;
};
constexpr CharacterPrompt kCharacterPrompts[] = {
    {0x04, 0x0F, 0x11, 3},  // mpd_030_party_mode
    {0x17, 0x1E, 0x4C, 3},  // mpd_097_mgbridge
    {0x19, 0x01, 0x08, 3},  // mpd_101_mgfreeplay
    {0x1A, 0x00, 0x24, 3},  // mpd_102_mgkachinuki
    {0x1B, 0x00, 0x23, 3},  // mpd_103_mgcup
    {0x1C, 0x00, 0x2E, 3},  // mpd_104_mgscoreat
    {0x1E, 0x01, 0x0B, 3},  // mpd_110_puz_mode
    {0x20, 0x12, 0x13, 1},  // mpd_130_extra_mode
};
constexpr u32 kGetMessage = 0x020187F4;
constexpr int kPickDelay = 60;  // ticks: the screen slides in after its prompt is fetched
constexpr int kPickFrames = 6;

int g_pick_countdown = -1;  // ticks until the tap; -1 none
int g_pick_frames = 0;

bool is_own_prompt(u32 id) {
    const u32 file = id >> 16, message = id & 0xFFFF;
    for (const CharacterPrompt& prompt : kCharacterPrompts) {
        if (prompt.file != file) continue;
        if (g_settings.role == NetRole::Host) return message == prompt.host;
        return g_own_aid >= 1 && g_own_aid <= prompt.guests && message == prompt.p2 + g_own_aid - 1;
    }
    return false;
}

void watch_character_prompt(const Cpu& c) {
    if (c.pc != kGetMessage || g_settings.character < 0 || !is_own_prompt(c.r[0])) return;
    g_pick_countdown = kPickDelay;
    std::printf("net: character screen (message %08x) at step %llu\n", c.r[0], static_cast<unsigned long long>(g_stats.steps));
}

}  // namespace

bool net_auto_touch(u32& x, u32& y) {
    if (g_pick_frames <= 0) return false;
    const int character = g_settings.character;
    x = character < 4 ? 75 : 184;  // two columns of four buttons
    y = 36 + 30 * u32(character % 4);
    return true;
}

bool net_hook(Cpu& c) {
    if (!g_started) return false;
    if (g_fs_start_return != 0 && (c.pc | c.thumb) == g_fs_start_return) {
        wr16(kBootType, 2);
        g_fs_start_return = 0;
    }
    if (c.pc < 0x02009000 || c.pc > 0x02080000) return false;
    watch_character_prompt(c);
    if (g_settings.role == NetRole::Host && host_hook(c)) return true;
    if (wh_hook(c)) return true;
    return step_hook(c);
}

namespace {

// Hands arrived port data to the game, as WM's port callback would. A guest takes the host's data
// after the step that followed its sending, where it came in the stream before messages were numbered.
void deliver_blocks() {
    const std::vector<u32> done = std::move(g_send_done);
    g_send_done.clear();
    for (u32 callback : done) call_game(callback, 1, 0, 0);
    while (!g_tagged_blocks.empty() && g_tagged_blocks.front().first < g_stats.steps) {
        g_blocks.push_back({0, std::move(g_tagged_blocks.front().second)});
        g_tagged_blocks.pop_front();
    }
    while (!g_blocks.empty()) {
        const u32 receive = wh(0x14);
        if (receive == 0) return;  // nobody listens yet; keep it
        const Block block = g_blocks.front();
        g_blocks.pop_front();
        if (block.data.size() > kBlockLimit) runtime_abort("network: port data too large", u32(block.data.size()));
        for (u32 i = 0; i < block.data.size(); ++i) wr8(kBlockBuffer + i, block.data[i]);
        std::printf("net: port data from %u, %zu bytes, to %08x at step %llu\n", block.aid, block.data.size(), receive,
                    static_cast<unsigned long long>(g_stats.steps));
        call_game(receive, block.aid, kBlockBuffer, u32(block.data.size()));
        std::printf("net: delivered\n");
    }
}

}  // namespace

void net_tick() {
    if (!g_started) return;
    if (g_pick_frames > 0) --g_pick_frames;
    if (g_pick_countdown > 0 && --g_pick_countdown == 0) {
        g_pick_countdown = -1;
        g_pick_frames = kPickFrames;
        std::printf("net: picks %s at step %llu\n", lobby::character_name(g_settings.character),
                    static_cast<unsigned long long>(g_stats.steps));
    }
    pump(0);
    deliver_blocks();
    if (g_settings.role == NetRole::Host) {
        // Download play: one stage per tick, in the order the game expects its events. The stage moves
        // on before the event, because the game's handler answers within it.
        if (g_mb_running && rd16(kMbState + 8) >= 2) {
            for (auto& [aid, guest] : g_guests) {
                if (!guest.welcomed) continue;
                if (guest.mb_stage == 0) { guest.mb_stage = 1; mb_event(guest, kConnected); }
                else if (guest.mb_stage == 1) { guest.mb_stage = 2; mb_event(guest, kRequested); }
                else if (guest.mb_stage == 3) { guest.mb_stage = 4; mb_event(guest, kWaitToSend); }
                else if (guest.mb_stage == 5) { guest.mb_stage = 6; mb_event(guest, kSendComplete); }
                else if (guest.mb_stage == 7) { guest.mb_stage = 8; mb_event(guest, kBootStartable); }
            }
            if (g_mb_ending) {
                g_mb_ending = false;
                g_mb_running = false;
                for (auto& [aid, guest] : g_guests) {
                    if (!guest.welcomed) continue;
                    mb_event(guest, kEnd);
                    break;
                }
            }
        }
        // After the download, guests join the exchange as soon as the host's session is open.
        if (wh(0x1C) == kDataSharing) {
            for (auto& [aid, guest] : g_guests) {
                if (guest.connected || guest.mb_stage < 8) continue;
                guest.connected = true;
                const u16 bitmap = connected_bitmap();
                set_bitmap(bitmap);
                std::vector<u8> body(2);
                std::memcpy(body.data(), &bitmap, 2);
                link_send(guest.link, kJoined, body);
                std::printf("net: guest %u joins the exchange at step %llu\n", aid,
                            static_cast<unsigned long long>(g_stats.steps));
            }
        }
    } else if (wh(0x1C) == kBusy && g_joined >= 0) {
        // The guest waits for the host to take it in.
        take_joined(g_joined);
        g_joined = -1;
    }
}

const NetStats& net_stats() { return g_stats; }

void net_set_wait_callback(NetWaitCallback callback) { g_wait_callback = callback; }

void net_set_end_callback(NetEndCallback callback) { g_end_callback = callback; }

std::string net_status_line() {
    if (g_settings.role == NetRole::Off) return {};
    const int players = g_settings.players > 0 ? g_settings.players : int(std::popcount(rd16(kWhState + 0xE)));
    std::string line = "P" + std::to_string((is_host() ? 0 : g_own_aid) + 1) + " von " + std::to_string(std::max(players, 1));
    if (is_host()) {
        std::string pings;
        for (const auto& [aid, guest] : g_guests) {
            if (!guest.welcomed) continue;
            if (!pings.empty()) pings += ", ";
            pings += "P" + std::to_string(aid + 1) + " ";
            pings += guest_reachable(guest) ? std::to_string(g_stats.ping_ms[aid & 3]) + " ms" : "weg";
        }
        if (!pings.empty()) line += ", Ping " + pings;
    } else {
        line += host_reachable() ? ", Ping " + std::to_string(g_stats.ping_ms[0]) + " ms" : ", Host nicht erreichbar";
    }
    return line;
}

void net_wait_text(std::string& first, std::string& second) {
    first = g_wait_first;
    second = g_wait_second;
    if (first.empty() && net_catching_up()) {
        u64 newest = 0;
        std::memcpy(&newest, g_steps.back().data() + 2, 8);
        first = "Hole die laufende Partie nach ...";
        second = "Schritt " + std::to_string(g_stats.steps) + " von " + std::to_string(newest);
    }
}

bool net_catching_up() { return g_settings.role == NetRole::Guest && g_steps.size() > 30; }

bool net_load_rejoin(const std::string& path, NetSettings& settings) {
    FILE* f = plat::open_file(path.c_str(), "r");
    if (f == nullptr) return false;
    char line[512];
    int found = 0;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = text.substr(0, eq), value = text.substr(eq + 1);
        ++found;
        if (key == "via_server") settings.via_server = value == "1";
        else if (key == "address") settings.address = value;
        else if (key == "port") settings.port = u16(std::atoi(value.c_str()));
        else if (key == "code") settings.code = value;
        else if (key == "slot") settings.slot = std::atoi(value.c_str());
        else if (key == "token") settings.token = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "players") settings.players = std::atoi(value.c_str());
        else if (key == "input_delay") settings.input_delay = std::atoi(value.c_str());
        else if (key == "give_up") settings.give_up_seconds = std::atoi(value.c_str());
        else if (key == "character") settings.character = std::atoi(value.c_str());
        else if (key == "name") settings.name = value;
        else if (key == "server_key") {
            settings.server_key.clear();
            for (size_t i = 0; i + 1 < value.size(); i += 2) {
                settings.server_key.push_back(u8(std::strtoul(value.substr(i, 2).c_str(), nullptr, 16)));
            }
            --found;  // older files have none
        } else --found;
    }
    std::fclose(f);
    if (found < 11 || settings.token == 0 || settings.slot < 1 || settings.slot > 3) return false;
    settings.role = NetRole::Guest;
    settings.rejoin = true;
    settings.relay = 0;
    return true;
}

void net_leave() {
    if (g_settings.role == NetRole::Off || g_left_on_purpose) return;
    g_left_on_purpose = true;
    remove_rejoin_file();
    std::printf("net: leaves the game at step %llu\n", static_cast<unsigned long long>(g_stats.steps));
    if (is_host()) {
        announce_end(kHostLeft, 0, "");
    } else {
        link_send(g_link, kBye, {});
        if (g_relay_mode && g_main.open()) conn_send(g_main, lobby::kLeave, {});
        flush_everything(1000);
    }
    conn_close(g_main);
    for (auto& [aid, guest] : g_guests) conn_close(guest.conn);
}
