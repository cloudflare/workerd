// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-95.
// ByteQueue::handlePush() holds a Ready& reference across
// request->resolve(js). The resolve triggers a thenable getter
// which calls controller.error(), freeing the ConsumerImpl.
// The loop then reads from freed RingBuffer storage.
// The weak-ref liveness guard after resolve should catch this.
export const handlePushResolveReadThenableErrorFreesConsumer = {
  async test() {
    let ctrl;

    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
    });

    await Promise.resolve();

    const reader = rs.getReader({ mode: 'byob' });
    // The read resolves with the enqueued data before the error fires.
    const readP = reader.read(new Uint8Array(4));

    let armed = true;
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (!armed) return undefined;
        armed = false;
        // Re-entrant during handlePush → resolve → thenable check.
        // controller.error() frees the ConsumerImpl.
        ctrl.error(new Error('boom'));
        return undefined;
      },
    });

    // enqueue triggers handlePush → resolve → thenable getter → error.
    ctrl.enqueue(new Uint8Array(100));

    delete Object.prototype.then;

    await readP;

    ok(!armed, 'thenable getter should have fired');
  },
};
