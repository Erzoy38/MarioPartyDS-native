// The lobby protocol between MarioPartyDS(.exe / -headless) and MarioPartyDS-server.
//
// One TCP connection per player, to the server only. Messages are framed like the game's network
// messages in net.cpp: u32 length (type byte + body), u8 type, body. Numbers are little endian,
// strings are u8 length + bytes.
//
// Before the start the connection carries the lobby. When the host starts, the server sends
// kStarted to everybody and the same connection turns into a relay for the game:
//   guest -> server: the game's own frames (net.cpp), passed to the host as kRelayFrom {slot, type, body}
//   host -> server:  kRelayTo {slot, type, body}, passed to that guest as the plain frame {type, body}
// So only the server needs a port that the others can reach.
//
// A connection that breaks during the game can come back with kResume and the token from kStarted;
// the room waits for it. The host's kLeave in the relay ends the room. A restarted server knows no
// rooms: the host's kResume builds the room again, guests that come first are told to try again.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lobby {

constexpr std::uint16_t kVersion = 2;
constexpr std::uint16_t kDefaultPort = 47000;
constexpr int kCodeLength = 6;
// Without I, O, 0 and 1, which are easy to confuse.
constexpr char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int kMaxPlayers = 4;
constexpr int kMaxName = 10;  // the game's own name field holds 10 characters
constexpr std::uint32_t kMaxFrame = 1u << 20;

// The game's eight characters, in the order of its character screen (left column, then right).
constexpr int kCharacters = 8;
inline const char* character_name(int index) {
    static const char* const kNames[kCharacters] = {"Mario", "Luigi", "Wario", "Yoshi",
                                                    "Peach", "Daisy", "Waluigi", "Toad"};
    return index >= 0 && index < kCharacters ? kNames[index] : "?";
}

enum Type : std::uint8_t {
    // client -> server
    kHello = 1,   // u16 version, str name
    kCreate = 2,  // u8 character
    kJoin = 3,    // 6 bytes code, u8 character
    kPick = 4,    // u8 character
    kReady = 5,   // u8 ready
    kStart = 6,   // host only
    kLeave = 7,   // back to the menu, the connection stays
    kPing = 8,    // u64 client time, u16 last round trip in ms; answered in the relay too
    kResume = 9,  // back into a started room: 6 bytes code, u8 slot, u64 token; the host adds
                  // u8 player count and every slot's token, so a restarted server can rebuild the room
    // server -> client
    kRoom = 20,     // 6 bytes code, u8 own slot, then 4 x {u8 present, str name, u8 character, u8 ready, u16 ping}
    kError = 21,    // u8 error code
    kStarted = 22,  // u8 own slot, u8 player count, 4 x u8 character, u8 input delay in pictures,
                    // u64 own token (for kResume); the host also gets every slot's token (player count x u64)
    kPong = 23,     // u64 client time, as sent
    kLeft = 24,     // the room is gone for this client (left, closed or expired); u8 reason (an Error)
    kResumed = 25,  // kResume accepted: u8 slot; the connection carries the game again
    // relay, after kStarted
    kRelayFrom = 30,  // to the host: u8 slot, u8 type, body
    kRelayTo = 31,    // from the host: u8 slot, u8 type, body
    kRelayGone = 32,  // to the others: u8 slot whose connection ended (it may come back with kResume)
    kRelayBack = 33,  // to the others: u8 slot that is back
};

enum Error : std::uint8_t {
    kNoError = 0,
    kBadVersion = 1,
    kBadName = 2,
    kUnknownCode = 3,
    kRoomFull = 4,
    kAlreadyStarted = 5,
    kCharacterTaken = 6,
    kNotHost = 7,
    kNotEnoughPlayers = 8,
    kNotReady = 9,
    kExpired = 10,
    kHostLeft = 11,
    kBadMessage = 12,
    kNotInRoom = 13,
    kLeftRoom = 14,
    kTryAgain = 15,
    kBadToken = 16,
};

