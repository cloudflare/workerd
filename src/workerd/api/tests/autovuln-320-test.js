// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-320.
// ConsumerImpl<ValueQueue>::cancel() iterates readRequests and calls
// resolveAsDone(js) which triggers a thenable getter. The getter calls
// controller.error() which frees the ConsumerImpl. Pre-fix, the
// range-for continued iterating the freed RingBuffer. Post-fix,
// cancel() extracts requests into a local before resolving.
export const cancelResolveAsDoneThenableErrorUAF = {
  async test() {
    let savedController;
    const rs = new ReadableStream({
      start(c) {
        savedController = c;
      },
    });
    const reader = rs.getReader();

    // Queue multiple pending reads so the iteration has >1 element.
    // Attach handlers to prevent unhandled rejection warnings.
    reader.read().then(
      () => {},
      () => {}
    );
    reader.read().then(
      () => {},
      () => {}
    );
    reader.read().then(
      () => {},
      () => {}
    );

    let triggered = false;
    const thenFn = function () {};
    Object.defineProperty(Object.prototype, 'then', {
      get() {
        if (!triggered) {
          triggered = true;
          savedController.error(new Error('boom'));
        }
        return thenFn;
      },
      configurable: true,
    });

    // cancel() → resolveAsDone → thenable check → getter → error().
    // Pre-fix: UAF on freed RingBuffer. Post-fix: iterates local copy.
    // The re-entrant error() causes cancel() to reject with "boom".
    await rejects(reader.cancel('cancel reason'), {
      message: 'boom',
    });
    delete Object.prototype.then;

    // If we got here without ASAN crash, the fix works.
    ok(triggered, 'thenable getter should have fired');
  },
};
