#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "net.h"

#include "machine.h"
#include "memory.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace {

// --- Addresses in the game, all read from its code (see MULTIPLAYER_ROADMAP.md, phase 8) ---
constexpr u32 kWhState = 0x020D1AE0;      // +0 own aid, +0xE connected bitmap, +0x1C system state
constexpr u32 kNetContext = 0x020D45A0;   // +0 wireless powered (s8)
constexpr u32 kMbState = 0x020C9234;      // +8 state, +0xA.. +0x14 bitmaps per download stage
constexpr u32 kMbCallback = 0x02017920;   // the game's download event handler (aid, event, info)
constexpr u32 kRecordSize = 0x26;
// Free space in main RAM that the game does not use: the ARM7's area, which nothing runs in here.
constexpr u32 kScratch = 0x023F8000;
constexpr u32 kSharedRecords = kScratch + 0x100;  // 4 records, handed out by the shared-data getter
constexpr u32 kUserInfo = kScratch + 0x200;       // a guest's name, as MB hands it to the game

enum WhState : u32 { kStop = 0, kIdle = 1, kBusy = 3, kDataSharing = 5, kMeasure = 7 };
enum MbEvent : u32 {
    kConnected = 2, kRequestAccepted = 5, kSendComplete = 7, kBootStartable = 9, kRequested = 10,
    kEnd = 12, kWaitToSend = 14,
};

NetSettings g_settings;
NetStats g_stats;
bool g_started = false;

// --- Transport: TCP on the local machine for now; length-prefixed messages ---
enum Message : u8 { kHello = 1, kWelcome = 2, kJoined = 3, kRecord = 4, kStep = 5 };

SOCKET g_listen = INVALID_SOCKET;
SOCKET g_host = INVALID_SOCKET;  // guest side
std::vector<u8> g_host_inbox;
struct Guest {
    SOCKET socket = INVALID_SOCKET;
    std::string name;
    u32 aid = 0;
    int mb_stage = 0;        // how far the download-play events have come
    bool connected = false;  // takes part in the record exchange
    u64 first_step = 0;      // host step at which this guest's first record counts
    std::deque<std::vector<u8>> records;
    std::vector<u8> inbox;
};
std::map<u32, Guest> g_guests;  // by aid

void send_message(SOCKET s, u8 type, const std::vector<u8>& body) {
    std::vector<u8> frame(5 + body.size());
    const u32 length = u32(body.size() + 1);
    std::memcpy(frame.data(), &length, 4);
    frame[4] = type;
    std::copy(body.begin(), body.end(), frame.begin() + 5);
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = send(s, reinterpret_cast<const char*>(frame.data() + sent), int(frame.size() - sent), 0);
        if (n <= 0) runtime_abort("network: send failed", u32(WSAGetLastError()));
        sent += size_t(n);
    }
}

