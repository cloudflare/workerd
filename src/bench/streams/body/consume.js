// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// JavaScript streams as Response bodies, read by the runtime's body
// methods: value and byte streams, and an IdentityTransformStream fed by a
// concurrent writer, the shape of a worker producing a body it hands to the
// runtime.

import {
  SHAPES,
  byteStream,
  bytesOf,
  expectEqual,
  shapeLabel,
  valueStream,
} from 'helpers';

export const consume = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(
        `arrayBuffer() of a value stream ${shapeLabel(count, size)}`,
        async () => {
          const body = await new Response(
            valueStream(count, size)
          ).arrayBuffer();
          expectEqual(body.byteLength, count * size, 'bytes read');
        }
      );
    }
    for (const [count, size] of SHAPES) {
      b.run(
        `arrayBuffer() of a byte stream ${shapeLabel(count, size)}`,
        async () => {
          const body = await new Response(
            byteStream(count, size)
          ).arrayBuffer();
          expectEqual(body.byteLength, count * size, 'bytes read');
        }
      );
    }
    const [count, size] = SHAPES[1];
    b.run(`text() of a value stream ${shapeLabel(count, size)}`, async () => {
      const text = await new Response(valueStream(count, size)).text();
      expectEqual(text.length, count * size, 'characters read');
    });
    b.run(
      `arrayBuffer() of an IdentityTransformStream ${shapeLabel(count, size)}`,
      async () => {
        const { readable, writable } = new IdentityTransformStream();
        const writer = writable.getWriter();
        const chunk = bytesOf(size);
        const writes = [];
        for (let i = 0; i < count; i++) writes.push(writer.write(chunk));
        writes.push(writer.close());
        const [body] = await Promise.all([
          new Response(readable).arrayBuffer(),
          ...writes,
        ]);
        expectEqual(body.byteLength, count * size, 'bytes read');
      }
    );
  },
};
