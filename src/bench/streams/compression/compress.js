// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// CompressionStream's whole life per call: queue 64 KiB of text in chunks
// and close, while the compressed bytes are read to the end. Both
// implementations drive the same zlib codec, so the small chunks isolate
// the cost of the stream around it.

import {
  FORMATS,
  PAYLOAD,
  chunked,
  chunksLabel,
  expectCompressedSize,
  expectEqual,
  pump,
} from 'helpers';

export const compress = {
  async bench(b) {
    for (const format of FORMATS) {
      for (const size of format === 'gzip' ? [256, 4096, 65536] : [4096]) {
        const chunks = chunked(PAYLOAD, size);
        // The output can depend on how the input is split (it does with
        // zlib-rs, under the compression-rs autogate), so the expected size
        // comes from compressing the same chunks once beforehand.
        const expected = await pump(new CompressionStream(format), chunks);
        expectCompressedSize(expected);
        b.run(
          `${format}, 64 KiB ${chunksLabel(PAYLOAD.byteLength, size)}`,
          async () => {
            const bytes = await pump(new CompressionStream(format), chunks);
            expectEqual(bytes, expected, 'compressed bytes read');
          }
        );
      }
    }
  },
};
