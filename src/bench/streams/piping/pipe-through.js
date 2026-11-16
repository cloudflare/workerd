// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// pipeThrough() chains of transform streams, read to the end: the cost of
// each pipe a chunk crosses.

import { drainReader, enqueuedStream, expectEqual } from 'helpers';

function passThrough() {
  return {
    transform(chunk, controller) {
      controller.enqueue(chunk);
    },
  };
}

async function chain(count, length, makeTransformer) {
  let stream = enqueuedStream(count);
  for (let i = 0; i < length; i++) {
    stream = stream.pipeThrough(new TransformStream(makeTransformer()));
  }
  expectEqual(await drainReader(stream.getReader()), count, 'chunks read');
}

export const pipeThrough = {
  bench(b) {
    for (const count of [16, 256]) {
      b.run(`1 TransformStream x${count}`, () =>
        chain(count, 1, () => undefined)
      );
    }
    b.run('4 TransformStreams x256', () => chain(256, 4, () => undefined));
    b.run('1 TransformStream, transform() enqueues x256', () =>
      chain(256, 1, passThrough)
    );
    b.run('4 TransformStreams, transform() enqueues x256', () =>
      chain(256, 4, passThrough)
    );
  },
};
