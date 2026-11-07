#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "net.h"

#include "lobby_protocol.h"

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
u32 g_own_aid = 0;  // guest: the player number the host gave
NetStats g_stats;
bool g_started = false;

// --- Transport: TCP on the local machine for now; length-prefixed messages ---
enum Message : u8 { kHello = 1, kWelcome = 2, kJoined = 3, kRecord = 4, kStep = 5, kBlock = 6 };

SOCKET g_listen = INVALID_SOCKET;
SOCKET g_host = INVALID_SOCKET;  // guest side
std::vector<u8> g_host_inbox;
struct Guest {
    SOCKET socket = INVALID_SOCKET;
    std::string name;
    u32 aid = 0;
    int mb_stage = 0;        // how far the download-play events have come
    bool connected = false;  // takes part in the record exchange
    std::deque<std::pair<u8, std::vector<u8>>> queue;  // relayed messages not read yet
    u64 first_step = 0;      // host step at which this guest's first record counts
    std::deque<std::vector<u8>> records;
    std::vector<u8> inbox;
};
std::map<u32, Guest> g_guests;  // by aid

// Through MarioPartyDS-server: the host has one connection for all guests (lobby_protocol.h).
SOCKET g_relay = INVALID_SOCKET;
std::vector<u8> g_relay_inbox;

// Port data (WH send) that has arrived and waits for the game's receive callback: sender aid, bytes.
struct Block {
    u32 aid;
    std::vector<u8> data;
};
std::deque<Block> g_blocks;
std::vector<u32> g_send_done;  // callbacks of sends, called on the next tick

void send_message(SOCKET s, u8 type, const std::vector<u8>& body) {
    std::vector<u8> frame(5 + body.size());
    const u32 length = u32(body.size() + 1);
    std::memcpy(frame.data(), &length, 4);
    frame[4] = type;
    std::copy(body.begin(), body.end(), frame.begin() + 5);
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = send(s, reinterpret_cast<const char*>(frame.data() + sent), int(frame.size() - sent), 0);
        if (n <= 0) runtime_abort("network: send failed, at step", u32(g_stats.steps));
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
        // A player gone for 20 s ends the run for now (phase 11 handles it). Waits in slices of
        // 100 ms, so a window can keep answering in between (net_set_wait_callback).
        const ULONGLONG limit = wait_ms < 0 ? 20000 : ULONGLONG(wait_ms);
        const ULONGLONG begin = GetTickCount64();
        int ready = 0;
        for (;;) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            const ULONGLONG waited = GetTickCount64() - begin;
            const ULONGLONG slice = std::min<ULONGLONG>(100, limit > waited ? limit - waited : 0);
            timeval timeout{0, long(slice * 1000)};
            ready = select(0, &set, nullptr, nullptr, &timeout);
            if (ready != 0 || GetTickCount64() - begin >= limit) break;
            if (g_wait_callback != nullptr) g_wait_callback();
        }
        if (ready == 0) {
            if (wait_ms < 0) runtime_abort("network: nothing from the other side for 20 s, at step", u32(g_stats.steps));
            return false;
        }
        char buffer[4096];
        const int n = recv(s, buffer, sizeof buffer, 0);
        if (n <= 0) runtime_abort("network: connection closed, at step", u32(g_stats.steps));
        inbox.insert(inbox.end(), buffer, buffer + n);
    }
}

void guest_send(Guest& guest, u8 type, const std::vector<u8>& body) {
    if (g_relay == INVALID_SOCKET) {
        send_message(guest.socket, type, body);
        return;
    }
    std::vector<u8> wrapped{u8(guest.aid), type};
    wrapped.insert(wrapped.end(), body.begin(), body.end());
    send_message(g_relay, lobby::kRelayTo, wrapped);
}

void take_in_guest(u32 aid, SOCKET s, const std::vector<u8>& hello);

// Reads one message from the server and files it with its guest. False if none came in time.
bool relay_pump(int wait_ms) {
    u8 type;
    std::vector<u8> body;
    if (!read_message(g_relay, g_relay_inbox, type, body, wait_ms)) return false;
    if (type == lobby::kRelayGone && !body.empty()) runtime_abort("network: a player left the game, player", body[0] + 1u);
    if (type != lobby::kRelayFrom || body.size() < 2) return true;
    const u32 aid = body[0];
    const u8 inner = body[1];
    std::vector<u8> rest(body.begin() + 2, body.end());
    auto it = g_guests.find(aid);
    if (it == g_guests.end()) {
        if (inner == kHello && aid >= 1 && aid <= 3) take_in_guest(aid, INVALID_SOCKET, rest);
        return true;
    }
    it->second.queue.push_back({inner, std::move(rest)});
    return true;
}

