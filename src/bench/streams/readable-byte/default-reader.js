// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A byte stream's whole life per call, read with a default reader: chunks
// enqueued in start(), or one enqueued per pull(). Under C++, each pull()
// also allocates a BYOB request that the enqueue() discards (see
// enqueueOnPull() in helpers.js).

import {
  SHAPES,
  drainBytes,
  enqueueOnPull,
  enqueuedByteStream,
  expectEqual,
  shapeLabel,
} from 'helpers';

async function drain(stream, bytes) {
  expectEqual(await drainBytes(stream.getReader()), bytes, 'bytes read');
}

export const defaultReader = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`enqueued in start() ${shapeLabel(count, size)}`, () =>
        drain(enqueuedByteStream(count, size), count * size)
      );
    }
    for (const [count, size] of SHAPES) {
      b.run(`enqueued in pull() ${shapeLabel(count, size)}`, () =>
        drain(enqueueOnPull(count, size), count * size)
      );
    }
  },
};