inline const char* error_text(int error) {
    switch (error) {
        case kBadVersion: return "Server und Spiel haben verschiedene Versionen.";
        case kBadName: return "Der Name fehlt oder ist zu lang.";
        case kUnknownCode: return "Diesen Code gibt es nicht.";
        case kRoomFull: return "Der Raum ist voll (4 Spieler).";
        case kAlreadyStarted: return "Das Spiel in diesem Raum hat schon begonnen.";
        case kCharacterTaken: return "Diese Figur hat schon jemand.";
        case kNotHost: return "Nur der Host kann starten.";
        case kNotEnoughPlayers: return "Zum Starten braucht es mindestens 2 Spieler.";
        case kNotReady: return "Noch nicht alle sind bereit.";
        case kExpired: return "Dieser Code ist abgelaufen.";
        case kHostLeft: return "Der Host hat den Raum verlassen.";
        case kBadMessage: return "Ungültige Nachricht.";
        case kNotInRoom: return "Nicht in einem Raum.";
        case kLeftRoom: return "Raum verlassen.";
        case kTryAgain: return "Der Raum wartet noch auf den Host.";
        case kBadToken: return "Dieser Platz gehört jemand anderem.";
        default: return "Unbekannter Fehler.";
    }
}

inline bool valid_code_char(char c) {
    return c != 0 && std::strchr(kCodeAlphabet, c) != nullptr;
}

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint32_t v) { bytes.push_back(std::uint8_t(v)); }
    void u16(std::uint32_t v) { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) { u16(v); u16(v >> 16); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(std::uint32_t(v >> (8 * i))); }
    void str(const std::string& s) {
        const std::size_t n = s.size() < 255 ? s.size() : 255;
        u8(std::uint32_t(n));
        bytes.insert(bytes.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
    }
    void raw(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + n);
    }
};

// Reads with bounds checks; after a short read `ok` is false and every value is 0.
struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at = 0;
    bool ok = true;
    explicit Reader(const std::vector<std::uint8_t>& b) : bytes(b) {}
    bool need(std::size_t n) {
        if (!ok || at + n > bytes.size()) ok = false;
        return ok;
    }
    std::uint32_t u8() { return need(1) ? bytes[at++] : 0; }
    std::uint32_t u16() { const std::uint32_t lo = u8(); return lo | (u8() << 8); }
    std::uint32_t u32() { const std::uint32_t lo = u16(); return lo | (u16() << 16); }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= std::uint64_t(u8()) << (8 * i);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u8();
        if (!need(n)) return {};
        std::string s(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + n));
        at += n;
        return s;
    }
    std::string fixed(std::size_t n) {
        if (!need(n)) return {};
        std::string s(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + n));
        at += n;
        return s;
    }
    bool done() const { return ok && at == bytes.size(); }
};

// Frames on a byte stream: appends to `out`.
inline void frame(std::vector<std::uint8_t>& out, std::uint8_t type, const std::vector<std::uint8_t>& body) {
    const std::uint32_t length = std::uint32_t(body.size() + 1);
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(length >> (8 * i)));
    out.push_back(type);
    out.insert(out.end(), body.begin(), body.end());
}

// Takes one complete frame from the front of `inbox`. Returns false if none is complete yet;
// sets `bad` if the length is impossible.
inline bool unframe(std::vector<std::uint8_t>& inbox, std::uint8_t& type, std::vector<std::uint8_t>& body, bool& bad) {
    bad = false;
    if (inbox.size() < 4) return false;
    std::uint32_t length = 0;
    for (int i = 0; i < 4; ++i) length |= std::uint32_t(inbox[std::size_t(i)]) << (8 * i);
    if (length == 0 || length > kMaxFrame) {
        bad = true;
        return false;
    }
    if (inbox.size() < 4 + std::size_t(length)) return false;
    type = inbox[4];
    body.assign(inbox.begin() + 5, inbox.begin() + 4 + std::ptrdiff_t(length));
    inbox.erase(inbox.begin(), inbox.begin() + 4 + std::ptrdiff_t(length));
    return true;
}

}  // namespace lobby
