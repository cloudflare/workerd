// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <openssl/rand.h>
#include <cstdlib>

namespace workerd {
namespace {
constexpr char HEX_DIGITS[] = "0123456789abcdef";
}  // namespace

kj::String randomUUID(kj::Maybe<kj::EntropySource&> optionalEntropySource) {
  kj::byte buffer[16];

  KJ_IF_SOME(entropySource, optionalEntropySource) {
    entropySource.generate(buffer);
  } else {
    KJ_ASSERT(RAND_bytes(buffer, sizeof(buffer)) == 1);
  }
  buffer[6] = kj::byte((buffer[6] & 0x0f) | 0x40);
  buffer[8] = kj::byte((buffer[8] & 0x3f) | 0x80);

#define HEX(b) (char)(HEX_DIGITS[(b >> 4) & 0xf]), (char)(HEX_DIGITS[b & 0xf])

  // The format for Random UUID's is established in
  // https://www.rfc-editor.org/rfc/rfc4122.txt
  // xxxxxxxx-xxxx-Axxx-Bxxx-xxxxxxxxxxxx
  //
  // The sequence is 16 hex-encoded random bytes
  // divided into 1 4-byte, 3 2-byte, and 1 6-byte
  // groups. The four most significant bits of the
  // 7th byte (A) are set to 0100xxxx (0x40), while
  // the two most significant bits of the 9th byte (B)
  // are set to 10xxxxxx (0x80). These are the key bits
  // that identify the type and version of the uuid.
  // All other bits are random. That ends up meaning
  // that in the serialized uuid, the first character
  // of the third grouping is always a 4, and the first
  // character of the fourth grouping is always either
  // an a, b, 8, or 9.

  // clang-format off
  return kj::String(kj::arr<char>(
    HEX(buffer[0]),
    HEX(buffer[1]),
    HEX(buffer[2]),
    HEX(buffer[3]),
    '-',
    HEX(buffer[4]),
    HEX(buffer[5]),
    '-',
    HEX(buffer[6]),
    HEX(buffer[7]),
    '-',
    HEX(buffer[8]),
    HEX(buffer[9]),
    '-',
    HEX(buffer[10]),
    HEX(buffer[11]),
    HEX(buffer[12]),
    HEX(buffer[13]),
    HEX(buffer[14]),
    HEX(buffer[15]),
    '\0'
  ));
  // clang-format on

#undef HEX
}

kj::Maybe<UUID> UUID::fromUpperLower(uint64_t upper, uint64_t lower) {
  if (upper == 0 && lower == 0) {
    return kj::none;
  }
  return UUID(upper, lower);
}

kj::Maybe<UUID> UUID::fromString(kj::StringPtr str) {
  if (str.size() != 36u) {
    return kj::none;
  }
  uint64_t upper = 0;
  uint64_t lower = 0;
  auto begin = str.cStr();
  char* p;
  upper += (strtoull(begin, &p, 16) << 32u);
  if (p - begin != 8 || *p != '-') {
    return kj::none;
  }
  upper += (strtoull(++p, &p, 16) << 16u);
  if (p - begin != 13 || *p != '-') {
    return kj::none;
  }
  upper += (strtoull(++p, &p, 16));
  if (p - begin != 18 || *p != '-') {
    return kj::none;
  }
  lower += (strtoull(++p, &p, 16) << 48u);
  if (p - begin != 23 || *p != '-') {
    return kj::none;
  }
  lower += (strtoull(++p, &p, 16));
  if (p - begin != 36) {
    return kj::none;
  }
  if (upper == 0 && lower == 0) {
    return kj::none;
  }
  return UUID(upper, lower);
}


kj::String UUID::toString() const {
  // clang-format off
  return kj::str(
    HEX_DIGITS[(upper >> 60u) & 0xf],
    HEX_DIGITS[(upper >> 56u) & 0xf],
    HEX_DIGITS[(upper >> 52u) & 0xf],
    HEX_DIGITS[(upper >> 48u) & 0xf],
    HEX_DIGITS[(upper >> 44u) & 0xf],
    HEX_DIGITS[(upper >> 40u) & 0xf],
    HEX_DIGITS[(upper >> 36u) & 0xf],
    HEX_DIGITS[(upper >> 32u) & 0xf],
    '-',
    HEX_DIGITS[(upper >> 28u) & 0xf],
    HEX_DIGITS[(upper >> 24u) & 0xf],
    HEX_DIGITS[(upper >> 20u) & 0xf],
    HEX_DIGITS[(upper >> 16u) & 0xf],
    '-',
    HEX_DIGITS[(upper >> 12u) & 0xf],
    HEX_DIGITS[(upper >>  8u) & 0xf],
    HEX_DIGITS[(upper >>  4u) & 0xf],
    HEX_DIGITS[(upper >>  0u) & 0xf],
    '-',
    HEX_DIGITS[(lower >> 60u) & 0xf],
    HEX_DIGITS[(lower >> 56u) & 0xf],
    HEX_DIGITS[(lower >> 52u) & 0xf],
    HEX_DIGITS[(lower >> 48u) & 0xf],
    '-',
    HEX_DIGITS[(lower >> 44u) & 0xf],
    HEX_DIGITS[(lower >> 40u) & 0xf],
    HEX_DIGITS[(lower >> 36u) & 0xf],
    HEX_DIGITS[(lower >> 32u) & 0xf],
    HEX_DIGITS[(lower >> 28u) & 0xf],
    HEX_DIGITS[(lower >> 24u) & 0xf],
    HEX_DIGITS[(lower >> 20u) & 0xf],
    HEX_DIGITS[(lower >> 16u) & 0xf],
    HEX_DIGITS[(lower >> 12u) & 0xf],
    HEX_DIGITS[(lower >>  8u) & 0xf],
    HEX_DIGITS[(lower >>  4u) & 0xf],
    HEX_DIGITS[(lower >>  0u) & 0xf]
  );
  // clang-format on
}

} // namespace workerd
