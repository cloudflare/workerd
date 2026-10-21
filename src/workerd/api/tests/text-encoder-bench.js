// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextEncoder.encode() and encodeInto() across character types and string lengths, which
// exercises the ASCII fast path and the one- and two-byte UTF-8 paths.

const TYPES = {
  ascii: 'a',
  'one-byte': '\xff',
  'two-byte': '\u011f',
};
const LENGTHS = [32, 256, 1024, 8192];

export default {
  bench(b) {
    const encoder = new TextEncoder();
    for (const [type, char] of Object.entries(TYPES)) {
      for (const len of LENGTHS) {
        const input = char.repeat(len);
        b.run(`encode ${type} ${len}`, () => b.blackBox(encoder.encode(input)));
      }
    }
    for (const [type, char] of Object.entries(TYPES)) {
      for (const len of LENGTHS) {
        const input = char.repeat(len);
        // Enough space for any UTF-8 encoding of the input.
        const buffer = new Uint8Array(len * 3);
        b.run(`encodeInto ${type} ${len}`, () =>
          b.blackBox(encoder.encodeInto(input, buffer))
        );
      }
    }
  },
};
