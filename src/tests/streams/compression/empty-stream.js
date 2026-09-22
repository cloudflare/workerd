// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Zero-payload streams: closing a CompressionStream with no writes still
// emits a valid (non-empty) compressed member, and decompressing that
// member yields clean EOF with no data chunks. (Contrast strict-checks.js:
// closing a DecompressionStream that has seen NO complete member is an
// error under strict_compression_checks.)

import { strictEqual, ok } from 'node:assert';
import { pump } from 'round-trip';

export const emptyPayloadRoundTrip = {
  async test() {
    for (const format of ['gzip', 'deflate']) {
      const compressed = await pump(new CompressionStream(format), []);
      ok(
        compressed.byteLength > 0,
        `${format}: empty input still produces header/trailer bytes`
      );
      const restored = await pump(new DecompressionStream(format), [
        compressed,
      ]);
      strictEqual(restored.byteLength, 0, `${format}: empty payload`);
    }
  },
};
