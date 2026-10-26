// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// DecompressionStream's whole life per call: queue the compressed text in
// chunks and close, while the 64 KiB it decompresses to are read to the
// end.

import {
  FORMATS,
  PAYLOAD,
  chunked,
  chunksLabel,
  compress,
  expectEqual,
  pump,
} from 'helpers';

export const decompress = {
  async bench(b) {
    for (const format of FORMATS) {
      const compressed = await compress(PAYLOAD, format);
      for (const size of format === 'gzip' ? [256, 4096, 65536] : [4096]) {
        const chunks = chunked(compressed, size);
        b.run(
          `${format}, to 64 KiB ${chunksLabel(compressed.byteLength, size)}`,
          async () => {
            const bytes = await pump(new DecompressionStream(format), chunks);
            expectEqual(bytes, PAYLOAD.byteLength, 'decompressed bytes read');
          }
        );
      }
    }
  },
};
