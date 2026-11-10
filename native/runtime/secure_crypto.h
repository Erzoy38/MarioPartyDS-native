// The cryptographic building blocks under secure.cpp, one implementation per platform:
// secure_cng.cpp (Windows' own CNG) and secure_mbedtls.cpp (Mbed TLS, for Android and iOS).
// Both must give the same bytes on the wire, so a phone and a PC can play together.
#pragma once

#include "secure.h"

#include <cstdint>
#include <string>

namespace secure::crypto {

// False if the platform's cryptography cannot be used.
bool available();
const char* unavailable_text();

Bytes random_bytes(std::size_t n);
Bytes sha256(const Bytes& data);
Bytes hmac_sha256(const Bytes& key, const Bytes& data);

// --- ECDH P-256, ephemeral ---
// A new key pair; `public_xy` gets X || Y (64 bytes, each big-endian). Null on failure.
void* ecdh_create(Bytes& public_xy);
// The shared secret with the other side's X || Y, in the byte order of CNG's BCRYPT_KDF_RAW_SECRET
// (the X coordinate, least significant byte first). Empty on failure, also for a point not on the curve.
Bytes ecdh_agree(void* own, const std::uint8_t* other_xy);
void ecdh_free(void* key);

// --- ECDSA P-256 ---
// The server's identity in `path`: CNG's BCRYPT_ECCPRIVATE_BLOB (magic, length 32, X, Y, d).
void* identity_load_or_create(const std::string& path, Bytes& public_xy, std::string& error);
// r || s, 64 bytes, over a 32-byte hash.
Bytes identity_sign(void* key, const Bytes& hash);
void identity_free(void* key);
bool verify(const std::uint8_t* public_xy, const Bytes& hash, const std::uint8_t* signature);

// --- AES-256-GCM, 12-byte nonce, 16-byte tag, no extra data ---
void* aes_create(const Bytes& key);
void aes_seal(void* key, const std::uint8_t nonce[12], const std::uint8_t* in, std::size_t n, std::uint8_t* out,
              std::uint8_t tag[16]);
// False if the record or its tag was changed.
bool aes_open(void* key, const std::uint8_t nonce[12], const std::uint8_t* in, std::size_t n, const std::uint8_t tag[16],
              std::uint8_t* out);
void aes_free(void* key);

}  // namespace secure::crypto
