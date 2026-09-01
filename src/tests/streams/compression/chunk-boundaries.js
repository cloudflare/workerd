// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk-boundary robustness: codec state carries across arbitrarily small
// writes on both the compression and decompression sides.

import { strictEqual } from 'node:assert';
import { pump } from 'round-trip';

const enc = new TextEncoder();
const dec = new TextDecoder();

export const byteAtATimeCompression = {
  async test() {
    const original = 'Hello, World! This is a test of chunked compression.';
    const bytes = enc.encode(original);
    const compressed = await pump(
      new CompressionStream('gzip'),
      [...bytes].map((b) => Uint8Array.of(b))
    );
    const restored = await pump(new DecompressionStream('gzip'), [compressed]);
    strictEqual(dec.decode(restored), original);
  },
};

export const splitCompressedInput = {
  async test() {
    // Multi-chunk writes on the way in, awkward splits on the way out.
    const parts = ['first ', 'second ', 'third'];
    const compressed = await pump(
      new CompressionStream('gzip'),
      parts.map((p) => enc.encode(p))
    );
    // Two-byte slices through the whole compressed body.
    const slices = [];
    for (let i = 0; i < compressed.length; i += 2) {
      slices.push(compressed.slice(i, Math.min(i + 2, compressed.length)));
    }
    const restored = await pump(new DecompressionStream('gzip'), slices);
    strictEqual(dec.decode(restored), parts.join(''));
  },
};

export const allFormatsChunkedWrites = {
  async test() {
    const original = 'Testing all compression formats with chunked data!';
    const bytes = enc.encode(original);
    for (const format of ['gzip', 'deflate', 'deflate-raw']) {
      const inChunks = [];
      for (let i = 0; i < bytes.length; i += 5) {
        inChunks.push(bytes.slice(i, Math.min(i + 5, bytes.length)));
      }
      const compressed = await pump(new CompressionStream(format), inChunks);
      const restored = await pump(new DecompressionStream(format), [
        compressed,
      ]);
      strictEqual(dec.decode(restored), original, `format ${format}`);
    }
  },
};
