// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "segment-cipher.h"

#include <openssl/aead.h>
#include <openssl/cipher.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>

#include <kj/debug.h>
#include <kj/io.h>
#include <kj/vector.h>

#include <climits>

namespace workerd::api {

namespace {

constexpr size_t AEAD_NONCE_SIZE = 12;
constexpr size_t AEAD_TAG_SIZE = 16;
constexpr size_t CBC_BLOCK_SIZE = 16;
constexpr size_t HMAC_SHA256_SIZE = 32;

// BoringSSL's CBC calls take int lengths; a sealed segment's always fit.
static_assert(SegmentLayout::MAX_SEGMENT_SIZE + 2 * CBC_BLOCK_SIZE + HMAC_SHA256_SIZE <= INT_MAX);

}  // namespace

SegmentAlgorithm parseSegmentAlgorithm(kj::StringPtr name) {
  if (name == "AES-GCM"_kj) return SegmentAlgorithm::AES_GCM;
  if (name == "ChaCha20-Poly1305"_kj) return SegmentAlgorithm::CHACHA20_POLY1305;
  if (name == "AES-CBC-HMAC-SHA256"_kj) return SegmentAlgorithm::AES_CBC_HMAC_SHA256;
  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized segment cipher algorithm \"", name, "\".");
}

SegmentLayout::SegmentLayout(SegmentAlgorithm algorithm, uint32_t segmentSize)
    : algorithm(algorithm),
      segmentSize(segmentSize) {
  JSG_REQUIRE(segmentSize >= MIN_SEGMENT_SIZE && segmentSize <= MAX_SEGMENT_SIZE, RangeError,
      "segmentSize must be between ", MIN_SEGMENT_SIZE, " and ", MAX_SEGMENT_SIZE, " bytes (got ",
      segmentSize, ").");
}

size_t SegmentLayout::sealedSegmentSize(size_t plaintextSize) const {
  KJ_REQUIRE(plaintextSize <= segmentSize);
  if (plaintextSize == 0) return 0;
  switch (algorithm) {
    case SegmentAlgorithm::AES_GCM:
    case SegmentAlgorithm::CHACHA20_POLY1305:
      return AEAD_NONCE_SIZE + plaintextSize + AEAD_TAG_SIZE;
    case SegmentAlgorithm::AES_CBC_HMAC_SHA256:
      // PKCS#7 always pads, adding a whole block when the plaintext fills its last one.
      return CBC_BLOCK_SIZE + (plaintextSize / CBC_BLOCK_SIZE + 1) * CBC_BLOCK_SIZE +
          HMAC_SHA256_SIZE;
  }
  KJ_UNREACHABLE;
}

uint64_t SegmentLayout::segmentCount(uint64_t plaintextLength) const {
  return integerCeilDivision<uint64_t>(plaintextLength, segmentSize);
}

uint64_t SegmentLayout::sealedLength(uint64_t plaintextLength) const {
  uint64_t fullSegments = plaintextLength / segmentSize;
  uint64_t remainder = plaintextLength % segmentSize;
  return fullSegments * fullSealedSegmentSize() + sealedSegmentSize(remainder);
}

SegmentIndexEncoding parseSegmentIndexEncoding(kj::StringPtr name) {
  if (name == "decimal"_kj) return SegmentIndexEncoding::DECIMAL;
  if (name == "uint32be"_kj) return SegmentIndexEncoding::UINT32BE;
  if (name == "uint64be"_kj) return SegmentIndexEncoding::UINT64BE;
  JSG_FAIL_REQUIRE(TypeError, "Unrecognized segment index encoding \"", name,
      "\"; expected \"decimal\", \"uint32be\" or \"uint64be\".");
}

uint64_t SegmentInfoTemplate::maxIndex() const {
  switch (index) {
    case SegmentIndexEncoding::UINT32BE:
      return 0xffffffffull;
    case SegmentIndexEncoding::DECIMAL:
    case SegmentIndexEncoding::UINT64BE:
      return UINT64_MAX;
  }
  KJ_UNREACHABLE;
}

kj::Array<kj::byte> SegmentInfoTemplate::build(uint64_t segmentIndex) const {
  KJ_REQUIRE(segmentIndex <= maxIndex());
  // 20 bytes holds the longest encoding, a 64-bit index in decimal.
  kj::Vector<kj::byte> result(prefix.size() + 20 + suffix.size());
  result.addAll(prefix);
  switch (index) {
    case SegmentIndexEncoding::DECIMAL:
      result.addAll(kj::str(segmentIndex).asBytes());
      break;
    case SegmentIndexEncoding::UINT32BE:
      for (int shift = 24; shift >= 0; shift -= 8) {
        result.add(static_cast<kj::byte>(segmentIndex >> shift));
      }
      break;
    case SegmentIndexEncoding::UINT64BE:
      for (int shift = 56; shift >= 0; shift -= 8) {
        result.add(static_cast<kj::byte>(segmentIndex >> shift));
      }
      break;
  }
  result.addAll(suffix);
  return result.releaseAsArray();
}

