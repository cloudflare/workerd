// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual, ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-131.
// A BYOB reader.read() wraps the read in deferControllerStateChange which
// calls state.beginOperation(). The pull() callback is invoked synchronously.
// If the attacker calls reader.releaseLock() then stream.tee() from inside
// pull(), tee() uses KJ_DEFER(state.transitionTo<Closed>) which bypasses
// the operation guard (transitionTo checks transitionLockCount, not
// operationCount), destroying the kj::Own<ByteReadable> while
// onConsumerWantsData() is still on the stack.
export const byobReadTeeFromPullUAF = {
  async test() {
    let stream;
    let reader;
    let inPull = false;
    let teeResult;

    stream = new ReadableStream({
      type: 'bytes',
      pull(c) {
        if (inPull) return;
        inPull = true;
        // We're inside ByteReadable::read → onConsumerWantsData →
        // controller.pull → user pull (synchronous).
        // ByteReadable `this` is on the stack.
        reader.releaseLock();
        teeResult = stream.tee();
        // After return, onConsumerWantsData accesses this->state on
        // freed memory (pre-fix).
      },
    });

    reader = stream.getReader({ mode: 'byob' });
    await rejects(reader.read(new Uint8Array(16)), {
      message: /This ReadableStream reader has been released/,
    });

    // If we got here without crashing under ASAN, the fix works.
    // The stream should be in a closed or errored state after tee()
    // destroyed the consumer.
    ok(teeResult, 'tee() should have returned');
    strictEqual(teeResult.length, 2, 'tee() should return two branches');
  },
};
