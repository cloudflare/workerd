// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <openssl/rand.h>
#include <kj/debug.h>

namespace workerd {

kj::String randomUUID(kj::Maybe<kj::EntropySource&> optionalEntropySource) {
  kj::byte buffer[16];

  KJ_IF_MAYBE(entropySource, optionalEntropySource) {
    entropySource->generate(buffer);
  } else {
    KJ_ASSERT(RAND_bytes(buffer, sizeof(buffer)) == 1);
  }

  kj::Vector<char> result(37);

  constexpr auto HEX_DIGITS = "0123456789abcdef";

  auto add = [&](kj::byte b) {
    result.add(HEX_DIGITS[(b >> 4) & 0xf]);
    result.add(HEX_DIGITS[b & 0xf]);
  };

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

  add(buffer[0]);
  add(buffer[1]);
  add(buffer[2]);
  add(buffer[3]);
  result.add('-');

  add(buffer[4]);
  add(buffer[5]);

  result.add('-');

  add(kj::byte((buffer[6] & 0x0f) | 0x40));
  add(buffer[7]);

  result.add('-');

  add(kj::byte((buffer[8] & 0x3f) | 0x80));
  add(buffer[9]);

  result.add('-');

  add(buffer[10]);
  add(buffer[11]);
  add(buffer[12]);
  add(buffer[13]);
  add(buffer[14]);
  add(buffer[15]);

  result.add('\0');
  return kj::String(result.releaseAsArray());
}

}
