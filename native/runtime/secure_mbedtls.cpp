// secure_crypto.h with Mbed TLS (Apache 2.0, native/third_party/mbedtls-*), for Android and iOS.
// Everything that reaches the wire or the key file is laid out exactly as CNG does it, so this side
// talks to a server and players on Windows.
#include "secure_crypto.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

namespace secure::crypto {

namespace {

// CNG's BCRYPT_ECCKEY_BLOB header for a P-256 ECDSA private key: "ECS2", key length 32.
constexpr std::uint32_t kPrivateMagic = 0x32534345;
constexpr std::size_t kBlobHeader = 8;

struct Random {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    std::mutex lock;
    bool ok = false;
    Random() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const unsigned char kPersonal[] = "MarioPartyDS secure";
        ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, kPersonal, sizeof kPersonal - 1) == 0;
    }
};

Random& rng() {
    static Random r;
    return r;
}

// For Mbed TLS's f_rng arguments.
int random_into(void*, unsigned char* out, std::size_t n) {
    Random& r = rng();
    std::lock_guard<std::mutex> hold(r.lock);
    return mbedtls_ctr_drbg_random(&r.drbg, out, n);
}

struct EcKey {
    mbedtls_ecp_group group;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    EcKey() {
        mbedtls_ecp_group_init(&group);
        mbedtls_mpi_init(&d);
        mbedtls_ecp_point_init(&q);
    }
    ~EcKey() {
        mbedtls_ecp_point_free(&q);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&group);
    }
    EcKey(const EcKey&) = delete;
    EcKey& operator=(const EcKey&) = delete;
};

bool load_p256(mbedtls_ecp_group& group) { return mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0; }

// X || Y of a point, each big-endian (the uncompressed form without its leading 0x04).
Bytes point_xy(const mbedtls_ecp_group& group, const mbedtls_ecp_point& p) {
    unsigned char buffer[1 + kPublicKey];
    std::size_t length = 0;
    if (mbedtls_ecp_point_write_binary(&group, &p, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, buffer, sizeof buffer) != 0
        || length != sizeof buffer) {
        return {};
    }
    return Bytes(buffer + 1, buffer + sizeof buffer);
}

// The point X || Y, checked to be on the curve.
bool read_point(const mbedtls_ecp_group& group, const std::uint8_t* xy, mbedtls_ecp_point& p) {
    unsigned char buffer[1 + kPublicKey];
    buffer[0] = 0x04;
    std::memcpy(buffer + 1, xy, kPublicKey);
    return mbedtls_ecp_point_read_binary(&group, &p, buffer, sizeof buffer) == 0 && mbedtls_ecp_check_pubkey(&group, &p) == 0;
}

void put_u32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

}  // namespace

bool available() { return rng().ok; }
const char* unavailable_text() { return "cryptography (Mbed TLS) not available"; }

Bytes random_bytes(std::size_t n) {
    Bytes out(n);
    random_into(nullptr, out.data(), n);
    return out;
}

Bytes sha256(const Bytes& data) {
    Bytes out(32);
    mbedtls_sha256(data.data(), data.size(), out.data(), 0);
    return out;
}

Bytes hmac_sha256(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key.data(), key.size(), data.data(), data.size(),
                    out.data());
    return out;
}

void* ecdh_create(Bytes& public_xy) {
    auto* key = new EcKey;
    if (!load_p256(key->group) || mbedtls_ecp_gen_keypair(&key->group, &key->d, &key->q, random_into, nullptr) != 0) {
        delete key;
        return nullptr;
    }
    public_xy = point_xy(key->group, key->q);
    if (public_xy.size() != kPublicKey) {
        delete key;
        return nullptr;
    }
    return key;
}

Bytes ecdh_agree(void* own, const std::uint8_t* other_xy) {
    auto* key = static_cast<EcKey*>(own);
    if (key == nullptr) return {};
    mbedtls_ecp_point peer;
    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi z;
    mbedtls_mpi_init(&z);
    Bytes out(32);
    const bool ok = read_point(key->group, other_xy, peer)
                    && mbedtls_ecdh_compute_shared(&key->group, &z, &peer, &key->d, random_into, nullptr) == 0
                    && mbedtls_mpi_write_binary(&z, out.data(), out.size()) == 0;
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&peer);
    if (!ok) return {};
    std::reverse(out.begin(), out.end());  // CNG's raw secret is little-endian
    return out;
}

