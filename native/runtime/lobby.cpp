#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "lobby.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace lobby;

namespace {

constexpr double kConnectTimeout = 5.0;

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

SOCKET as_socket(std::uintptr_t s) { return static_cast<SOCKET>(s); }

bool winsock_ready() {
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

}  // namespace

LobbyClient::~LobbyClient() {
    if (phase_ != Phase::Started) disconnect();
}

void LobbyClient::fail(const std::string& text) {
    message_ = text;
    std::printf("lobby: %s\n", text.c_str());
    if (as_socket(socket_) != INVALID_SOCKET) closesocket(as_socket(socket_));
    socket_ = std::uintptr_t(INVALID_SOCKET);
    phase_ = Phase::Failed;
}

void LobbyClient::disconnect() {
    if (as_socket(socket_) != INVALID_SOCKET) closesocket(as_socket(socket_));
    socket_ = std::uintptr_t(INVALID_SOCKET);
    inbox_.clear();
    outbox_.clear();
    channel_.reset();
    key_checked_ = false;
    phase_ = Phase::Offline;
    code_.clear();
    own_slot_ = -1;
}

void LobbyClient::connect(const std::string& address, std::uint16_t port, const std::string& name) {
    disconnect();
    message_.clear();
    last_error_ = 0;
    name_ = name;
    address_ = address;
    port_ = port;
    if (!winsock_ready()) return fail("Netzwerk nicht verfügbar.");
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        return fail("Server-Adresse nicht gefunden: " + address);
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    BOOL yes = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
    const int result = ::connect(s, found->ai_addr, int(found->ai_addrlen));
    freeaddrinfo(found);
    if (result != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return fail("Keine Verbindung zum Server.");
    }
    socket_ = std::uintptr_t(s);
    phase_ = Phase::Connecting;
    connect_started_ = now_seconds();
    // Every connection to the server is encrypted; the lobby's messages wait for the handshake.
    channel_ = std::make_shared<secure::Channel>(secure::Channel::Role::Client);
    outbox_ = channel_->start();
    fingerprint_.clear();
    server_is_new_ = false;
    Writer w;
    w.u16(kVersion);
    w.str(name);
    send(kHello, w.bytes);
}

void LobbyClient::send(std::uint8_t type, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> plain;
    frame(plain, type, body);
    if (channel_) channel_->send(plain.data(), plain.size(), outbox_);
}

void LobbyClient::create(int character) {
    message_.clear();
    last_error_ = 0;
    Writer w;
    w.u8(std::uint32_t(character));
    send(kCreate, w.bytes);
}

void LobbyClient::join(const std::string& code, int character) {
    message_.clear();
    last_error_ = 0;
    std::string fixed = code;
    fixed.resize(kCodeLength, ' ');
    Writer w;
    w.raw(fixed.data(), kCodeLength);
    w.u8(std::uint32_t(character));
    send(kJoin, w.bytes);
}

void LobbyClient::pick(int character) {
    message_.clear();
    Writer w;
    w.u8(std::uint32_t(character));
    send(kPick, w.bytes);
}

void LobbyClient::set_ready(bool ready) {
    Writer w;
    w.u8(ready ? 1 : 0);
    send(kReady, w.bytes);
}

void LobbyClient::start() {
    message_.clear();
    send(kStart, {});
}

void LobbyClient::leave() {
    send(kLeave, {});
}

void LobbyClient::handle(std::uint8_t type, const std::vector<std::uint8_t>& body) {
    Reader r(body);
    switch (type) {
        case kRoom: {
            const std::string code = r.fixed(kCodeLength);
            const int slot = int(r.u8());
            std::array<LobbySeat, kMaxPlayers> seats{};
            for (LobbySeat& seat : seats) {
                seat.present = r.u8() != 0;
                seat.name = r.str();
                const std::uint32_t character = r.u8();
                seat.character = character < std::uint32_t(kCharacters) ? int(character) : -1;
                seat.ready = r.u8() != 0;
                seat.ping_ms = int(r.u16());
            }
            if (!r.done()) return;
            code_ = code;
            own_slot_ = slot;
            seats_ = seats;
            phase_ = Phase::InRoom;
            ++room_updates_;
            return;
        }
        case kError: {
            last_error_ = int(r.u8());
            message_ = error_text(last_error_);
            std::printf("lobby: refused: %s\n", message_.c_str());
            return;
        }
        case kLeft: {
            last_error_ = int(r.u8());
            message_ = error_text(last_error_);
            std::printf("lobby: out of the room: %s\n", message_.c_str());
            code_.clear();
            own_slot_ = -1;
            seats_ = {};
            phase_ = Phase::Menu;
            return;
        }
        case kPong: {
            const std::uint64_t sent = r.u64();
            if (!r.done()) return;
            const std::uint64_t now = std::uint64_t(now_seconds() * 1000.0);
            // The smallest of the last five: the way itself, without one slow answer.
            recent_pings_[ping_count_++ % recent_pings_.size()] = int(std::min<std::uint64_t>(now - sent, 65535));
            ping_ms_ = 65535;
            for (std::size_t i = 0; i < std::min<std::size_t>(ping_count_, recent_pings_.size()); ++i) {
                ping_ms_ = std::min(ping_ms_, recent_pings_[i]);
            }
            return;
        }
        case kStarted: {
            start_ = {};
            start_.slot = int(r.u8());
            start_.players = int(r.u8());
            for (int& character : start_.characters) {
                const std::uint32_t c = r.u8();
                character = c < std::uint32_t(kCharacters) ? int(c) : -1;
            }
            start_.input_delay = int(r.u8());
            start_.token = r.u64();
            if (start_.slot == 0) {
                for (int i = 0; i < start_.players && i < kMaxPlayers; ++i) start_.tokens[size_t(i)] = r.u64();
            }
            if (!r.done()) return fail("Ungültige Startnachricht.");
            start_.code = code_;
            start_.address = address_;
            start_.port = port_;
            std::printf("lobby: started as player %d of %d\n", start_.slot + 1, start_.players);
            phase_ = Phase::Started;
            return;
        }
        default:
            return;
    }
}

void LobbyClient::poll() {
    if (phase_ == Phase::Offline || phase_ == Phase::Failed || phase_ == Phase::Started) return;
    SOCKET s = as_socket(socket_);
    const double now = now_seconds();
    if (phase_ == Phase::Connecting) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval zero{0, 0};
        select(0, nullptr, &writable, &failed, &zero);
        if (FD_ISSET(s, &failed)) return fail("Keine Verbindung zum Server.");
        if (!FD_ISSET(s, &writable)) {
            if (now - connect_started_ > kConnectTimeout) fail("Server antwortet nicht.");
            return;
        }
        phase_ = Phase::Menu;
        last_ping_ = 0;
    }
    if (now - last_ping_ >= 1.0 && channel_->ready()) {  // not before the handshake: it would count in
        last_ping_ = now;
        Writer w;
        w.u64(std::uint64_t(now * 1000.0));
        w.u16(std::uint32_t(ping_ms_));
        send(kPing, w.bytes);
    }
    while (!outbox_.empty()) {
        const int n = ::send(s, reinterpret_cast<const char*>(outbox_.data()), int(outbox_.size()), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            return fail("Verbindung zum Server verloren.");
        }
        outbox_.erase(outbox_.begin(), outbox_.begin() + n);
    }
    for (;;) {
        char buffer[8192];
        const int n = recv(s, buffer, sizeof buffer, 0);
        if (n == 0) return fail("Der Server hat die Verbindung beendet.");
        if (n < 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            return fail("Verbindung zum Server verloren.");
        }
        if (!channel_->receive(reinterpret_cast<const std::uint8_t*>(buffer), std::size_t(n), inbox_, outbox_)) {
            return fail("Verschlüsselung gestört: " + channel_->error());
        }
    }
    if (channel_->ready() && !key_checked_) {
        key_checked_ = true;
        fingerprint_ = secure::fingerprint(channel_->server_key());
        const std::string server = address_ + ":" + std::to_string(port_);
        if (!known_servers_.empty()) {
            const int known = secure::KnownServers(known_servers_).check(server, channel_->server_key());
            if (known < 0) {
                return fail("Der Server " + server + " hat einen anderen Schlüssel als beim letzten Mal ("
                            + fingerprint_ + "). Jemand könnte sich dazwischen schalten. Erst nach Rückfrage beim "
                            "Host den alten Schlüssel in den Einstellungen vergessen.");
            }
            server_is_new_ = known == 0;
        }
        std::printf("lobby: server key %s%s\n", fingerprint_.c_str(), server_is_new_ ? " (new)" : "");
    }
    while (!outbox_.empty()) {
        const int n = ::send(s, reinterpret_cast<const char*>(outbox_.data()), int(outbox_.size()), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            return fail("Verbindung zum Server verloren.");
        }
        outbox_.erase(outbox_.begin(), outbox_.begin() + n);
    }
    for (;;) {
        std::uint8_t type;
        std::vector<std::uint8_t> body;
        bool bad = false;
        if (!unframe(inbox_, type, body, bad)) {
            if (bad) fail("Ungültige Daten vom Server.");
            return;
        }
        handle(type, body);
        if (phase_ == Phase::Started || phase_ == Phase::Failed) return;
    }
}

LobbyStart LobbyClient::take_start() {
    LobbyStart start = start_;
    SOCKET s = as_socket(socket_);
    // What is still queued is encrypted already and counted; the game sends it first.
    start.outbox = std::move(outbox_);
    outbox_.clear();
    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
    start.socket = std::uintptr_t(s);
    start.channel = channel_;
    if (channel_) start.server_key = channel_->server_key();
    channel_.reset();
    start.inbox = std::move(inbox_);
    inbox_.clear();
    socket_ = std::uintptr_t(INVALID_SOCKET);
    return start;
}
