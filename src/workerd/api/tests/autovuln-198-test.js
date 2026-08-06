// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-198.
//
// maybeDrainAndSetState() holds a raw Ready& and ConsumerImpl& while calling
// handleMaybeClose() -> request->resolve(js). A re-entrant reader.cancel()
// would reach ByteReadable::cancel(), which sets state to kj::none, destroying
// the kj::Own<Consumer> and freeing the ConsumerImpl whose frame is still on
// the stack.
//
// Two independent things can free the consumer during that call:
//
//  - GC. The resolve performs a wrapOpaque() allocation (ReadResult is a
//    JSG_STRUCT, so it always takes the opaque path), and a collection there
//    can reclaim the ReadableStream that owns the ConsumerImpl through the
//    ownership gap -- QueueImpl holds only weak refs to its consumers. That
//    path takes a weak ref before calling handleMaybeClose() and re-checks it
//    afterwards.
//
//  - Re-entrant JS. v8::Promise::Resolver::Resolve() does a thenable check,
//    Get(value, "then"), on the value being resolved. jsg's opaque wrappers
//    have a null prototype (Wrappable::attachOpaqueWrapper), so that lookup no
//    longer reaches Object.prototype and cannot reach a user getter.
//
// This test covers the second: the getter installed below must never run.
// Removing the null prototype makes it fail with a V8 fatal error, "Invoke in
// DisallowJavascriptExecutionScope".
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
        // Reached only if the thenable check finds this getter. cancel()
        // reaches ByteReadable::cancel(), which sets state = kj::none and
        // frees the ConsumerImpl while maybeDrainAndSetState is on the stack.
        reader.cancel();
        return undefined;
      },
    });

    // close() → handleMaybeClose → resolve → thenable check. The getter must
    // not run, so no re-entrant cancel() happens and close() succeeds.
    controller.close();
    delete Object.prototype.then;

    ok(armed, 'thenable getter must not have fired');

    // min=10 was never satisfied (only 5 bytes enqueued), so closing rejects
    // the pending BYOB read.
    await p1.then(
      () => {},
      () => {}
    );
  },
};