void ecdh_free(void* key) { delete static_cast<EcKey*>(key); }

void* identity_load_or_create(const std::string& path, Bytes& public_xy, std::string& error) {
    auto* key = new EcKey;
    if (!load_p256(key->group)) {
        delete key;
        error = unavailable_text();
        return nullptr;
    }
    std::ifstream in(path, std::ios::binary);
    if (in) {
        const Bytes blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool ok = blob.size() == kBlobHeader + 96 && get_u32(blob.data()) == kPrivateMagic
                        && get_u32(blob.data() + 4) == 32 && read_point(key->group, blob.data() + kBlobHeader, key->q)
                        && mbedtls_mpi_read_binary(&key->d, blob.data() + kBlobHeader + 64, 32) == 0
                        && mbedtls_ecp_check_privkey(&key->group, &key->d) == 0;
        if (!ok) {
            delete key;
            error = "the key file " + path + " cannot be read";
            return nullptr;
        }
    } else {
        if (mbedtls_ecp_gen_keypair(&key->group, &key->d, &key->q, random_into, nullptr) != 0) {
            delete key;
            error = "no key could be made";
            return nullptr;
        }
        Bytes blob;
        put_u32(blob, kPrivateMagic);
        put_u32(blob, 32);
        const Bytes xy = point_xy(key->group, key->q);
        blob.insert(blob.end(), xy.begin(), xy.end());
        unsigned char d[32];
        mbedtls_mpi_write_binary(&key->d, d, sizeof d);
        blob.insert(blob.end(), d, d + sizeof d);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
        if (!out || xy.size() != kPublicKey) {
            delete key;
            error = "the key file " + path + " cannot be written";
            return nullptr;
        }
    }
    public_xy = point_xy(key->group, key->q);
    return key;
}

Bytes identity_sign(void* own, const Bytes& hash) {
    auto* key = static_cast<EcKey*>(own);
    Bytes signature(64);
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    if (mbedtls_ecdsa_sign(&key->group, &r, &s, &key->d, hash.data(), hash.size(), random_into, nullptr) == 0) {
        mbedtls_mpi_write_binary(&r, signature.data(), 32);
        mbedtls_mpi_write_binary(&s, signature.data() + 32, 32);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return signature;
}

void identity_free(void* key) { delete static_cast<EcKey*>(key); }

bool verify(const std::uint8_t* public_xy, const Bytes& hash, const std::uint8_t* signature) {
    EcKey key;
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    const bool ok = load_p256(key.group) && read_point(key.group, public_xy, key.q)
                    && mbedtls_mpi_read_binary(&r, signature, 32) == 0 && mbedtls_mpi_read_binary(&s, signature + 32, 32) == 0
                    && mbedtls_ecdsa_verify(&key.group, hash.data(), hash.size(), &key.q, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return ok;
}

void* aes_create(const Bytes& secret) {
    auto* gcm = new mbedtls_gcm_context;
    mbedtls_gcm_init(gcm);
    if (mbedtls_gcm_setkey(gcm, MBEDTLS_CIPHER_ID_AES, secret.data(), unsigned(secret.size() * 8)) != 0) {
        mbedtls_gcm_free(gcm);
        delete gcm;
        return nullptr;
    }
    return gcm;
}

void aes_seal(void* key, const std::uint8_t nonce[12], const std::uint8_t* in, std::size_t n, std::uint8_t* out,
              std::uint8_t tag[16]) {
    mbedtls_gcm_crypt_and_tag(static_cast<mbedtls_gcm_context*>(key), MBEDTLS_GCM_ENCRYPT, n, nonce, 12, nullptr, 0, in, out,
                              16, tag);
}

bool aes_open(void* key, const std::uint8_t nonce[12], const std::uint8_t* in, std::size_t n, const std::uint8_t tag[16],
              std::uint8_t* out) {
    return key != nullptr
           && mbedtls_gcm_auth_decrypt(static_cast<mbedtls_gcm_context*>(key), n, nonce, 12, nullptr, 0, tag, 16, in, out) == 0;
}

void aes_free(void* key) {
    if (key == nullptr) return;
    auto* gcm = static_cast<mbedtls_gcm_context*>(key);
    mbedtls_gcm_free(gcm);
    delete gcm;
}

}  // namespace secure::crypto
