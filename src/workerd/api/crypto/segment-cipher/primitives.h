// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The cryptographic primitives the segment-cipher crate (lib.rs) calls. They are BoringSSL's, as
// all of workerd's cryptography is; the crate composes them into segments.

#include <rust/cxx.h>

#include <cstdint>

namespace kj {
class EntropySource;
}

namespace workerd::api::segment_cipher {

// The source of nonces and IVs: the IoContext's.
using EntropySource = kj::EntropySource;

// The HKDF hash. The crate's bridge mirrors this enum.
enum class Digest : uint8_t {
  Sha1,
  Sha256,
  Sha384,
  Sha512,
};

// The AEADs. AES-GCM's key size follows the key's length.
enum class Aead : uint8_t {
  AesGcm,
  ChaCha20Poly1305,
};

void fill_random(EntropySource& source, ::rust::Slice<uint8_t> out);

// HKDF-Expand, filling `out`.
void hkdf_expand(Digest digest,
    ::rust::Slice<const uint8_t> prk,
    ::rust::Slice<const uint8_t> info,
    ::rust::Slice<uint8_t> out);

// Seals `plaintext` into `out`, which holds the ciphertext followed by the 16-byte tag.
void aead_seal(Aead aead,
    ::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> nonce,
    ::rust::Slice<const uint8_t> plaintext,
    ::rust::Slice<uint8_t> out);

// Opens `sealed` (ciphertext and tag) into `out`, 16 bytes shorter. Returns false if it fails to
// authenticate.
bool aead_open(Aead aead,
    ::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> nonce,
    ::rust::Slice<const uint8_t> sealed,
    ::rust::Slice<uint8_t> out);

// AES-256-CBC with PKCS#7 padding. `out` holds the padded ciphertext.
void aes_256_cbc_encrypt(::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> iv,
    ::rust::Slice<const uint8_t> plaintext,
    ::rust::Slice<uint8_t> out);

// Decrypts into `out`, which is as long as `ciphertext`, and sets `written` to the unpadded size.
// Returns false if the padding is invalid.
bool aes_256_cbc_decrypt(::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> iv,
    ::rust::Slice<const uint8_t> ciphertext,
    ::rust::Slice<uint8_t> out,
    size_t& written);

// HMAC-SHA256 of `data`, into the 32 bytes of `out`.
void hmac_sha256(::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> data,
    ::rust::Slice<uint8_t> out);

// Compares in time that depends only on the length.
bool constant_time_equal(::rust::Slice<const uint8_t> a, ::rust::Slice<const uint8_t> b);

// Overwrites `bytes` with zeros, in a way the compiler can't remove.
void cleanse(::rust::Slice<uint8_t> bytes);

}  // namespace workerd::api::segment_cipher