SegmentCipher::SegmentCipher(SegmentLayout layout, KeySchedule keys, uint32_t keyBits)
    : layout(layout),
      digest(keys.digest),
      prk(kj::mv(keys.prk)),
      info(kj::mv(keys.info)),
      macInfo(kj::mv(keys.macInfo)),
      keyLength(keyBits / 8) {
  switch (layout.getAlgorithm()) {
    case SegmentAlgorithm::AES_GCM:
      JSG_REQUIRE(keyBits == 128 || keyBits == 256, DOMOperationError,
          "AES-GCM segment keys must be 128 or 256 bits long (requested ", keyBits, ").");
      JSG_REQUIRE(macInfo == kj::none, TypeError, "macInfo is only used by AES-CBC-HMAC-SHA256.");
      break;
    case SegmentAlgorithm::CHACHA20_POLY1305:
      JSG_REQUIRE(keyBits == 256, DOMOperationError,
          "ChaCha20-Poly1305 segment keys are 256 bits long (requested ", keyBits, ").");
      JSG_REQUIRE(macInfo == kj::none, TypeError, "macInfo is only used by AES-CBC-HMAC-SHA256.");
      break;
    case SegmentAlgorithm::AES_CBC_HMAC_SHA256:
      JSG_REQUIRE(keyBits == 256, DOMOperationError,
          "AES-CBC-HMAC-SHA256 segment keys are 256 bits long (requested ", keyBits, ").");
      JSG_REQUIRE(macInfo != kj::none, TypeError, "AES-CBC-HMAC-SHA256 requires macInfo.");
      break;
  }
}

uint64_t SegmentCipher::maxIndex() const {
  uint64_t result = info.maxIndex();
  KJ_IF_SOME(m, macInfo) {
    result = kj::min(result, m.maxIndex());
  }
  return result;
}

// The key depends only on the key schedule and the index, so segments are bound to their
// message only as far as the schedule is unique to it; see the class comment.
ZeroOnFree SegmentCipher::deriveKey(
    const SegmentInfoTemplate& infoTemplate, uint64_t segmentIndex) const {
  auto segmentInfo = infoTemplate.build(segmentIndex);
  auto key = kj::heapArray<kj::byte>(keyLength);
  OSSLCALL(HKDF_expand(key.begin(), key.size(), digest, prk.begin(), prk.size(),
      segmentInfo.begin(), segmentInfo.size()));
  return ZeroOnFree(kj::mv(key));
}

namespace {

const EVP_AEAD* aeadFor(SegmentAlgorithm algorithm, size_t keyLength) {
  switch (algorithm) {
    case SegmentAlgorithm::AES_GCM:
      return keyLength == 16 ? EVP_aead_aes_128_gcm() : EVP_aead_aes_256_gcm();
    case SegmentAlgorithm::CHACHA20_POLY1305:
      return EVP_aead_chacha20_poly1305();
    case SegmentAlgorithm::AES_CBC_HMAC_SHA256:
      break;
  }
  KJ_UNREACHABLE;
}

kj::Own<EVP_AEAD_CTX> newAeadContext(const EVP_AEAD* aead, const ZeroOnFree& key) {
  auto ctx = EVP_AEAD_CTX_new(aead, key.begin(), key.size(), AEAD_TAG_SIZE);
  JSG_REQUIRE(ctx != nullptr, DOMOperationError, "Failed to set up a segment key.");
  return kj::disposeWith<EVP_AEAD_CTX_free>(ctx);
}

kj::FixedArray<kj::byte, HMAC_SHA256_SIZE> hmacSha256(
    const ZeroOnFree& key, kj::ArrayPtr<const kj::byte> data) {
  kj::FixedArray<kj::byte, HMAC_SHA256_SIZE> mac;
  unsigned int macLength = 0;
  JSG_REQUIRE(HMAC(EVP_sha256(), key.begin(), key.size(), data.begin(), data.size(), mac.begin(),
                  &macLength) != nullptr,
      DOMOperationError, "HMAC failed.");
  KJ_ASSERT(macLength == HMAC_SHA256_SIZE);
  return mac;
}

[[noreturn]] void failAuthentication(uint64_t segmentIndex) {
  JSG_FAIL_REQUIRE(DOMOperationError, "Segment ", segmentIndex, " failed authentication.");
}

}  // namespace

size_t SegmentCipher::retainedBytes() const {
  size_t result = prk.size() + info.retainedBytes();
  KJ_IF_SOME(mac, macInfo) {
    result += mac.retainedBytes();
  }
  return result;
}

