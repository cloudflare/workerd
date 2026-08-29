// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Helpers for large-body integration tests: a deterministic byte pattern
// that is continuous across chunk boundaries, so reordering, truncation,
// duplication, or boundary corruption anywhere in a multi-megabyte body
// fails with the offending byte index named. The modulus is prime so the
// pattern never falls into step with power-of-two buffer sizes.

import { fail, strictEqual } from 'node:assert';

export const PATTERN_MODULUS = 251;

// A Uint8Array of `length` bytes carrying the pattern for the absolute
// byte range [offset, offset + length).
export function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

// Writes `total` patterned bytes into the writable in `chunkLength`-sized
// writes (last chunk partial), then closes. Awaits each write, so memory
// stays bounded by the stream's own buffering.
export async function writePatternedBody(writable, total, chunkLength) {
  const writer = writable.getWriter();
  for (let offset = 0; offset < total; offset += chunkLength) {
    const length = offset + chunkLength > total ? total - offset : chunkLength;
    await writer.write(patternChunk(offset, length));
  }
  await writer.close();
}

// Asserts that `bytes` (a Uint8Array) is exactly `total` patterned bytes.
export function assertPatternedBytes(bytes, total) {
  strictEqual(bytes.byteLength, total, 'wrong total byte length');
  for (let i = 0; i < total; i++) {
    if (bytes[i] !== i % PATTERN_MODULUS) {
      fail(
        `byte ${i}: expected ${i % PATTERN_MODULUS}, got ${bytes[i]} ` +
          `(of ${total} total)`
      );
    }
  }
}
