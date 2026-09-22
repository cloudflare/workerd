// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Closing the writable side propagates cleanly to the readable side.

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const closeResolvesPendingRead = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    await writer.close();
    const { value, done } = await readPromise;
    strictEqual(done, true);
    strictEqual(value, undefined);
    await writer.closed;
    await reader.closed;
  },
};

export const closeThenReadIsDone = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await writer.close();
    const reader = readable.getReader();
    const { done } = await reader.read();
    strictEqual(done, true);
    // Reads after EOF stay done.
    const again = await reader.read();
    strictEqual(again.done, true);
  },
};

export const bufferedDataDrainsBeforeDone = {
  async test() {
    // Data written before close must still be delivered; done comes after.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writePromise = writer.write(new Uint8Array([7, 8, 9]));
    const closePromise = writer.close();
    const first = await reader.read();
    strictEqual(first.done, false);
    strictEqual(first.value.byteLength, 3);
    const second = await reader.read();
    strictEqual(second.done, true);
    await writePromise;
    await closePromise;
  },
};

export const writesAfterQueuedCloseReject = {
  async test() {
    // Writes issued after close() has been queued (same synchronous turn)
    // all reject; the close still completes and the reader sees clean EOF
    // after the accepted bytes.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    await writer.write(Uint8Array.of(0x6b));
    const closePromise = writer.close();
    const expectedMsg = usingTsImpl
      ? 'Cannot write to a stream that is closing or closed'
      : 'This WritableStream has been closed.';
    const rejections = [];
    for (let i = 0; i < 3; i++) {
      rejections.push(
        rejects(writer.write(new Uint8Array(65536)), (err) => {
          strictEqual(err.constructor, TypeError);
          strictEqual(err.message, expectedMsg);
          return true;
        })
      );
    }
    await Promise.all(rejections);
    await closePromise;
    strictEqual((await readPromise).value[0], 0x6b);
    strictEqual((await reader.read()).done, true);
  },
};
