// The handshake, the records and the known server keys. The cryptography itself is in
// secure_crypto.h: Windows' own (secure_cng.cpp) or Mbed TLS (secure_mbedtls.cpp).
#include "secure.h"

#include "secure_crypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace secure {

namespace {

constexpr std::uint8_t kVersion = 1;
constexpr char kMagic[4] = {'M', 'P', 'D', 'S'};

using crypto::sha256;

void nonce_for(std::uint64_t counter, std::uint8_t nonce[12]) {
    std::memset(nonce, 0, 12);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = std::uint8_t(counter >> (8 * i));
}

Bytes text_bytes(const char* text) { return Bytes(text, text + std::strlen(text)); }

std::string hex(const Bytes& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : data) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}  // namespace

// --- Identity ---

Identity::~Identity() { crypto::identity_free(key_); }

bool Identity::load_or_create(const std::string& path, std::string& error) {
    if (!crypto::available()) {
        error = crypto::unavailable_text();
        return false;
    }
    key_ = crypto::identity_load_or_create(path, public_key_, error);
    return key_ != nullptr && public_key_.size() == kPublicKey;
}

Bytes Identity::sign(const Bytes& hash) const { return crypto::identity_sign(key_, hash); }

std::string fingerprint(const Bytes& public_key) {
    const Bytes hash = sha256(public_key);
    std::string text = hex(Bytes(hash.begin(), hash.begin() + 16));
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        if (!out.empty()) out += ' ';
        out += text.substr(i, 4);
    }
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// --- Channel ---

Channel::Channel(Role role, const Identity* identity) : role_(role), identity_(identity) {
    Bytes pub;
    void* key = crypto::available() ? crypto::ecdh_create(pub) : nullptr;
    if (key == nullptr) {
        fail(crypto::unavailable_text());
        return;
    }
    own_key_ = key;
    own_hello_.assign(kMagic, kMagic + 4);
    own_hello_.push_back(kVersion);
    own_hello_.insert(own_hello_.end(), pub.begin(), pub.end());
    const Bytes nonce = crypto::random_bytes(32);
    own_hello_.insert(own_hello_.end(), nonce.begin(), nonce.end());
}

Channel::~Channel() {
    crypto::ecdh_free(own_key_);
    crypto::aes_free(send_key_);
    crypto::aes_free(receive_key_);
}

bool Channel::fail(const std::string& text) {
    failed_ = true;
    if (error_.empty()) error_ = text;
    return false;
}

Bytes Channel::start() { return role_ == Role::Client ? own_hello_ : Bytes{}; }

void Channel::derive(const Bytes& secret, const Bytes& transcript) {
    const Bytes hash = sha256(transcript);
    Bytes c2s = hash, s2c = hash;
    const Bytes a = text_bytes("client to server"), b = text_bytes("server to client");
    c2s.insert(c2s.end(), a.begin(), a.end());
    s2c.insert(s2c.end(), b.begin(), b.end());
    const Bytes up = crypto::hmac_sha256(secret, c2s), down = crypto::hmac_sha256(secret, s2c);
    send_key_ = crypto::aes_create(role_ == Role::Client ? up : down);
    receive_key_ = crypto::aes_create(role_ == Role::Client ? down : up);
}

bool Channel::accept_client(Bytes& wire) {
    const Bytes hello(in_.begin(), in_.begin() + kClientHello);
    in_.erase(in_.begin(), in_.begin() + kClientHello);
    if (std::memcmp(hello.data(), kMagic, 4) != 0 || hello[4] != kVersion) return fail("not an encrypted game connection");
    const Bytes secret = crypto::ecdh_agree(own_key_, hello.data() + 5);
    if (secret.empty()) return fail("bad key from the client");
    Bytes reply = own_hello_;
    const Bytes& identity = identity_->public_key();
    reply.insert(reply.end(), identity.begin(), identity.end());
    Bytes transcript = text_bytes("MPDS handshake");
    transcript.insert(transcript.end(), hello.begin(), hello.end());
    transcript.insert(transcript.end(), reply.begin(), reply.end());
    const Bytes signature = identity_->sign(sha256(transcript));
    reply.insert(reply.end(), signature.begin(), signature.end());
    wire.insert(wire.end(), reply.begin(), reply.end());
    derive(secret, transcript);
    ready_ = true;
    return true;
}

