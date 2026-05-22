// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-94.
// ByobRequest::respond() holds raw ConsumerImpl& across resolveRead().
// The resolve triggers a thenable getter which calls controller.error(),
// freeing the ConsumerImpl. The unaligned-excess branch then calls
// consumer.push() on freed memory. The weak-ref liveness guard after
// resolveRead() should catch this.
export const byobRespondResolveReadThenableErrorFreesConsumer = {
  async test() {
    let controller;
    let savedReq;

    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
      pull(c) {
        if (!savedReq) {
          savedReq = c.byobRequest;
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    // BigUint64Array: elementSize=8, 64-byte buffer
    const readPromise = reader.read(new BigUint64Array(8));
    const promise = readPromise;

    // Let pull fire to capture the byobRequest.
    for (let i = 0; i < 10; i++) await Promise.resolve();

    let armed = true;
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (armed) {
          armed = false;
          // Re-entrant during resolveRead → thenable check.
          // controller.error() frees the ConsumerImpl via the error path.
          controller.error(new Error('boom'));
        }
        return undefined;
      },
    });

    // 11 bytes: filled=11 >= atLeast=8, unaligned = 11 % 8 = 3
    // → excess push after resolveRead on potentially freed consumer.
    savedReq.respond(11);

    delete Object.prototype.then;

    await promise;

    ok(!armed, 'thenable getter should have fired');
  },
};
