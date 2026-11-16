// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The read path on long-lived streams, so construction is out of the
// picture: each call performs 16 operations, since one costs about as much
// as the runner's overhead per call. The streams never close, and each call
// leaves its stream as it found it.

import { CHUNK, expectEqual } from 'helpers';

const COUNT = 16;

async function readOne(reader) {
  const { done } = await reader.read();
  expectEqual(done, false, 'done');
}

async function readMany(reader) {
  for (let i = 0; i < COUNT; i++) await readOne(reader);
}

export const reader = {
  bench(b) {
    const idle = new ReadableStream();
    b.run(`getReader() + releaseLock() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) idle.getReader().releaseLock();
    });

    const onDemand = new ReadableStream(
      {
        pull(controller) {
          controller.enqueue(CHUNK);
        },
      },
      { highWaterMark: 0 }
    ).getReader();
    b.run(`read(), pulled on demand x${COUNT}`, () => readMany(onDemand));

    // The default high-water mark of 1 keeps a chunk queued: each read takes
    // it, and pull() refills the queue.
    const refilled = new ReadableStream({
      pull(controller) {
        controller.enqueue(CHUNK);
      },
    }).getReader();
    b.run(`read(), queue refilled by pull() x${COUNT}`, () =>
      readMany(refilled)
    );

    const asyncPulled = new ReadableStream(
      {
        async pull(controller) {
          controller.enqueue(CHUNK);
        },
      },
      { highWaterMark: 0 }
    ).getReader();
    b.run(`read(), async pull() x${COUNT}`, () => readMany(asyncPulled));

    let controller;
    const pushed = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 0 }
    ).getReader();
    b.run(`enqueue() then read() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        controller.enqueue(CHUNK);
        await readOne(pushed);
      }
    });
    b.run(`read() then enqueue() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        const read = readOne(pushed);
        controller.enqueue(CHUNK);
        await read;
      }
    });
  },
};
