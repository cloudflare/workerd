// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextDecoderStream's whole life per call: queue every byte chunk and close,
// while the text is read to the end. The chunks are consecutive pieces of
// one encoded text, and the chunk sizes aren't multiples of three, so the
// three-byte chunks end in the middle of a character, which the decoder
// carries over to the next chunk.

import { SHAPES, TEXT, expectEqual, pump, sizeLabel } from 'helpers';

// The chunks of `size` bytes that `char`'s UTF-8 encoding, repeated, splits
// into: `chunkAt(i)` is the i-th. Writes don't modify their chunks, so a
// text of whole characters needs only one chunk per offset into the
// character at which a chunk can start.
function encodedChunks(char, size) {
  const unit = new TextEncoder().encode(char);
  const byOffset = [];
  for (let offset = 0; offset < unit.length; offset++) {
    const chunk = new Uint8Array(size);
    for (let i = 0; i < size; i++) chunk[i] = unit[(offset + i) % unit.length];
    byOffset.push(chunk);
  }
  return (i) => byOffset[(i * size) % unit.length];
}

// The number of characters `bytes` bytes of the text decode to: a partial
// last character decodes as U+FFFD at the close.
function decodedLength(bytes, bytesPerChar) {
  return Math.ceil(bytes / bytesPerChar);
}

export const decoder = {
  bench(b) {
    for (const [name, { char, bytesPerChar }] of Object.entries(TEXT)) {
      for (const [count, size] of SHAPES) {
        const chunkAt = encodedChunks(char, size);
        b.run(`${name} ${count} x ${sizeLabel(size)} bytes`, async () => {
          const length = await pump(
            new TextDecoderStream(),
            chunkAt,
            count,
            (value) => value.length
          );
          expectEqual(
            length,
            decodedLength(count * size, bytesPerChar),
            'characters read'
          );
        });
      }
    }
    const [count, size] = SHAPES[1];
    const chunkAt = encodedChunks('a', size);
    b.run(`fatal, ascii ${count} x ${sizeLabel(size)} bytes`, async () => {
      const length = await pump(
        new TextDecoderStream('utf-8', { fatal: true }),
        chunkAt,
        count,
        (value) => value.length
      );
      expectEqual(length, count * size, 'characters read');
    });
  },
};
