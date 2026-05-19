// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-148.
// ByteQueue::ByobRequest::respond() holds a raw ReadRequest& across
// queue.push() which can trigger user JS via thenable check. If the
// attacker calls readerA.releaseLock() inside the thenable getter,
// cancelPendingReads() frees the ReadRequest. respond() then accesses
// the freed req.pullInto.filled — UAF.
export const byobRespondReleaseLockViaThenableUAF = {
  async test() {
    let ctrl;
    const stream = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
    });

    const [a, b] = stream.tee();
    const readerA = a.getReader({ mode: 'byob' });
    const readerB = b.getReader({ mode: 'byob' });

    // Wrap in rejects() immediately — pa will be rejected synchronously
    // during byobReq.respond() when the thenable getter calls releaseLock().
    const pa = readerA.read(new Uint8Array(100));
    const pb = readerB.read(new Uint8Array(100));

    const byobReq = ctrl.byobRequest;

    let armed = true;
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (!armed) return undefined;
        armed = false;
        // Re-entrant during queue.push() → resolve() → thenable check.
        // readerA.releaseLock() → cancelPendingReads() → frees the
        // ReadRequest that respond() holds as a raw reference.
        readerA.releaseLock();
        return undefined;
      },
    });

    // With the fix, respond() detects the invalidated request after
    // queue.push() and returns without accessing freed memory.
    byobReq.respond(50);
    delete Object.prototype.then;

    // If we got here without ASAN crash, the fix works.
    // The thenable getter should have fired.
    ok(!armed, 'thenable getter should have fired');

    // readerA's pending read was canceled by releaseLock() inside the getter.
    await Promise.all([
      rejects(pa, { message: /This ReadableStream reader has been released/ }),
      pb,
    ]);
  },
};
