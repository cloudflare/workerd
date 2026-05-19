// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, throws } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-198.
// maybeDrainAndSetState holds raw Ready& / ConsumerImpl& while calling
// handleMaybeClose → request->resolve(js). The thenable getter calls
// reader.cancel() which reaches ByteReadable::cancel() → state = kj::none,
// directly destroying the kj::Own<Consumer> and freeing the ConsumerImpl
// whose maybeDrainAndSetState frame is on the stack.
export const cancelFromThenableFreesConsumerDuringClose = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader({ mode: 'byob' });

    // Pending BYOB read with min=10, then enqueue 5 (partial fill).
    const p1 = reader.read(new Uint8Array(10), { min: 10 });
    controller.enqueue(new Uint8Array(5));

    let armed = true;
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (!armed) return undefined;
        armed = false;
        // Re-entrant during handleMaybeClose → request->resolve().
        // reader.cancel() → ByteReadable::cancel() → state = kj::none
        // → frees the ConsumerImpl while maybeDrainAndSetState is on stack.
        reader.cancel();
        return undefined;
      },
    });

    // close() → handleMaybeClose → resolve → thenable check → getter.
    // The re-entrant cancel invalidates the stream state, so close throws.
    throws(() => controller.close(), {
      message: /internal error/,
    });
    delete Object.prototype.then;

    // If we got here without ASAN crash, the liveness guard worked.
    ok(!armed, 'thenable getter should have fired');

    // The read resolves with the partial data from the close path.
    await p1;
  },
};