kj::Array<kj::byte> SegmentCipher::seal(uint64_t segmentIndex,
    kj::ArrayPtr<const kj::byte> plaintext,
    kj::EntropySource& entropy) const {
  KJ_REQUIRE(plaintext.size() > 0 && plaintext.size() <= layout.getSegmentSize());
  auto result = kj::heapArray<kj::byte>(layout.sealedSegmentSize(plaintext.size()));
  auto key = deriveKey(info, segmentIndex);

  switch (layout.getAlgorithm()) {
    case SegmentAlgorithm::AES_GCM:
    case SegmentAlgorithm::CHACHA20_POLY1305: {
      auto nonce = result.first(AEAD_NONCE_SIZE);
      entropy.generate(nonce);
      auto ctx = newAeadContext(aeadFor(layout.getAlgorithm(), keyLength), key);
      auto out = result.slice(AEAD_NONCE_SIZE);
      size_t written = 0;
      OSSLCALL(EVP_AEAD_CTX_seal(ctx.get(), out.begin(), &written, out.size(), nonce.begin(),
          nonce.size(), plaintext.begin(), plaintext.size(), nullptr, 0));
      KJ_ASSERT(written == out.size());
      break;
    }
    case SegmentAlgorithm::AES_CBC_HMAC_SHA256: {
      auto iv = result.first(CBC_BLOCK_SIZE);
      entropy.generate(iv);
      auto signedLength = result.size() - HMAC_SHA256_SIZE;
      auto out = result.slice(CBC_BLOCK_SIZE, signedLength);

      auto ctx = kj::disposeWith<EVP_CIPHER_CTX_free>(EVP_CIPHER_CTX_new());
      KJ_ASSERT(ctx.get() != nullptr);
      OSSLCALL(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.begin(), iv.begin()));
      int updateLength = 0;
      OSSLCALL(EVP_EncryptUpdate(
          ctx.get(), out.begin(), &updateLength, plaintext.begin(), plaintext.size()));
      int finalLength = 0;
      OSSLCALL(EVP_EncryptFinal_ex(ctx.get(), out.begin() + updateLength, &finalLength));
      KJ_ASSERT(size_t(updateLength + finalLength) == out.size());

      auto macKey = deriveKey(KJ_ASSERT_NONNULL(macInfo), segmentIndex);
      auto mac = hmacSha256(macKey, result.first(signedLength));
      result.slice(signedLength).copyFrom(mac);
      break;
    }
  }
  return result;
}

kj::Array<kj::byte> SegmentCipher::open(
    uint64_t segmentIndex, kj::ArrayPtr<const kj::byte> sealed) const {
  auto key = deriveKey(info, segmentIndex);

  switch (layout.getAlgorithm()) {
    case SegmentAlgorithm::AES_GCM:
    case SegmentAlgorithm::CHACHA20_POLY1305: {
      if (sealed.size() <= AEAD_NONCE_SIZE + AEAD_TAG_SIZE) failAuthentication(segmentIndex);
      auto nonce = sealed.first(AEAD_NONCE_SIZE);
      auto in = sealed.slice(AEAD_NONCE_SIZE);
      auto result = kj::heapArray<kj::byte>(in.size() - AEAD_TAG_SIZE);
      auto ctx = newAeadContext(aeadFor(layout.getAlgorithm(), keyLength), key);
      size_t written = 0;
      if (EVP_AEAD_CTX_open(ctx.get(), result.begin(), &written, result.size(), nonce.begin(),
              nonce.size(), in.begin(), in.size(), nullptr, 0) != 1) {
        ERR_clear_error();
        failAuthentication(segmentIndex);
      }
      KJ_ASSERT(written == result.size());
      return result;
    }
    case SegmentAlgorithm::AES_CBC_HMAC_SHA256: {
      // The smallest segment is one IV block, one ciphertext block and the MAC.
      if (sealed.size() < 2 * CBC_BLOCK_SIZE + HMAC_SHA256_SIZE ||
          (sealed.size() - HMAC_SHA256_SIZE) % CBC_BLOCK_SIZE != 0) {
        failAuthentication(segmentIndex);
      }
      auto signedData = sealed.first(sealed.size() - HMAC_SHA256_SIZE);
      auto expectedMac = sealed.slice(signedData.size());
      // Encrypt-then-MAC: nothing is decrypted until the MAC verifies.
      auto macKey = deriveKey(KJ_ASSERT_NONNULL(macInfo), segmentIndex);
      auto mac = hmacSha256(macKey, signedData);
      if (CRYPTO_memcmp(mac.begin(), expectedMac.begin(), HMAC_SHA256_SIZE) != 0) {
        failAuthentication(segmentIndex);
      }

      auto iv = signedData.first(CBC_BLOCK_SIZE);
      auto in = signedData.slice(CBC_BLOCK_SIZE);
      auto result = kj::heapArray<kj::byte>(in.size());
      auto ctx = kj::disposeWith<EVP_CIPHER_CTX_free>(EVP_CIPHER_CTX_new());
      KJ_ASSERT(ctx.get() != nullptr);
      OSSLCALL(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.begin(), iv.begin()));
      int updateLength = 0;
      OSSLCALL(EVP_DecryptUpdate(ctx.get(), result.begin(), &updateLength, in.begin(), in.size()));
      int finalLength = 0;
      if (EVP_DecryptFinal_ex(ctx.get(), result.begin() + updateLength, &finalLength) != 1) {
        // The MAC verified, so the writer padded it wrongly.
        ERR_clear_error();
        failAuthentication(segmentIndex);
      }
      return kj::heapArray(result.first(updateLength + finalLength));
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api
