// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// A segmented authenticated cipher: the plaintext is cut into segments of a fixed size (only the
// last may be shorter), and each segment is sealed on its own under a key derived for its index.
// A reader can therefore start at any segment, and decrypt as the data streams in, without
// holding the whole message. Tink's streaming AEAD and age's STREAM are constructions of the same
// kind.
//
// Each sealed segment carries its own random nonce:
//
//   AES-GCM, ChaCha20-Poly1305:  nonce(12) || ciphertext || tag(16)
//   AES-CBC-HMAC-SHA256:         iv(16) || AES-CBC-PKCS7(plaintext) || HMAC-SHA256(iv || ct)(32)
//
// The key for segment i is HKDF-Expand(prk, info(i)), where prk is HKDF-Extract of the caller's
// HKDF key and info(i) = prefix || encode(i) || suffix. AES-CBC-HMAC-SHA256 derives its HMAC key
// the same way from a second info template.

#include <workerd/api/crypto/impl.h>

#include <kj/common.h>
#include <kj/string.h>

namespace kj {
class EntropySource;
}

namespace workerd::api {

enum class SegmentAlgorithm {
  AES_GCM,
  CHACHA20_POLY1305,
  AES_CBC_HMAC_SHA256,
};

// Parses an algorithm name, throwing a NotSupportedError for an unknown one.
SegmentAlgorithm parseSegmentAlgorithm(kj::StringPtr name);

// The sizes of a segmented message, which depend only on the algorithm and the segment size.
class SegmentLayout {
 public:
  // The supported range of segmentSize. The upper bound bounds the memory a stream holds.
  static constexpr uint32_t MIN_SEGMENT_SIZE = 1024;
  static constexpr uint32_t MAX_SEGMENT_SIZE = 16 * 1024 * 1024;

  // Throws a RangeError if segmentSize is out of range.
  SegmentLayout(SegmentAlgorithm algorithm, uint32_t segmentSize);

  SegmentAlgorithm getAlgorithm() const {
    return algorithm;
  }
  uint32_t getSegmentSize() const {
    return segmentSize;
  }

  // The size of one sealed segment holding `plaintextSize` bytes (at most segmentSize). An empty
  // plaintext is never sealed, so its size is 0.
  size_t sealedSegmentSize(size_t plaintextSize) const;

  // The size of a full sealed segment.
  size_t fullSealedSegmentSize() const {
    return sealedSegmentSize(segmentSize);
  }

  // The number of segments a plaintext of `plaintextLength` bytes is cut into.
  uint64_t segmentCount(uint64_t plaintextLength) const;

  // The total sealed size of a plaintext of `plaintextLength` bytes.
  uint64_t sealedLength(uint64_t plaintextLength) const;

 private:
  SegmentAlgorithm algorithm;
  uint32_t segmentSize;
};

// How a segment index is written into the HKDF info.
enum class SegmentIndexEncoding {
  // ASCII decimal digits, without leading zeros.
  DECIMAL,
  UINT32BE,
  UINT64BE,
};

// Parses an index encoding name, throwing a TypeError for an unknown one.
SegmentIndexEncoding parseSegmentIndexEncoding(kj::StringPtr name);

// The HKDF info for each segment: prefix || encode(index) || suffix.
struct SegmentInfoTemplate {
  kj::Array<kj::byte> prefix;
  SegmentIndexEncoding index;
  kj::Array<kj::byte> suffix;

  // The largest segment index the encoding can represent.
  uint64_t maxIndex() const;

  kj::Array<kj::byte> build(uint64_t segmentIndex) const;

  // The bytes the template holds, for memory accounting.
  size_t retainedBytes() const {
    return prefix.size() + suffix.size();
  }
};

// Seals and opens individual segments.
//
// Each segment's key is HKDF-Expand(prk, info(index)), and the nonce or IV beside it is random.
// The segment index authenticates a segment's position, and the decrypting side checks each
// segment's plaintext size against the plaintextLength it is given. Nothing else ties a segment to
// its message, so the caller must keep two things:
//
// - The key schedule (key, salt and info) must be unique to each message, and to each version of
//   a message that is rewritten under the same name: for example a fresh random salt for every
//   encryption, or an info that carries an authenticated version identifier. Otherwise segments at
//   the same index of two messages, or two versions, are interchangeable, and so are whole
//   messages that end at the same segment.
// - The plaintextLength given to the decrypting side must come from metadata that is itself
//   authenticated or trusted, such as the record that holds the salt. It is what detects
//   truncation and extension; the ciphertext cannot.
class SegmentCipher {
 public:
  struct KeySchedule {
    const EVP_MD* digest;
    // The HKDF pseudorandom key, from CryptoKey::hkdfExtract().
    kj::Array<kj::byte> prk;
    SegmentInfoTemplate info;
    // Required for, and only allowed with, AES-CBC-HMAC-SHA256.
    kj::Maybe<SegmentInfoTemplate> macInfo;
  };

  // `keyBits` is the AES key size for AES-GCM (128 or 256); the other algorithms have fixed key
  // sizes and require 256. Throws if the key schedule doesn't suit the algorithm.
  SegmentCipher(SegmentLayout layout, KeySchedule keys, uint32_t keyBits);

  const SegmentLayout& getLayout() const {
    return layout;
  }

  // The largest segment index the key schedule can derive a key for.
  uint64_t maxIndex() const;

  // The bytes the key schedule holds, for memory accounting.
  size_t retainedBytes() const;

  // Seals the plaintext of segment `segmentIndex`, which must be non-empty and at most
  // segmentSize bytes.
  kj::Array<kj::byte> seal(uint64_t segmentIndex,
      kj::ArrayPtr<const kj::byte> plaintext,
      kj::EntropySource& entropy) const;

  // Opens sealed segment `segmentIndex`, throwing an OperationError if it fails authentication.
  kj::Array<kj::byte> open(uint64_t segmentIndex, kj::ArrayPtr<const kj::byte> sealed) const;

 private:
  SegmentLayout layout;
  const EVP_MD* digest;
  ZeroOnFree prk;
  SegmentInfoTemplate info;
  kj::Maybe<SegmentInfoTemplate> macInfo;
  size_t keyLength;

  ZeroOnFree deriveKey(const SegmentInfoTemplate& infoTemplate, uint64_t segmentIndex) const;
};

}  // namespace workerd::api
