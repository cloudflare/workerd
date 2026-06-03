// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-198: Heap UAF in
// ByteQueue ConsumerImpl via Object.prototype.then re-entrancy during
// controller.close().
//
// When a byte ReadableStream with a pending BYOB read ({min: N}) has
// buffered data (fewer than min bytes) and controller.close() is called,
// ByteQueue::handleMaybeClose() flushes the buffered bytes into the
// pending BYOB view and calls request->resolve(js). V8's promise
// resolution performs Get(resolution, "then"), which invokes an
// attacker-installed Object.prototype.then getter. Inside the getter,
// reader.cancel() frees the ByteQueue::Consumer while
// ConsumerImpl::maybeDrainAndSetState() is still on the stack.
//
// The fix adds selfRef.addRef() liveness guards around handleMaybeClose
// and after each request->resolve(js) call inside it.

import { strictEqual } from 'node:assert';

export const byobCloseReentryViaThen = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    // Issue a BYOB read with min:10 into a 10-byte buffer.
    // Pending read sits in ConsumerImpl::Ready::readRequests.
    reader.read(new Uint8Array(10), {
      min: 10,
    });

    // Enqueue 5 bytes — fewer than min, so the read stays pending.
    controller.enqueue(new Uint8Array([1, 2, 3, 4, 5]));

    let armed = true;
    const noopThen = function (resolve, reject) {
      /* never settle — prevents further thenable chaining */
    };

    // Install a trap on Object.prototype.then. When V8 resolves
    // the pending read inside handleMaybeClose, it checks for a
    // "then" property on the ReadResult wrapper. Our getter calls
    // reader.cancel() to free the ConsumerImpl while
    // handleMaybeClose / maybeDrainAndSetState still hold raw
    // references to it.
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (armed) {
          armed = false;
          try {
            reader.cancel();
          } catch {
            // cancel may throw — that's fine
          }
          return noopThen;
        }
        return undefined;
      },
    });

    try {
      // controller.close() enters the close path:
      //   ReadableByteStreamController::close →
      //   ReadableImpl::close → ByteQueue::close →
      //   QueueImpl::close → ConsumerImpl::close →
      //   maybeDrainAndSetState → handleMaybeClose
      // handleMaybeClose flushes the 5 buffered bytes into the
      // BYOB view and calls request->resolve(js), triggering
      // the then getter.
      controller.close();
    } catch {
      // close may throw due to re-entrant cancel — expected
    }

    armed = false;
    delete Object.prototype.then;

    // If we get here without SIGSEGV / UAF, the fix works.
    strictEqual(
      true,
      true,
      'survived re-entrant cancel during BYOB close drain'
    );
  },
};
