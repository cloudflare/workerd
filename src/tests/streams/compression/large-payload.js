// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Large multi-pump payload (many 16KiB codec pump iterations, chunked
// writes), verified byte-for-byte.

import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import { readAll, concat } from 'round-trip';

export const largePayloadRoundtrip = {
  async test() {
    const size = 400 * 1024;
    const original = new Uint8Array(size);
    // Compressible but non-trivial content.
    for (let i = 0; i < size; i++) {
      original[i] = (i * 31 + ((i / 512) | 0)) & 0xff;
    }

    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const writes = (async () => {
      for (let off = 0; off < size; off += 64 * 1024) {
        await writer.write(
          original.subarray(off, Math.min(off + 64 * 1024, size))
        );
      }
      await writer.close();
    })();
    const compressed = concat(await readAll(cs.readable));
    await writes;
    ok(
      compressed.byteLength < size,
      `compressed output should be smaller: ${compressed.byteLength} vs ${size}`
    );

    const ds = new DecompressionStream('gzip');
    const writer2 = ds.writable.getWriter();
    const writes2 = (async () => {
      await writer2.write(compressed);
      await writer2.close();
    })();
    const roundtrip = concat(await readAll(ds.readable));
    await writes2;
    strictEqual(roundtrip.byteLength, size);
    deepStrictEqual(roundtrip, original);
  },
};
