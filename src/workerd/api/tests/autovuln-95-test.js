// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-95.
//
// ByteQueue::handlePush() holds a raw Ready& across request->resolve(js) while
// draining an entry into pending reads.
//
// Two independent things can free the consumer during that call:
//
//  - GC. The resolve performs a wrapOpaque() allocation (ReadResult is a
//    JSG_STRUCT, so it always takes the opaque path), and a collection there
//    can reclaim the ReadableStream that owns the ConsumerImpl through the
//    ownership gap -- QueueImpl holds only weak refs to its consumers.
//    handlePush() re-checks consumer liveness after each resolve.
//
//  - Re-entrant JS. v8::Promise::Resolver::Resolve() does a thenable check,
//    Get(value, "then"), on the value being resolved. jsg's opaque wrappers
//    have a null prototype (Wrappable::attachOpaqueWrapper), so that lookup no
//    longer reaches Object.prototype and cannot reach a user getter.
//
// This test covers the second: the getter installed below must never run.
// Removing the null prototype makes it fail with the getter firing and freeing
// the consumer mid-drain.
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
        // Reached only if the thenable check finds this getter, which would
        // free the ConsumerImpl out from under handlePush().
        ctrl.error(new Error('boom'));
        return undefined;
      },
    });

    // enqueue triggers handlePush → resolve → thenable getter → error.
    ctrl.enqueue(new Uint8Array(100));

    delete Object.prototype.then;

    await readP;

    ok(armed, 'thenable getter must not have fired');
  },
};
