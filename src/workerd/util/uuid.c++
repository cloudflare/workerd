// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <openssl/rand.h>

#include <kj/compat/http.h>
#include <kj/debug.h>

#include <cstdlib>

namespace workerd {
namespace {
constexpr char HEX_DIGITS[] = "0123456789abcdef";

static constexpr kj::FixedArray<int8_t, 256> HEX_VALUES = []() consteval {
  kj::FixedArray<int8_t, 256> result{};
  for (int i = 0; i < 256; i++) {
    result[i] = -1;
  }
  for (uint8_t c = '0'; c <= '9'; c++) {
    result[c] = c - '0';
  }
  for (uint8_t c = 'a'; c <= 'f'; c++) {
    result[c] = c - 'a' + 10;
  }
  for (uint8_t c = 'A'; c <= 'F'; c++) {
    result[c] = c - 'A' + 10;
  }
  return result;
}();

constexpr int hexValue(char c) {
  return HEX_VALUES[static_cast<uint8_t>(c)];
}
static_assert(hexValue('B') == 11);
static_assert(hexValue('a') == 10);
static_assert(hexValue('0') == 0);
static_assert(hexValue('G') == -1);
}  // namespace

kj::String randomUUID(kj::Maybe<kj::EntropySource&> optionalEntropySource) {
  kj::byte buffer[16]{};

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

kj::Maybe<UUID> UUID::fromString(kj::ArrayPtr<const char> str) {
  if (str.size() != 36u || str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
    return kj::none;
  }

  int v0 = hexValue(str[0]);
  int v1 = hexValue(str[1]);
  int v2 = hexValue(str[2]);
  int v3 = hexValue(str[3]);
  int v4 = hexValue(str[4]);
  int v5 = hexValue(str[5]);
  int v6 = hexValue(str[6]);
  int v7 = hexValue(str[7]);

  int v9 = hexValue(str[9]);
  int v10 = hexValue(str[10]);
  int v11 = hexValue(str[11]);
  int v12 = hexValue(str[12]);

  int v14 = hexValue(str[14]);
  int v15 = hexValue(str[15]);
  int v16 = hexValue(str[16]);
  int v17 = hexValue(str[17]);

  int v19 = hexValue(str[19]);
  int v20 = hexValue(str[20]);
  int v21 = hexValue(str[21]);
  int v22 = hexValue(str[22]);

  int v24 = hexValue(str[24]);
  int v25 = hexValue(str[25]);
  int v26 = hexValue(str[26]);
  int v27 = hexValue(str[27]);
  int v28 = hexValue(str[28]);
  int v29 = hexValue(str[29]);
  int v30 = hexValue(str[30]);
  int v31 = hexValue(str[31]);
  int v32 = hexValue(str[32]);
  int v33 = hexValue(str[33]);
  int v34 = hexValue(str[34]);
  int v35 = hexValue(str[35]);

  if ((v0 | v1 | v2 | v3 | v4 | v5 | v6 | v7 | v9 | v10 | v11 | v12 | v14 | v15 | v16 | v17 | v19 |
          v20 | v21 | v22 | v24 | v25 | v26 | v27 | v28 | v29 | v30 | v31 | v32 | v33 | v34 | v35) <
      0) {
    return kj::none;
  }

  // Please forgive the c-style casts here. Way less verbose and more readable than static_casts.
  uint64_t upper = ((uint64_t)v0 << 60) | ((uint64_t)v1 << 56) | ((uint64_t)v2 << 52) |
      ((uint64_t)v3 << 48) | ((uint64_t)v4 << 44) | ((uint64_t)v5 << 40) | ((uint64_t)v6 << 36) |
      ((uint64_t)v7 << 32) | ((uint64_t)v9 << 28) | ((uint64_t)v10 << 24) | ((uint64_t)v11 << 20) |
      ((uint64_t)v12 << 16) | ((uint64_t)v14 << 12) | ((uint64_t)v15 << 8) | ((uint64_t)v16 << 4) |
      ((uint64_t)v17);

  uint64_t lower = ((uint64_t)v19 << 60) | ((uint64_t)v20 << 56) | ((uint64_t)v21 << 52) |
      ((uint64_t)v22 << 48) | ((uint64_t)v24 << 44) | ((uint64_t)v25 << 40) |
      ((uint64_t)v26 << 36) | ((uint64_t)v27 << 32) | ((uint64_t)v28 << 28) |
      ((uint64_t)v29 << 24) | ((uint64_t)v30 << 20) | ((uint64_t)v31 << 16) |
      ((uint64_t)v32 << 12) | ((uint64_t)v33 << 8) | ((uint64_t)v34 << 4) | ((uint64_t)v35);

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

}  // namespace workerd
