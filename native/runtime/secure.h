// Encrypted connections between the game and MarioPartyDS-server, with Windows' own cryptography
// (CNG, bcrypt.dll): no extra library.
//
// Handshake, once per TCP connection, before any lobby message:
//   client -> server  "MPDS", u8 version, ECDH P-256 public key (64), random (32)
//   server -> client  "MPDS", u8 version, ECDH P-256 public key (64), random (32),
//                     server identity key, ECDSA P-256 public (64), signature over both hellos (64)
// Both derive two AES-256-GCM keys from the shared secret and the hash of both hellos (HMAC-SHA256),
// one per direction. Every record afterwards: u32 length, ciphertext, 16-byte tag; the nonce is a
// counter per direction, so a record changed, dropped, repeated or reordered breaks the connection.
//
// The server proves it holds its identity key; the client compares that key with the one it saw the
// last time (KnownServers), so somebody in between cannot pose as the server. The first connection
// to a server takes its key on trust, as SSH does; its fingerprint is shown to the player.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace secure {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kPublicKey = 64;  // X || Y of a P-256 point
constexpr std::size_t kClientHello = 4 + 1 + kPublicKey + 32;
constexpr std::size_t kServerHello = kClientHello + kPublicKey + 64;
constexpr std::size_t kMaxRecord = 1u << 16;  // plaintext bytes per record

// The server's long-lived ECDSA key, kept in a file next to the server.
class Identity {
public:
    Identity() = default;
    ~Identity();
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;
    // Reads the key from `path`, or makes a new one and writes it there. False on failure.
    bool load_or_create(const std::string& path, std::string& error);
    // No key any more (a server that stops and may start again in the same process).
    void reset();
    const Bytes& public_key() const { return public_key_; }
    Bytes sign(const Bytes& hash) const;

private:
    void* key_ = nullptr;
    Bytes public_key_;
};

// "1A2B 3C4D ..." over the first 16 bytes of SHA-256 of a public key: short enough to compare.
std::string fingerprint(const Bytes& public_key);

class Channel {
public:
    enum class Role { Client, Server };
    // A server channel needs the identity; it must outlive the channel.
    Channel(Role role, const Identity* identity = nullptr);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // Client: the hello to send first.
    Bytes start();
    // Bytes from the socket. Appends what they decrypt to to `plain`, and anything the handshake
    // must send to `wire`. False on any error; the connection must then be closed (`error` says why).
    bool receive(const std::uint8_t* data, std::size_t size, Bytes& plain, Bytes& wire);
    // Encrypts `size` bytes into `wire`; before the handshake is done they wait and go out with it.
    void send(const std::uint8_t* data, std::size_t size, Bytes& wire);

    bool ready() const { return ready_; }
    // Client, once ready: the identity key the server proved it holds.
    const Bytes& server_key() const { return server_key_; }
    const std::string& error() const { return error_; }

private:
    bool fail(const std::string& text);
    bool finish_client();
    bool accept_client(Bytes& wire);
    void derive(const Bytes& secret, const Bytes& transcript);
    void seal(const std::uint8_t* data, std::size_t size, Bytes& wire);

    Role role_;
    const Identity* identity_;
    void* own_key_ = nullptr;  // ephemeral ECDH
    Bytes own_hello_;
    Bytes in_;  // raw bytes not yet used
    Bytes waiting_;  // plaintext sent before the handshake was done
    void* send_key_ = nullptr;
    void* receive_key_ = nullptr;
    std::uint64_t send_counter_ = 0;
    std::uint64_t receive_counter_ = 0;
    bool ready_ = false;
    bool failed_ = false;
    Bytes server_key_;
    std::string error_;
};

// Server keys seen before, by "address:port", in a text file (one "address:port hex-key" per line).
class KnownServers {
public:
    explicit KnownServers(std::string path) : path_(std::move(path)) {}
    // 1: known and the same, 0: new (now remembered), -1: known with a different key.
    int check(const std::string& server, const Bytes& key);
    // Forgets the key of `server`, so the next connection takes the new one.
    void forget(const std::string& server);

private:
    std::string path_;
};

}  // namespace secure
