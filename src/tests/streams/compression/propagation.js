// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Abort/cancel propagation across the codec. Two divergences:
// - The canceling reader's own parked read: C++ rejects it with the cancel
//   reason; TypeScript resolves it done per WHATWG.
// - readable.cancel() reaching the writable side: TypeScript errors the
//   writable (writer.closed rejects with the reason); under C++ the
//   writable is left untouched and writer.closed stays pending.

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function macrotask() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

export const abortRejectsPendingRead = {
  async test() {
    const { writable, readable } = new CompressionStream('deflate');
    const reader = readable.getReader();
    const writer = writable.getWriter();
    const readPromise = reader.read();
    writer.abort(new Error('boom'));
    await rejects(readPromise, { message: 'boom' });
  },
};

export const cancelSettlesPendingRead = {
  async test() {
    const { readable } = new CompressionStream('deflate');
    const reader = readable.getReader();
    const readPromise = reader.read();
    reader.cancel(new Error('boom'));
    if (usingTsImpl) {
      strictEqual((await readPromise).done, true);
    } else {
      await rejects(readPromise, { message: 'boom' });
    }
  },
};

export const abortErrorsBothSides = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(new TextEncoder().encode('partial'));
    await writer.abort(new Error('abandon'));
    await rejects(cs.readable.getReader().read(), /abandon/);
  },
};

export const cancelReadableWritableAftermath = {
  async test() {
    const cs = new CompressionStream('gzip');
    await cs.readable.cancel(new Error('no more'));
    const writer = cs.writable.getWriter();
    if (usingTsImpl) {
      await rejects(writer.closed, /no more/);
    } else {
      let state = 'pending';
      writer.closed.then(
        () => (state = 'resolved'),
        () => (state = 'rejected')
      );
      await macrotask();
      await macrotask();
      strictEqual(state, 'pending');
    }
  },
};
