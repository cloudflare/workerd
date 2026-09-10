// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, strictEqual } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-148.
//
// ByteQueue::ByobRequest::respond() binds `auto& req = request.assertLive()`,
// and when the queue has more than one consumer it copies the response into an
// Entry and hands it to queue->push() so the other consumers' pending reads can
// be satisfied. It keeps using `req` afterwards.
//
// A ConsumerImpl is owned only by the ReadableStream above it -- QueueImpl
// holds nothing but weak refs -- so if the stream owning the responding
// consumer were ever collected during that push(), `req` would dangle and
// respond() would go on to read and write freed memory.
//
// Resolving the other consumers' reads inside push() allocates: ReadResult is a
// JSG_STRUCT, so every resolve goes through wrapOpaque(). Allocation is exactly
// where a collection can happen, which is what makes push() the dangerous
// point rather than an arbitrary one.
//
// This is not presently reachable. A consumer with a pending read is kept alive
// by that pending read, independently of whether any JS reference to the branch
// survives, so the consumer respond() is working on cannot be collected while
// its read is outstanding. The test does not assert that fact directly -- it is
// an internal lifetime invariant with no JS-visible surface -- it instead sets
// up the arrangement that would break if the invariant were lost:
//
//   * a byte stream tee'd into two consumers, so respond() takes the push()
//     path rather than the single-consumer fast path,
//   * a pending BYOB read on each branch, with branch A's being the one the
//     controller's byobRequest refers to,
//   * every JS reference to branch A dropped before respond() is called, so
//     the branch is unreachable from script and the pending-read root is the
//     only thing holding it.
//
// Under the @gc-stress variants, a collection is forced at points where one
// could legitimately occur. If the pending-read root is ever removed, branch A
// becomes collectible at the push() inside respond() and this test fails --
// under ASAN as a heap-use-after-free in respond(), otherwise as a crash or a
// wrong result. That makes it a tripwire on the lifetime invariant rather than
// a demonstration of a live bug.
export const byobRespondPushCollectsRespondingConsumer = {
  async test() {
    let ctrl;
    const stream = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
    });

    let [a, b] = stream.tee();
    let readerA = a.getReader({ mode: 'byob' });
    const readerB = b.getReader({ mode: 'byob' });

    // A pending BYOB read on each branch. Branch A reads first, so its request
    // is the one nextPendingByobReadRequest() hands back below.
    //
    // Branch A's promise is deliberately not kept in a variable: holding it
    // would be another root on the branch and would weaken the arrangement
    // described above. Its result is captured out-of-band instead.
    let resultA;
    let errorA;
    readerA.read(new Uint8Array(100)).then(
      (r) => {
        resultA = r;
      },
      (e) => {
        errorA = e;
      }
    );
    const pb = readerB.read(new Uint8Array(100));

    const byobReq = ctrl.byobRequest;
    ok(byobReq != null, 'expected a pending byobRequest');
    strictEqual(byobReq.view.byteLength, 100);

    // Drop every JS reference to branch A. Only the pending read still roots
    // it; if that root ever goes away, the push() below can collect it.
    a = null;
    readerA = null;

    // Two consumers, so this goes through queue->push() to hand the bytes to
    // branch B, and then keeps using the ReadRequest belonging to branch A.
    byobReq.respond(50);

    const rb = await pb;
    strictEqual(rb.done, false);
    strictEqual(rb.value.byteLength, 50);

    // Branch A's continuation is an ordinary microtask, so give the queue a
    // few turns to drain rather than assuming a particular ordering against
    // branch B's.
    for (
      let i = 0;
      i < 10 && resultA === undefined && errorA === undefined;
      i++
    ) {
      await Promise.resolve();
    }

    // Branch A's read was satisfied by the respond() itself.
    strictEqual(errorA, undefined, `branch A read rejected: ${errorA}`);
    ok(resultA != null, 'branch A read should have settled');
    strictEqual(resultA.done, false);
    strictEqual(resultA.value.byteLength, 50);
  },
};
