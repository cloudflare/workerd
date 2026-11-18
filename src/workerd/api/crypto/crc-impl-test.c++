// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crc-impl.h"

#include <kj/array.h>
#include <kj/string.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

// Bit-at-a-time CRC-64/NVME, written directly from the catalogue definition
// (https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-64-nvme): reflected input and
// output, polynomial 0xad93d23594c93659 (0x9a6c9329ac4bc9b5 bit-reversed), init and xorout all
// ones. It shares no tables or code with crc64nvme(), so the two can be compared on any input.
uint64_t referenceCrc64nvme(uint64_t crc, kj::ArrayPtr<const kj::byte> data) {
  crc = ~crc;
  for (kj::byte b: data) {
    crc ^= b;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? 0x9a6c9329ac4bc9b5 : 0);
    }
  }
  return ~crc;
}

uint64_t crc64nvmeOf(uint64_t crc, kj::ArrayPtr<const kj::byte> data) {
  return crc64nvme(crc, data.begin(), data.size());
}

// Deterministic pseudo-random bytes (xorshift64), so failures reproduce.
kj::Array<kj::byte> pseudoRandomBytes(size_t size) {
  auto bytes = kj::heapArray<kj::byte>(size);
  uint64_t state = 0x2545f4914f6cdd1d;
  for (auto& b: bytes) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    b = static_cast<kj::byte>(state >> 56);
  }
  return bytes;
}

KJ_TEST("crc64nvme matches the CRC-64/NVME check value and known digests") {
  KJ_EXPECT(crc64nvmeOf(0, "123456789"_kjb) == 0xae8b14860a799888);
  KJ_EXPECT(crc64nvmeOf(0, ""_kjb) == 0);
  KJ_EXPECT(crc64nvmeOf(0, "abc"_kjb) == 0x05e5cabb3fc1faeb);
  KJ_EXPECT(crc64nvmeOf(0, "Hello world"_kjb) == 0x38e259d03f312adb);
}

KJ_TEST("crc64nvme of a null pointer is zero") {
  KJ_EXPECT(crc64nvme(0, nullptr, 0) == 0);
}

KJ_TEST("crc64nvme matches the bit-at-a-time reference at every length and alignment") {
  auto bytes = pseudoRandomBytes(1024 + 16);
  for (size_t offset = 0; offset < 16; ++offset) {
    for (size_t length = 0; length <= 1024; ++length) {
      auto input = bytes.slice(offset, offset + length);
      uint64_t expected = referenceCrc64nvme(0, input);
      uint64_t actual = crc64nvmeOf(0, input);
      KJ_EXPECT(actual == expected, offset, length, actual, expected);
    }
  }
}

KJ_TEST("crc64nvme matches the reference on large unaligned inputs") {
  auto bytes = pseudoRandomBytes((1 << 20) + 64);
  for (size_t offset: {0, 1, 7, 8, 15}) {
    for (size_t length: {4096, 65536, 65536 + 13, 1 << 20}) {
      auto input = bytes.slice(offset, offset + length);
      uint64_t expected = referenceCrc64nvme(0, input);
      uint64_t actual = crc64nvmeOf(0, input);
      KJ_EXPECT(actual == expected, offset, length, actual, expected);
    }
  }
}

KJ_TEST("crc64nvme gives the same result when the input is split across calls") {
  auto bytes = pseudoRandomBytes(5000);
  uint64_t expected = crc64nvmeOf(0, bytes);
  for (size_t first: {0, 1, 15, 16, 17, 127, 128, 255, 256, 1000, 4999, 5000}) {
    for (size_t second: {0, 1, 3, 16, 129, 300, 2048}) {
      size_t secondEnd = kj::min(first + second, bytes.size());
      uint64_t crc = crc64nvmeOf(0, bytes.first(first));
      crc = crc64nvmeOf(crc, bytes.slice(first, secondEnd));
      crc = crc64nvmeOf(crc, bytes.slice(secondEnd, bytes.size()));
      KJ_EXPECT(crc == expected, first, second, crc, expected);
    }
  }
}

}  // namespace
}  // namespace workerd::api
