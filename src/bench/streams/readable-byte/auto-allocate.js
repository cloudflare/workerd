// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Default reads of a byte stream with autoAllocateChunkSize: the stream
// allocates each read's buffer and the source fills it through byobRequest,
// as it would for a BYOB reader.

import {
  SHAPES,
  drainBytes,
  expectEqual,
  respondOnPull,
  shapeLabel,
} from 'helpers';

export const autoAllocate = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`autoAllocateChunkSize ${shapeLabel(count, size)}`, async () => {
        const stream = respondOnPull(count, size, {
          autoAllocateChunkSize: size,
        });
        expectEqual(
          await drainBytes(stream.getReader()),
          count * size,
          'bytes read'
        );
      });
    }
  },
};
