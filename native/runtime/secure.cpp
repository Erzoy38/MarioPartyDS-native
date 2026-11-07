#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "secure.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace secure {

namespace {

constexpr std::uint8_t kVersion = 1;
constexpr char kMagic[4] = {'M', 'P', 'D', 'S'};
constexpr NTSTATUS kOk = 0;

struct Algorithms {
    BCRYPT_ALG_HANDLE ecdh = nullptr, ecdsa = nullptr, sha = nullptr, hmac = nullptr, aes = nullptr;
    bool ok = false;
    Algorithms() {
        ok = BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) == kOk
             && BCryptOpenAlgorithmProvider(&ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == kOk
             && BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == kOk
             && BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) == kOk
             && BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) == kOk
             && BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                  sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == kOk;
    }
};

const Algorithms& algorithms() {
    static const Algorithms a;
    return a;
}

Bytes random_bytes(std::size_t n) {
    Bytes out(n);
    BCryptGenRandom(nullptr, out.data(), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return out;
}

Bytes sha256(const Bytes& data) {
    Bytes out(32);
    BCryptHash(algorithms().sha, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), out.data(), 32);
    return out;
}

Bytes hmac(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    BCryptHash(algorithms().hmac, const_cast<PUCHAR>(key.data()), ULONG(key.size()), const_cast<PUCHAR>(data.data()),
               ULONG(data.size()), out.data(), 32);
    return out;
}

// A P-256 public key as X || Y, from and to CNG's blob.
Bytes export_public(BCRYPT_KEY_HANDLE key) {
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + kPublicKey);
    ULONG got = 0;
    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), ULONG(blob.size()), &got, 0) != kOk) return {};
    return Bytes(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.end());
}

BCRYPT_KEY_HANDLE import_public(BCRYPT_ALG_HANDLE algorithm, ULONG magic, const std::uint8_t* xy) {
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + kPublicKey);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = magic;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), xy, kPublicKey);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), ULONG(blob.size()), 0) != kOk) {
        return nullptr;
    }
    return key;
}

BCRYPT_KEY_HANDLE aes_key(const Bytes& secret) {
    BCRYPT_KEY_HANDLE key = nullptr;
    BCryptGenerateSymmetricKey(algorithms().aes, &key, nullptr, 0, const_cast<PUCHAR>(secret.data()), ULONG(secret.size()), 0);
    return key;
}

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

Identity::~Identity() {
    if (key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool Identity::load_or_create(const std::string& path, std::string& error) {
    if (!algorithms().ok) {
        error = "cryptography of Windows not available";
        return false;
    }
    BCRYPT_KEY_HANDLE key = nullptr;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        Bytes blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (BCryptImportKeyPair(algorithms().ecdsa, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, blob.data(), ULONG(blob.size()),
                                0) != kOk) {
            error = "the key file " + path + " cannot be read";
            return false;
        }
    } else {
        if (BCryptGenerateKeyPair(algorithms().ecdsa, &key, 256, 0) != kOk || BCryptFinalizeKeyPair(key, 0) != kOk) {
            error = "no key could be made";
            return false;
        }
        Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 96);
        ULONG got = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), ULONG(blob.size()), &got, 0) != kOk) {
            error = "the new key cannot be kept";
            return false;
        }
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(got));
        if (!out) {
            error = "the key file " + path + " cannot be written";
            return false;
        }
    }
    key_ = key;
    public_key_ = export_public(key);
    return public_key_.size() == kPublicKey;
}

Bytes Identity::sign(const Bytes& hash) const {
    Bytes signature(64);
    ULONG got = 0;
    BCryptSignHash(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr, const_cast<PUCHAR>(hash.data()), ULONG(hash.size()),
                   signature.data(), ULONG(signature.size()), &got, 0);
    return signature;
}

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
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!algorithms().ok || BCryptGenerateKeyPair(algorithms().ecdh, &key, 256, 0) != kOk
        || BCryptFinalizeKeyPair(key, 0) != kOk) {
        fail("cryptography of Windows not available");
        return;
    }
    own_key_ = key;
    own_hello_.assign(kMagic, kMagic + 4);
    own_hello_.push_back(kVersion);
    const Bytes pub = export_public(key);
    own_hello_.insert(own_hello_.end(), pub.begin(), pub.end());
    const Bytes nonce = random_bytes(32);
    own_hello_.insert(own_hello_.end(), nonce.begin(), nonce.end());
}

Channel::~Channel() {
    if (own_key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(own_key_));
    if (send_key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(send_key_));
    if (receive_key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(receive_key_));
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
    const Bytes up = hmac(secret, c2s), down = hmac(secret, s2c);
    send_key_ = aes_key(role_ == Role::Client ? up : down);
    receive_key_ = aes_key(role_ == Role::Client ? down : up);
}

// Shared secret of our ephemeral key and the other side's public key.
static Bytes agree(void* own, const std::uint8_t* other_xy) {
    BCRYPT_KEY_HANDLE peer = import_public(algorithms().ecdh, BCRYPT_ECDH_PUBLIC_P256_MAGIC, other_xy);
    if (peer == nullptr) return {};
    BCRYPT_SECRET_HANDLE secret = nullptr;
    Bytes out(32);
    ULONG got = 0;
    const bool ok = BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(own), peer, &secret, 0) == kOk
                    && BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, out.data(), 32, &got, 0) == kOk;
    if (secret != nullptr) BCryptDestroySecret(secret);
    BCryptDestroyKey(peer);
    return ok ? out : Bytes{};
}

bool Channel::accept_client(Bytes& wire) {
    const Bytes hello(in_.begin(), in_.begin() + kClientHello);
    in_.erase(in_.begin(), in_.begin() + kClientHello);
    if (std::memcmp(hello.data(), kMagic, 4) != 0 || hello[4] != kVersion) return fail("not an encrypted game connection");
    const Bytes secret = agree(own_key_, hello.data() + 5);
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
    const Bytes hash = sha256(transcript);
    BCRYPT_KEY_HANDLE verify = import_public(algorithms().ecdsa, BCRYPT_ECDSA_PUBLIC_P256_MAGIC, identity);
    const bool signed_ok = verify != nullptr
                           && BCryptVerifySignature(verify, nullptr, const_cast<PUCHAR>(hash.data()), 32,
                                                    const_cast<PUCHAR>(signature), 64, 0) == kOk;
    if (verify != nullptr) BCryptDestroyKey(verify);
    if (!signed_ok) return fail("the server's signature is wrong");
    const Bytes secret = agree(own_key_, reply.data() + 5);
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
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce;
        info.cbNonce = 12;
        info.pbTag = tag;
        info.cbTag = 16;
        const std::uint32_t length = std::uint32_t(n + 16);
        const std::size_t base = wire.size();
        wire.resize(base + 4 + n + 16);
        std::memcpy(wire.data() + base, &length, 4);
        ULONG got = 0;
        BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(send_key_), const_cast<PUCHAR>(data + at), ULONG(n), &info, nullptr, 0,
                      wire.data() + base + 4, ULONG(n), &got, 0);
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
        Bytes tag(in_.begin() + std::ptrdiff_t(at + 4 + n), in_.begin() + std::ptrdiff_t(at + 4 + n + 16));
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce;
        info.cbNonce = 12;
        info.pbTag = tag.data();
        info.cbTag = 16;
        const std::size_t base = plain.size();
        plain.resize(base + n);
        ULONG got = 0;
        const NTSTATUS status = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(receive_key_), in_.data() + at + 4, ULONG(n), &info,
                                              nullptr, 0, plain.data() + base, ULONG(n), &got, 0);
        if (status != kOk) {
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