// Reads what has arrived; returns complete messages. `wait_ms` < 0 blocks until one is there.
bool read_message(SOCKET s, std::vector<u8>& inbox, u8& type, std::vector<u8>& body, int wait_ms) {
    for (;;) {
        if (inbox.size() >= 4) {
            u32 length;
            std::memcpy(&length, inbox.data(), 4);
            if (inbox.size() >= 4 + length) {
                type = inbox[4];
                body.assign(inbox.begin() + 5, inbox.begin() + 4 + length);
                inbox.erase(inbox.begin(), inbox.begin() + 4 + length);
                return true;
            }
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval timeout{0, 0};
        if (wait_ms >= 0) timeout = {wait_ms / 1000, (wait_ms % 1000) * 1000};
        timeval long_wait{20, 0};  // a player gone for 20 s ends the run for now (phase 11 handles it)
        const int ready = select(0, &set, nullptr, nullptr, wait_ms < 0 ? &long_wait : &timeout);
        if (ready == 0) {
            if (wait_ms < 0) runtime_abort("network: nothing from the other side for 20 s", 0);
            return false;
        }
        char buffer[4096];
        const int n = recv(s, buffer, sizeof buffer, 0);
        if (n <= 0) runtime_abort("network: connection closed", u32(WSAGetLastError()));
        inbox.insert(inbox.end(), buffer, buffer + n);
    }
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
    while (c.pc != kReturnMark) machine_step();
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

void host_accept() {
    if (g_listen == INVALID_SOCKET) return;
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(g_listen, &set);
        timeval zero{0, 0};
        if (select(0, &set, nullptr, nullptr, &zero) <= 0) return;
        SOCKET s = accept(g_listen, nullptr, nullptr);
        if (s == INVALID_SOCKET) return;
        u32 aid = 1;
        while (g_guests.count(aid)) ++aid;
        if (aid > 3) {
            closesocket(s);
            continue;
        }
        Guest& guest = g_guests[aid];
        guest.socket = s;
        guest.aid = aid;
        u8 type;
        std::vector<u8> body;
        read_message(s, guest.inbox, type, body, -1);
        if (type != kHello) runtime_abort("network: guest did not say hello", type);
        guest.name.assign(body.begin(), body.end());
        send_message(s, kWelcome, {u8(aid)});
        std::printf("net: guest %u \"%s\" is here\n", aid, guest.name.c_str());
    }
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
            if (hit == g_guests.end()) {
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

bool wh_hook(Cpu& c) {
    switch (c.pc) {
        case 0x0200932C:  // file system start-up, which a download guest leaves to WFS
            if (g_settings.role != NetRole::Guest) return false;
            // Run the card start-up instead: the guest has the ROM. This enters the function as
            // usual (push {r3,lr}) and goes on where it would for a card start.
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
            finish(c, 1);
            return true;
        case 0x020238B0: {  // guest connects: waits here until the host has taken it in
            // In the original a guest only starts once the host has finished the download, so the
            // game's own timeout must not run while the host is still in its menu.
            for (;;) {
                u8 type;
                std::vector<u8> body;
                if (!read_message(g_host, g_host_inbox, type, body, 60000)) continue;
                if (type != kJoined) continue;
                u16 bitmap;
                std::memcpy(&bitmap, body.data(), 2);
                set_bitmap(bitmap);
                break;
            }
            set_wh_state(kDataSharing);
            std::printf("net: taken in, players %#x\n", rd16(kWhState + 0xE));
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
        default: return false;
    }
}

void store_step(const std::vector<u8>& records, u16 bitmap) {
    for (u32 i = 0; i < 4 * kRecordSize; ++i) wr8(kSharedRecords + i, records[i]);
    set_bitmap(bitmap);
    u32 sum = 0x811C9DC5u;
    for (u8 b : records) sum = (sum ^ b) * 0x01000193u;
    g_stats.checksum = sum;
    ++g_stats.steps;
}

// The exchange: every connected player's record of this step, the same for everybody.
bool step_hook(Cpu& c) {
    if (c.pc != 0x02024D44) return false;
    const u32 send = c.r[0];
    std::vector<u8> own(kRecordSize);
    for (u32 i = 0; i < kRecordSize; ++i) own[i] = rd8(send + i);
    if (g_settings.role == NetRole::Host) {
        host_accept();
        std::vector<u8> records(4 * kRecordSize, 0);
        std::copy(own.begin(), own.end(), records.begin());
        u16 bitmap = connected_bitmap();
        for (auto& [aid, guest] : g_guests) {
            if (!guest.connected) continue;
            while (guest.records.empty()) {
                u8 type;
                std::vector<u8> body;
                read_message(guest.socket, guest.inbox, type, body, -1);
                if (type == kRecord) guest.records.push_back(body);
            }
            std::copy(guest.records.front().begin(), guest.records.front().end(), records.begin() + aid * kRecordSize);
            guest.records.pop_front();
        }
        std::vector<u8> packet(2 + records.size());
        std::memcpy(packet.data(), &bitmap, 2);
        std::copy(records.begin(), records.end(), packet.begin() + 2);
        for (auto& [aid, guest] : g_guests) {
            if (guest.connected) send_message(guest.socket, kStep, packet);
        }
        store_step(records, bitmap);
    } else {
        send_message(g_host, kRecord, own);
        for (;;) {
            u8 type;
            std::vector<u8> body;
            read_message(g_host, g_host_inbox, type, body, -1);
            if (type == kStep) {
                u16 bitmap;
                std::memcpy(&bitmap, body.data(), 2);
                store_step(std::vector<u8>(body.begin() + 2, body.end()), bitmap);
                break;
            }
        }
    }
    g_stats.players = u32(__popcnt(rd16(kWhState + 0xE)));
    finish(c, 1);
    return true;
}

}  // namespace

void net_start(const NetSettings& settings) {
    g_settings = settings;
    if (settings.role == NetRole::Off) return;
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    sockaddr_in where{};
    where.sin_family = AF_INET;
    where.sin_port = htons(settings.port);
    inet_pton(AF_INET, settings.address.c_str(), &where.sin_addr);
    if (settings.role == NetRole::Host) {
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL yes = TRUE;
        setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        if (bind(g_listen, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0 || listen(g_listen, 4) != 0) {
            runtime_abort("network: cannot listen", settings.port);
        }
        std::printf("net: host on port %u\n", settings.port);
    } else {
        g_host = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        for (int attempt = 0; connect(g_host, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0; ++attempt) {
            if (attempt > 100) runtime_abort("network: cannot reach the host", settings.port);
            Sleep(100);
        }
        BOOL yes = TRUE;
        setsockopt(g_host, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
        send_message(g_host, kHello, std::vector<u8>(settings.name.begin(), settings.name.end()));
        u8 type;
        std::vector<u8> body;
        read_message(g_host, g_host_inbox, type, body, -1);
        if (type != kWelcome) runtime_abort("network: no welcome", type);
        wr16(kWhState, body[0]);
        std::printf("net: guest, player %u\n", body[0]);
    }
    g_started = true;
}

bool net_hook(Cpu& c) {
    if (!g_started) return false;
    if (c.pc < 0x02009000 || c.pc > 0x02080000) return false;
    if (g_settings.role == NetRole::Host && host_hook(c)) return true;
    if (wh_hook(c)) return true;
    return step_hook(c);
}

void net_tick() {
    if (!g_started) return;
    if (g_settings.role == NetRole::Host) {
        host_accept();
        // Download play: one stage per tick, in the order the game expects its events. The stage moves
        // on before the event, because the game's handler answers within it.
        if (g_mb_running && rd16(kMbState + 8) >= 2) {
            for (auto& [aid, guest] : g_guests) {
                if (guest.mb_stage == 0) { guest.mb_stage = 1; mb_event(guest, kConnected); }
                else if (guest.mb_stage == 1) { guest.mb_stage = 2; mb_event(guest, kRequested); }
                else if (guest.mb_stage == 3) { guest.mb_stage = 4; mb_event(guest, kWaitToSend); }
                else if (guest.mb_stage == 5) { guest.mb_stage = 6; mb_event(guest, kSendComplete); }
                else if (guest.mb_stage == 7) { guest.mb_stage = 8; mb_event(guest, kBootStartable); }
            }
            if (g_mb_ending) {
                g_mb_ending = false;
                g_mb_running = false;
                if (!g_guests.empty()) mb_event(g_guests.begin()->second, kEnd);
            }
        }
        // After the download, guests join the exchange as soon as the host's session is open.
        if (wh(0x1C) == kDataSharing) {
            for (auto& [aid, guest] : g_guests) {
                if (guest.connected || guest.mb_stage < 8) continue;
                guest.connected = true;
                guest.first_step = g_stats.steps;
                const u16 bitmap = connected_bitmap();
                set_bitmap(bitmap);
                std::vector<u8> body(2);
                std::memcpy(body.data(), &bitmap, 2);
                send_message(guest.socket, kJoined, body);
                std::printf("net: guest %u joins the exchange at step %llu\n", aid,
                            static_cast<unsigned long long>(g_stats.steps));
            }
        }
    } else if (wh(0x1C) == kBusy) {
        // The guest waits for the host to take it in.
        u8 type;
        std::vector<u8> body;
        if (read_message(g_host, g_host_inbox, type, body, 0) && type == kJoined) {
            u16 bitmap;
            std::memcpy(&bitmap, body.data(), 2);
            set_bitmap(bitmap);
            set_wh_state(kDataSharing);
            std::printf("net: taken in, players %#x\n", bitmap);
        }
    }
}

const NetStats& net_stats() { return g_stats; }
