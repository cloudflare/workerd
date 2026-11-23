// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "primitives.h"

#include <openssl/aead.h>
#include <openssl/cipher.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>

#include <kj/common.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/memory.h>

#include <climits>

namespace workerd::api::segment_cipher {

namespace {

constexpr size_t AEAD_TAG_SIZE = 16;
constexpr size_t HMAC_SHA256_SIZE = 32;
constexpr size_t CBC_BLOCK_SIZE = 16;

const EVP_MD* digestFor(Digest digest) {
  switch (digest) {
    case Digest::Sha1:
      return EVP_sha1();
    case Digest::Sha256:
      return EVP_sha256();
    case Digest::Sha384:
      return EVP_sha384();
    case Digest::Sha512:
      return EVP_sha512();
  }
  KJ_UNREACHABLE;
}

const EVP_AEAD* aeadFor(Aead aead, size_t keySize) {
  switch (aead) {
    case Aead::AesGcm:
      KJ_REQUIRE(keySize == 16 || keySize == 32, "AES-GCM keys are 128 or 256 bits");
      return keySize == 16 ? EVP_aead_aes_128_gcm() : EVP_aead_aes_256_gcm();
    case Aead::ChaCha20Poly1305:
      return EVP_aead_chacha20_poly1305();
  }
  KJ_UNREACHABLE;
}

kj::Own<EVP_AEAD_CTX> newAeadContext(Aead aead, ::rust::Slice<const uint8_t> key) {
  auto ctx = EVP_AEAD_CTX_new(aeadFor(aead, key.size()), key.data(), key.size(), AEAD_TAG_SIZE);
  KJ_REQUIRE(ctx != nullptr, "EVP_AEAD_CTX_new() failed");
  return kj::disposeWith<EVP_AEAD_CTX_free>(ctx);
}

kj::Own<EVP_CIPHER_CTX> newCipherContext() {
  auto ctx = EVP_CIPHER_CTX_new();
  KJ_REQUIRE(ctx != nullptr, "EVP_CIPHER_CTX_new() failed");
  return kj::disposeWith<EVP_CIPHER_CTX_free>(ctx);
}

// Fails with the BoringSSL error that `call` left, if it didn't return 1.
void check(int result, kj::StringPtr call) {
  if (result != 1) {
    auto code = ERR_get_error();
    ERR_clear_error();
    KJ_FAIL_REQUIRE(call, "failed", ERR_reason_error_string(code));
  }
}

}  // namespace

void fill_random(EntropySource& source, ::rust::Slice<uint8_t> out) {
  source.generate(kj::arrayPtr(out.data(), out.size()));
}

void hkdf_expand(Digest digest,
    ::rust::Slice<const uint8_t> prk,
    ::rust::Slice<const uint8_t> info,
    ::rust::Slice<uint8_t> out) {
  check(HKDF_expand(out.data(), out.size(), digestFor(digest), prk.data(), prk.size(), info.data(),
            info.size()),
      "HKDF_expand()");
}

void aead_seal(Aead aead,
    ::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> nonce,
    ::rust::Slice<const uint8_t> plaintext,
    ::rust::Slice<uint8_t> out) {
  KJ_REQUIRE(out.size() == plaintext.size() + AEAD_TAG_SIZE);
  auto ctx = newAeadContext(aead, key);
  size_t written = 0;
  check(EVP_AEAD_CTX_seal(ctx.get(), out.data(), &written, out.size(), nonce.data(), nonce.size(),
            plaintext.data(), plaintext.size(), nullptr, 0),
      "EVP_AEAD_CTX_seal()");
  KJ_ASSERT(written == out.size());
}

bool aead_open(Aead aead,
    ::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> nonce,
    ::rust::Slice<const uint8_t> sealed,
    ::rust::Slice<uint8_t> out) {
  KJ_REQUIRE(sealed.size() >= AEAD_TAG_SIZE && out.size() == sealed.size() - AEAD_TAG_SIZE);
  auto ctx = newAeadContext(aead, key);
  size_t written = 0;
  if (EVP_AEAD_CTX_open(ctx.get(), out.data(), &written, out.size(), nonce.data(), nonce.size(),
          sealed.data(), sealed.size(), nullptr, 0) != 1) {
    ERR_clear_error();
    return false;
  }
  KJ_ASSERT(written == out.size());
  return true;
}

void aes_256_cbc_encrypt(::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> iv,
    ::rust::Slice<const uint8_t> plaintext,
    ::rust::Slice<uint8_t> out) {
  KJ_REQUIRE(key.size() == 32 && iv.size() == CBC_BLOCK_SIZE);
  // EVP takes an int length, and PKCS#7 always adds a block's padding at most.
  KJ_REQUIRE(plaintext.size() <= static_cast<size_t>(INT_MAX) - CBC_BLOCK_SIZE);
  KJ_REQUIRE(out.size() == (plaintext.size() / CBC_BLOCK_SIZE + 1) * CBC_BLOCK_SIZE,
      "the output must be the padded ciphertext's size");
  auto ctx = newCipherContext();
  check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv.data()),
      "EVP_EncryptInit_ex()");
  int updateLength = 0;
  check(EVP_EncryptUpdate(ctx.get(), out.data(), &updateLength, plaintext.data(),
            static_cast<int>(plaintext.size())),
      "EVP_EncryptUpdate()");
  int finalLength = 0;
  check(EVP_EncryptFinal_ex(ctx.get(), out.data() + updateLength, &finalLength),
      "EVP_EncryptFinal_ex()");
  KJ_ASSERT(static_cast<size_t>(updateLength + finalLength) == out.size());
}

