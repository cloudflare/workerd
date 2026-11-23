// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "segment-cipher.h"

#include <openssl/evp.h>

#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

// Fills with a counter, so that sealing is deterministic.
class CountingEntropy final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (auto& byte: buffer) byte = next++;
  }

 private:
  kj::byte next = 0;
};

SegmentInfoTemplate info(size_t prefixSize) {
  auto prefix = kj::heapArray<kj::byte>(prefixSize);
  prefix.asPtr().fill('p');
  return {.prefix = kj::mv(prefix), .index = SegmentIndexEncoding::DECIMAL};
}

SegmentCipher cipher(SegmentAlgorithm algorithm, size_t prefixSize = 4) {
  auto prk = kj::heapArray<kj::byte>(32);
  prk.asPtr().fill(7);
  kj::Maybe<SegmentInfoTemplate> macInfo;
  if (algorithm == SegmentAlgorithm::AES_CBC_HMAC_SHA256) macInfo = info(4);
  return SegmentCipher(SegmentLayout(algorithm, 1024),
      SegmentCipher::KeySchedule{
        .digest = EVP_sha256(),
        .prk = kj::mv(prk),
        .info = info(prefixSize),
        .macInfo = kj::mv(macInfo),
      },
      256);
}

constexpr SegmentAlgorithm ALGORITHMS[] = {
  SegmentAlgorithm::AES_GCM,
  SegmentAlgorithm::CHACHA20_POLY1305,
  SegmentAlgorithm::AES_CBC_HMAC_SHA256,
};

KJ_TEST("a segment cipher retains its key schedule") {
  // The stream charges this to the isolate from the moment it is created.
  KJ_EXPECT(cipher(SegmentAlgorithm::AES_GCM).retainedBytes() == 32 + 4);
  KJ_EXPECT(cipher(SegmentAlgorithm::AES_CBC_HMAC_SHA256).retainedBytes() == 32 + 4 + 4);
  KJ_EXPECT(cipher(SegmentAlgorithm::AES_GCM, 8192).retainedBytes() == 32 + 8192);
}

KJ_TEST("a sealed segment is allocated at exactly its size and opens to its plaintext") {
  CountingEntropy entropy;
  for (auto algorithm: ALGORITHMS) {
    auto segments = cipher(algorithm);
    // CBC pads 15 bytes to one block and 16 to two.
    for (size_t size: {1, 15, 16, 1023, 1024}) {
      auto plaintext = kj::heapArray<kj::byte>(size);
      for (size_t i = 0; i < size; ++i) plaintext[i] = static_cast<kj::byte>(i * 31);
      auto sealed = segments.seal(3, plaintext, entropy);
      KJ_EXPECT(sealed.size() == segments.getLayout().sealedSegmentSize(size), size);
      auto opened = segments.open(3, sealed);
      KJ_EXPECT(opened == plaintext, size);
      // Another index is another key.
      KJ_EXPECT_THROW_MESSAGE("failed authentication", segments.open(4, sealed));
    }
  }
}

}  // namespace
}  // namespace workerd::api
