// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-96.
//
// ConsumerImpl::maybeDrainAndSetState() drains pending reads on the close path,
// calling resolveAsDone(js) on each.
//
// Two independent things can free the consumer during that call:
//
//  - GC. The resolve performs a wrapOpaque() allocation (ReadResult is a
//    JSG_STRUCT, so it always takes the opaque path), and a collection there
//    can reclaim the ReadableStream that owns the ConsumerImpl through the
//    ownership gap -- QueueImpl holds only weak refs to its consumers. That
//    path extracts the pending reads into a local vector first, and re-checks a
//    weak ref before touching any member.
//
//  - Re-entrant JS. v8::Promise::Resolver::Resolve() does a thenable check,
//    Get(value, "then"), on the value being resolved. jsg's opaque wrappers
//    have a null prototype (Wrappable::attachOpaqueWrapper), so that lookup no
//    longer reaches Object.prototype and cannot reach a user getter.
//
// This test covers the second: the getter installed below must never run.
// Removing the null prototype makes it fail with a segmentation fault.
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
        // Reached only if the thenable check finds this getter, which would
        // free the ConsumerImpl mid-drain.
        ctrl.error(new Error('boom'));
        return thenFn;
      },
    });

    // close() → maybeDrainAndSetState → resolveAsDone → thenable check. The
    // getter must not run, so close() succeeds and the pending reads resolve
    // as done.
    ctrl.close();
    delete Object.prototype.then;

    ok(armed, 'thenable getter must not have fired');
  },
};
