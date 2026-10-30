// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// pipeTo() from a byte stream into a writable stream, to the end. enqueue()
// transfers its chunk's buffer, so every chunk is a new one.

import { SHAPES, expectEqual, shapeLabel } from 'helpers';

export const bytePipes = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`byte stream ${shapeLabel(count, size)}`, async () => {
        const source = new ReadableStream({
          type: 'bytes',
          start(controller) {
            for (let i = 0; i < count; i++) {
              controller.enqueue(new Uint8Array(size));
            }
            controller.close();
          },
        });
        let bytes = 0;
        await source.pipeTo(
          new WritableStream({
            write(chunk) {
              bytes += chunk.byteLength;
            },
          })
        );
        expectEqual(bytes, count * size, 'bytes written');
      });
    }
  },
};
