// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <openssl/rand.h>
#include <kj/debug.h>

namespace workerd {

kj::String randomUUID(kj::Maybe<kj::EntropySource&> optionalEntropySource) {
  kj::byte buffer[16];

  KJ_IF_SOME(entropySource, optionalEntropySource) {
    entropySource.generate(buffer);
  } else {
    KJ_ASSERT(RAND_bytes(buffer, sizeof(buffer)) == 1);
  }
  buffer[6] = kj::byte((buffer[6] & 0x0f) | 0x40);
  buffer[8] = kj::byte((buffer[8] & 0x3f) | 0x80);

  static constexpr char HEX_DIGITS[] = "0123456789abcdef";

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

#undef HEX
}

}