bool aes_256_cbc_decrypt(::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> iv,
    ::rust::Slice<const uint8_t> ciphertext,
    ::rust::Slice<uint8_t> out,
    size_t& written) {
  KJ_REQUIRE(key.size() == 32 && iv.size() == CBC_BLOCK_SIZE);
  // EVP takes an int length, and writes at most the ciphertext's size.
  KJ_REQUIRE(ciphertext.size() <= static_cast<size_t>(INT_MAX));
  KJ_REQUIRE(ciphertext.size() % CBC_BLOCK_SIZE == 0 && out.size() == ciphertext.size(),
      "the ciphertext must be whole blocks, and the output as long");
  auto ctx = newCipherContext();
  check(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv.data()),
      "EVP_DecryptInit_ex()");
  int updateLength = 0;
  check(EVP_DecryptUpdate(ctx.get(), out.data(), &updateLength, ciphertext.data(),
            static_cast<int>(ciphertext.size())),
      "EVP_DecryptUpdate()");
  int finalLength = 0;
  if (EVP_DecryptFinal_ex(ctx.get(), out.data() + updateLength, &finalLength) != 1) {
    ERR_clear_error();
    return false;
  }
  written = static_cast<size_t>(updateLength + finalLength);
  return true;
}

void hmac_sha256(::rust::Slice<const uint8_t> key,
    ::rust::Slice<const uint8_t> data,
    ::rust::Slice<uint8_t> out) {
  KJ_REQUIRE(out.size() == HMAC_SHA256_SIZE);
  unsigned int length = 0;
  KJ_REQUIRE(HMAC(EVP_sha256(), key.data(), key.size(), data.data(), data.size(), out.data(),
                 &length) != nullptr,
      "HMAC() failed");
  KJ_ASSERT(length == HMAC_SHA256_SIZE);
}

bool constant_time_equal(::rust::Slice<const uint8_t> a, ::rust::Slice<const uint8_t> b) {
  return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

void cleanse(::rust::Slice<uint8_t> bytes) {
  OPENSSL_cleanse(bytes.data(), bytes.size());
}

}  // namespace workerd::api::segment_cipher
