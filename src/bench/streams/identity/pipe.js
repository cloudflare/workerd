// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Piping JavaScript streams into an identity stream, whose readable side is
// read to the end: the shape of a worker that hands a stream it produces to
// the runtime as a body.

import { SHAPES, chunkOf, drainBytes, expectEqual, shapeLabel } from 'helpers';

// A value stream of `count` chunks of `size` bytes. Value streams don't
// transfer their chunks, so one buffer serves as every chunk.
function valueStream(count, size) {
  const chunk = chunkOf(size);
  return new ReadableStream({
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(chunk);
      controller.close();
    },
  });
}

// A byte stream of `count` chunks of `size` bytes. enqueue() transfers its
// chunk's buffer, so every chunk is a new one.
function byteStream(count, size) {
  return new ReadableStream({
    type: 'bytes',
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(new Uint8Array(size));
      controller.close();
    },
  });
}

export const pipe = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(
        `pipeTo() from a value stream ${shapeLabel(count, size)}`,
        async () => {
          const { readable, writable } = new IdentityTransformStream();
          const [bytes] = await Promise.all([
            drainBytes(readable.getReader()),
            valueStream(count, size).pipeTo(writable),
          ]);
          expectEqual(bytes, count * size, 'bytes read');
        }
      );
    }
    for (const [count, size] of SHAPES) {
      b.run(
        `pipeThrough() from a byte stream ${shapeLabel(count, size)}`,
        async () => {
          const readable = byteStream(count, size).pipeThrough(
            new IdentityTransformStream()
          );
          expectEqual(
            await drainBytes(readable.getReader()),
            count * size,
            'bytes read'
          );
        }
      );
    }
  },
};
