// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, throws } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-96.
// ConsumerImpl::maybeDrainAndSetState (close path) iterates
// readRequests calling resolveAsDone(js). The thenable getter calls
// controller.error() which frees the ConsumerImpl. Pre-fix: the
// range-for continued on freed RingBuffer storage. Post-fix:
// reads are extracted into a local before resolving.
export const closeResolveAsDoneThenableErrorFreesConsumer = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      start(c) {
        ctrl = c;
      },
    });

    const reader = rs.getReader();

    // Queue multiple pending reads so the iteration has >1 element.
    // We don't care about the results of these reads.
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

    let armed = true;
    const thenFn = function () {};
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (!armed) return thenFn;
        armed = false;
        // Re-entrant during resolveAsDone → thenable check.
        // controller.error() frees the ConsumerImpl.
        ctrl.error(new Error('boom'));
        return thenFn;
      },
    });

    // close() → maybeDrainAndSetState → resolveAsDone → thenable → error.
    // The re-entrant error transitions to terminal state, so close throws.
    throws(() => ctrl.close(), {
      message: /internal error/,
    });
    delete Object.prototype.then;

    ok(!armed, 'thenable getter should have fired');
  },
};