bool guest_read(Guest& guest, u8& type, std::vector<u8>& body, int wait_ms) {
    if (g_relay == INVALID_SOCKET) return read_message(guest.socket, guest.inbox, type, body, wait_ms);
    while (guest.queue.empty()) {
        if (!relay_pump(wait_ms)) return false;
    }
    type = guest.queue.front().first;
    body = std::move(guest.queue.front().second);
    guest.queue.pop_front();
    return true;
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

void take_in_guest(u32 aid, SOCKET s, const std::vector<u8>& hello) {
    Guest& guest = g_guests[aid];
    guest.socket = s;
    guest.aid = aid;
    guest.name.assign(hello.begin(), hello.end());
    // What a download boot leaves to the guest: its aid and the host's download parameter, whose
    // first byte is the game language the host chose (0x020C9210, read back at 0x0201A8B8).
    guest_send(guest, kWelcome, {u8(aid), rd8(kLanguage)});
    std::printf("net: guest %u \"%s\" is here\n", aid, guest.name.c_str());
}

void host_accept() {
    if (g_relay != INVALID_SOCKET) {
        while (relay_pump(0)) {
        }
        return;
    }
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
        std::vector<u8> inbox;
        u8 type;
        std::vector<u8> body;
        read_message(s, inbox, type, body, -1);
        if (type != kHello) runtime_abort("network: guest did not say hello", type);
        take_in_guest(aid, s, body);
        g_guests[aid].inbox = std::move(inbox);
    }
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

u32 g_fs_start_return = 0;  // where the guest's FS start-up returns to; the boot type is 1 till then

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
            wr16(kWhState, u16(g_own_aid));  // WH_Initialize cleared it
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
        case 0x02024CC4:  // sets the receive callback of the data port (r0, 0 = none)
            wr32(kWhState + 0x14, c.r[0]);
            finish(c, 0);
            return true;
        case 0x02024410: {  // sends r1 bytes at r0 through the port; r2 is called once it is out
            std::vector<u8> data(c.r[1]);
            for (u32 i = 0; i < c.r[1]; ++i) data[i] = rd8(c.r[0] + i);
            if (g_settings.role == NetRole::Host) {
                for (auto& [aid, guest] : g_guests) {
                    if (guest.connected) guest_send(guest, kBlock, data);
                }
            } else {
                send_message(g_host, kBlock, data);
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
                guest_read(guest, type, body, -1);
                if (type == kRecord) guest.records.push_back(body);
                else if (type == kBlock) g_blocks.push_back({aid, body});
            }
            std::copy(guest.records.front().begin(), guest.records.front().end(), records.begin() + aid * kRecordSize);
            guest.records.pop_front();
        }
        const u64 step = g_stats.steps + 1;
        std::vector<u8> packet(10 + records.size());
        std::memcpy(packet.data(), &bitmap, 2);
        std::memcpy(packet.data() + 2, &step, 8);
        std::copy(records.begin(), records.end(), packet.begin() + 10);
        for (auto& [aid, guest] : g_guests) {
            if (guest.connected) guest_send(guest, kStep, packet);
        }
        store_step(records, bitmap, step);
    } else {
        send_message(g_host, kRecord, own);
        for (;;) {
            u8 type;
            std::vector<u8> body;
            read_message(g_host, g_host_inbox, type, body, -1);
            if (type == kBlock) {
                g_blocks.push_back({0, body});
                continue;
            }
            if (type == kStep) {
                u16 bitmap;
                std::memcpy(&bitmap, body.data(), 2);
                u64 step;
                std::memcpy(&step, body.data() + 2, 8);
                store_step(std::vector<u8>(body.begin() + 10, body.end()), bitmap, step);
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
    if (settings.relay != 0) {
        // Through the server: the lobby's connection, already open (lobby.h).
        const SOCKET relay = static_cast<SOCKET>(settings.relay);
        if (settings.role == NetRole::Host) {
            g_relay = relay;
            g_relay_inbox = settings.relay_inbox;
            std::printf("net: host through the server\n");
        } else {
            g_host = relay;
            g_host_inbox = settings.relay_inbox;
        }
    } else if (settings.role == NetRole::Host) {
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
    }
    if (settings.role == NetRole::Guest) {
        send_message(g_host, kHello, std::vector<u8>(settings.name.begin(), settings.name.end()));
        u8 type;
        std::vector<u8> body;
        read_message(g_host, g_host_inbox, type, body, -1);
        if (type != kWelcome) runtime_abort("network: no welcome", type);
        g_own_aid = body[0];
        wr16(kWhState, body[0]);
        write_download_boot(body[1]);
        std::printf("net: guest, player %u, language %u\n", body[0], body[1]);
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

// Hands arrived port data to the game, as WM's port callback would.
void deliver_blocks() {
    const std::vector<u32> done = std::move(g_send_done);
    g_send_done.clear();
    for (u32 callback : done) call_game(callback, 1, 0, 0);
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

void net_tick() {
    if (!g_started) return;
    if (g_pick_frames > 0) --g_pick_frames;
    if (g_pick_countdown > 0 && --g_pick_countdown == 0) {
        g_pick_countdown = -1;
        g_pick_frames = kPickFrames;
        std::printf("net: picks %s at step %llu\n", lobby::character_name(g_settings.character),
                    static_cast<unsigned long long>(g_stats.steps));
    }
    deliver_blocks();
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
                guest_send(guest, kJoined, body);
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
            wr16(kWhState, u16(g_own_aid));
            set_wh_state(kDataSharing);
            std::printf("net: taken in, players %#x\n", bitmap);
        }
    }
}

const NetStats& net_stats() { return g_stats; }

void net_set_wait_callback(NetWaitCallback callback) { g_wait_callback = callback; }
