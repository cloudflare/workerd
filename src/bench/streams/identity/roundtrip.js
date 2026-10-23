// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// One chunk through a long-lived IdentityTransformStream at a time, so
// construction is out of the picture: each call writes a chunk and reads its
// bytes back 16 times, since one round trip costs about as much as the
// runner's overhead per call. A write settles once its bytes have been
// read. The streams never close, and each call leaves its stream as it
// found it.

import { chunkOf, expectEqual, sizeLabel } from 'helpers';

const COUNT = 16;

// Reads exactly `size` bytes, however the implementation splits them.
async function readBytes(reader, size) {
  let bytes = 0;
  while (bytes < size) {
    const { value, done } = await reader.read();
    expectEqual(done, false, 'done');
    bytes += value.byteLength;
  }
  expectEqual(bytes, size, 'bytes read');
}

export const roundtrip = {
  bench(b) {
    for (const size of [64, 4096, 65536]) {
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reader = readable.getReader();
      const chunk = chunkOf(size);
      b.run(`write() + read() ${sizeLabel(size)} x${COUNT}`, async () => {
        for (let i = 0; i < COUNT; i++) {
          await Promise.all([writer.write(chunk), readBytes(reader, size)]);
        }
      });
    }
    for (const size of [64, 4096, 65536]) {
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reader = readable.getReader({ mode: 'byob' });
      const chunk = chunkOf(size);
      let buffer = new ArrayBuffer(size);
      b.run(`write() + read(view) ${sizeLabel(size)} x${COUNT}`, async () => {
        for (let i = 0; i < COUNT; i++) {
          const [, { value }] = await Promise.all([
            writer.write(chunk),
            reader.read(new Uint8Array(buffer)),
          ]);
          expectEqual(value.byteLength, size, 'bytes read');
          buffer = value.buffer;
        }
      });
    }
  },
};
