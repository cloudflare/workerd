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
const INTO_TYPES = {
  ...TYPES,
  emoji: '\u{1F639}',
  mixed: 'Hi \u{1F44B} \u043f\u0440\u0438 \u6f22\u5b57 caf\xe9! ',
  'lone-surrogates': 'ab\ud800cd\udc00ef',
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
    for (const [type, char] of Object.entries(INTO_TYPES)) {
      for (const len of LENGTHS) {
        const input = char.repeat(Math.ceil(len / char.length)).slice(0, len);
        const needed = encoder.encode(input).length;
        // Enough space for any UTF-8 encoding of the input, and half of what this one needs.
        for (const [fit, size] of [
          ['', len * 3],
          [' half', needed >> 1],
        ]) {
          const buffer = new Uint8Array(size);
          b.run(`encodeInto ${type} ${len}${fit}`, () =>
            b.blackBox(encoder.encodeInto(input, buffer))
          );
        }
      }
    }
  },
};