bool Channel::finish_client() {
    const Bytes reply(in_.begin(), in_.begin() + kServerHello);
    in_.erase(in_.begin(), in_.begin() + kServerHello);
    if (std::memcmp(reply.data(), kMagic, 4) != 0 || reply[4] != kVersion) return fail("the server does not encrypt");
    const std::uint8_t* identity = reply.data() + kClientHello;
    const std::uint8_t* signature = identity + kPublicKey;
    Bytes transcript = text_bytes("MPDS handshake");
    transcript.insert(transcript.end(), own_hello_.begin(), own_hello_.end());
    transcript.insert(transcript.end(), reply.begin(), reply.begin() + kClientHello + kPublicKey);
    if (!crypto::verify(identity, sha256(transcript), signature)) return fail("the server's signature is wrong");
    const Bytes secret = crypto::ecdh_agree(own_key_, reply.data() + 5);
    if (secret.empty()) return fail("bad key from the server");
    server_key_.assign(identity, identity + kPublicKey);
    derive(secret, transcript);
    ready_ = true;
    return true;
}

void Channel::seal(const std::uint8_t* data, std::size_t size, Bytes& wire) {
    for (std::size_t at = 0; at < size || size == 0; at += kMaxRecord) {
        const std::size_t n = std::min(kMaxRecord, size - at);
        std::uint8_t nonce[12], tag[16];
        nonce_for(send_counter_++, nonce);
        const std::uint32_t length = std::uint32_t(n + 16);
        const std::size_t base = wire.size();
        wire.resize(base + 4 + n + 16);
        std::memcpy(wire.data() + base, &length, 4);
        crypto::aes_seal(send_key_, nonce, data + at, n, wire.data() + base + 4, tag);
        std::memcpy(wire.data() + base + 4 + n, tag, 16);
        if (size == 0) break;
    }
}

void Channel::send(const std::uint8_t* data, std::size_t size, Bytes& wire) {
    if (failed_ || size == 0) return;
    if (!ready_) {
        waiting_.insert(waiting_.end(), data, data + size);
        return;
    }
    seal(data, size, wire);
}

bool Channel::receive(const std::uint8_t* data, std::size_t size, Bytes& plain, Bytes& wire) {
    if (failed_) return false;
    in_.insert(in_.end(), data, data + size);
    if (!ready_) {
        if (role_ == Role::Server) {
            if (in_.size() < kClientHello) return true;
            if (!accept_client(wire)) return false;
        } else {
            if (in_.size() < kServerHello) return true;
            if (!finish_client()) return false;
        }
        if (!waiting_.empty()) {
            seal(waiting_.data(), waiting_.size(), wire);
            waiting_.clear();
        }
    }
    std::size_t at = 0;
    while (in_.size() - at >= 4) {
        std::uint32_t length = 0;
        std::memcpy(&length, in_.data() + at, 4);
        if (length < 16 || length > kMaxRecord + 16) return fail("broken encrypted record");
        if (in_.size() - at < 4 + std::size_t(length)) break;
        const std::size_t n = length - 16;
        std::uint8_t nonce[12];
        nonce_for(receive_counter_++, nonce);
        const std::size_t base = plain.size();
        plain.resize(base + n);
        if (!crypto::aes_open(receive_key_, nonce, in_.data() + at + 4, n, in_.data() + at + 4 + n, plain.data() + base)) {
            plain.resize(base);
            return fail("an encrypted record was changed on the way");
        }
        at += 4 + length;
    }
    in_.erase(in_.begin(), in_.begin() + std::ptrdiff_t(at));
    return true;
}

// --- KnownServers ---

int KnownServers::check(const std::string& server, const Bytes& key) {
    const std::string wanted = hex(key);
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string name, value;
        if (!(fields >> name >> value) || name != server) continue;
        return value == wanted ? 1 : -1;
    }
    in.close();
    std::ofstream out(path_, std::ios::app);
    out << server << ' ' << wanted << '\n';
    return 0;
}

void KnownServers::forget(const std::string& server) {
    std::ifstream in(path_);
    std::vector<std::string> kept;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string name;
        if ((fields >> name) && name == server) continue;
        kept.push_back(line);
    }
    in.close();
    std::ofstream out(path_, std::ios::trunc);
    for (const std::string& k : kept) out << k << '\n';
}

}  // namespace secure
