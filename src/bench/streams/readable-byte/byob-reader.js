// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// BYOB reads, reusing the buffer each read returns: the source filling the
// reader's views through byobRequest (no copy), and reads copying out of
// chunks the source enqueued. The minimum-read cases need four pulls per
// read.

import {
  SHAPES,
  drainByob,
  enqueuedByteStream,
  expectEqual,
  respondOnPull,
  shapeLabel,
  sizeLabel,
} from 'helpers';

// A byte stream whose pull() answers a quarter of each BYOB request.
function quarterResponseStream(count, size) {
  let remaining = count * 4;
  return new ReadableStream({
    type: 'bytes',
    pull(controller) {
      controller.byobRequest.respond(size / 4);
      if (--remaining === 0) controller.close();
    },
  });
}

export const byobReader = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`respond() ${shapeLabel(count, size)}`, async () => {
        const reader = respondOnPull(count, size).getReader({ mode: 'byob' });
        expectEqual(await drainByob(reader, size), count * size, 'bytes');
      });
    }
    for (const [count, size] of SHAPES) {
      b.run(
        `copied from enqueued chunks ${shapeLabel(count, size)}`,
        async () => {
          const reader = enqueuedByteStream(count, size).getReader({
            mode: 'byob',
          });
          expectEqual(await drainByob(reader, size), count * size, 'bytes');
        }
      );
    }

    const [count, size] = [64, 4096];
    b.run(`read(view, { min }) ${shapeLabel(count, size)}`, async () => {
      const reader = quarterResponseStream(count, size).getReader({
        mode: 'byob',
      });
      let buffer = new ArrayBuffer(size);
      let bytes = 0;
      for (;;) {
        const { value, done } = await reader.read(new Uint8Array(buffer), {
          min: size,
        });
        if (done) break;
        bytes += value.byteLength;
        buffer = value.buffer;
      }
      expectEqual(bytes, count * size, 'bytes');
    });
    b.run(`readAtLeast() ${shapeLabel(count, size)}`, async () => {
      const reader = quarterResponseStream(count, size).getReader({
        mode: 'byob',
      });
      let buffer = new ArrayBuffer(size);
      let bytes = 0;
      for (;;) {
        const { value, done } = await reader.readAtLeast(
          size,
          new Uint8Array(buffer)
        );
        if (done) break;
        bytes += value.byteLength;
        buffer = value.buffer;
      }
      expectEqual(bytes, count * size, 'bytes');
    });

    // One endless stream per view size; each call makes 16 reads from it.
    for (const viewSize of [64, 4096, 65536]) {
      const reader = new ReadableStream({
        type: 'bytes',
        pull(controller) {
          const request = controller.byobRequest;
          request.respond(request.view.byteLength);
        },
      }).getReader({ mode: 'byob' });
      let buffer = new ArrayBuffer(viewSize);
      b.run(
        `long-lived stream, read(view) ${sizeLabel(viewSize)} x16`,
        async () => {
          for (let i = 0; i < 16; i++) {
            const { value } = await reader.read(new Uint8Array(buffer));
            expectEqual(value.byteLength, viewSize, 'bytes');
            buffer = value.buffer;
          }
        }
      );
    }
  },
};
