// secure_crypto.h with Windows' own cryptography (CNG, bcrypt.dll).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "secure_crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <fstream>

namespace secure::crypto {

namespace {

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

BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO gcm_info(const std::uint8_t nonce[12], std::uint8_t* tag) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = 12;
    info.pbTag = tag;
    info.cbTag = 16;
    return info;
}

}  // namespace

bool available() { return algorithms().ok; }
const char* unavailable_text() { return "cryptography of Windows not available"; }

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

Bytes hmac_sha256(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    BCryptHash(algorithms().hmac, const_cast<PUCHAR>(key.data()), ULONG(key.size()), const_cast<PUCHAR>(data.data()),
               ULONG(data.size()), out.data(), 32);
    return out;
}

void* ecdh_create(Bytes& public_xy) {
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!algorithms().ok || BCryptGenerateKeyPair(algorithms().ecdh, &key, 256, 0) != kOk
        || BCryptFinalizeKeyPair(key, 0) != kOk) {
        return nullptr;
    }
    public_xy = export_public(key);
    return key;
}

Bytes ecdh_agree(void* own, const std::uint8_t* other_xy) {
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

void ecdh_free(void* key) {
    if (key != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key));
}

void* identity_load_or_create(const std::string& path, Bytes& public_xy, std::string& error) {
    BCRYPT_KEY_HANDLE key = nullptr;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        Bytes blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (BCryptImportKeyPair(algorithms().ecdsa, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, blob.data(), ULONG(blob.size()),
                                0) != kOk) {
            error = "the key file " + path + " cannot be read";
            return nullptr;
        }
    } else {
        if (BCryptGenerateKeyPair(algorithms().ecdsa, &key, 256, 0) != kOk || BCryptFinalizeKeyPair(key, 0) != kOk) {
            error = "no key could be made";
            return nullptr;
        }
        Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 96);
        ULONG got = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), ULONG(blob.size()), &got, 0) != kOk) {
            error = "the new key cannot be kept";
            return nullptr;
        }
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(got));
        if (!out) {
            error = "the key file " + path + " cannot be written";
            return nullptr;
        }
    }
    public_xy = export_public(key);
    return key;
}

Bytes identity_sign(void* key, const Bytes& hash) {
    Bytes signature(64);
    ULONG got = 0;
    BCryptSignHash(static_cast<BCRYPT_KEY_HANDLE>(key), nullptr, const_cast<PUCHAR>(hash.data()), ULONG(hash.size()),
                   signature.data(), ULONG(signature.size()), &got, 0);
    return signature;
}

void identity_free(void* key) {
    if (key != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key));
}

bool verify(const std::uint8_t* public_xy, const Bytes& hash, const std::uint8_t* signature) {
    BCRYPT_KEY_HANDLE key = import_public(algorithms().ecdsa, BCRYPT_ECDSA_PUBLIC_P256_MAGIC, public_xy);
    const bool ok = key != nullptr
                    && BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(hash.data()), 32,
                                             const_cast<PUCHAR>(signature), 64, 0) == kOk;
    if (key != nullptr) BCryptDestroyKey(key);
    return ok;
}

void* aes_create(const Bytes& secret) {
    BCRYPT_KEY_HANDLE key = nullptr;
    BCryptGenerateSymmetricKey(algorithms().aes, &key, nullptr, 0, const_cast<PUCHAR>(secret.data()), ULONG(secret.size()), 0);
    return key;
}

void aes_seal(void* key, const std::uint8_t nonce[12], const std::uint8_t* in, std::size_t n, std::uint8_t* out,
              std::uint8_t tag[16]) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info = gcm_info(nonce, tag);
    ULONG got = 0;
    BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key), const_cast<PUCHAR>(in), ULONG(n), &info, nullptr, 0, out, ULONG(n),
                  &got, 0);
}

bool aes_open(void* key, const std::uint8_t nonce[12], const std::uint8_t* in, std::size_t n, const std::uint8_t tag[16],
              std::uint8_t* out) {
    std::uint8_t tag_copy[16];
    std::memcpy(tag_copy, tag, 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info = gcm_info(nonce, tag_copy);
    ULONG got = 0;
    return BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key), const_cast<PUCHAR>(in), ULONG(n), &info, nullptr, 0, out,
                         ULONG(n), &got, 0)
           == kOk;
}

void aes_free(void* key) {
    if (key != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key));
}

}  // namespace secure::crypto
