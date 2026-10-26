// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An identity stream's whole life per call: queue every write and close,
// while the readable side is read to the end with a default reader, a BYOB
// reader, or readAtLeast(4 KiB). The implementations do different numbers
// of reads for the same bytes: over several queued writes, a BYOB read
// takes one write's bytes under C++ (with a minimum, stopping once it is
// met), while TypeScript fills the view with everything already written
// (identity ledger #22).

import {
  SHAPES,
  drainByob,
  drainBytes,
  expectEqual,
  pump,
  shapeLabel,
} from 'helpers';

const MIN = 4096;

async function drainAtLeast(reader) {
  let buffer = new ArrayBuffer(65536);
  let bytes = 0;
  for (;;) {
    const { value, done } = await reader.readAtLeast(
      MIN,
      new Uint8Array(buffer)
    );
    bytes += value.byteLength;
    if (done) return bytes;
    buffer = value.buffer;
  }
}

export const throughput = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`IdentityTransformStream ${shapeLabel(count, size)}`, async () => {
        const bytes = await pump(
          new IdentityTransformStream(),
          count,
          size,
          (readable) => drainBytes(readable.getReader())
        );
        expectEqual(bytes, count * size, 'bytes read');
      });
    }
    for (const [count, size] of SHAPES) {
      b.run(`FixedLengthStream ${shapeLabel(count, size)}`, async () => {
        const bytes = await pump(
          new FixedLengthStream(count * size),
          count,
          size,
          (readable) => drainBytes(readable.getReader())
        );
        expectEqual(bytes, count * size, 'bytes read');
      });
    }
    for (const [count, size] of SHAPES) {
      b.run(`BYOB reader ${shapeLabel(count, size)}`, async () => {
        const bytes = await pump(
          new IdentityTransformStream(),
          count,
          size,
          (readable) => drainByob(readable.getReader({ mode: 'byob' }), size)
        );
        expectEqual(bytes, count * size, 'bytes read');
      });
    }
    for (const [count, size] of SHAPES.slice(0, 2)) {
      b.run(`readAtLeast(4 KiB) ${shapeLabel(count, size)}`, async () => {
        const bytes = await pump(
          new IdentityTransformStream(),
          count,
          size,
          (readable) => drainAtLeast(readable.getReader({ mode: 'byob' }))
        );
        expectEqual(bytes, count * size, 'bytes read');
      });
    }
  },
};
