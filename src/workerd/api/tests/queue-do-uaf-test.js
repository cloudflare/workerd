// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-300:
// Heap use-after-free in QueueEvent/QueueMessage via dangling
// IoPtr<QueueEventResult> on a Durable Object's persistent
// IoContext.

import assert from 'node:assert';

export class TestDO {
  constructor(state) {
    this.stashedBatch = null;
  }

  async queue(batch) {
    // Stash the QueueController so it survives past the queue
    // dispatch. Pre-fix, this retains a dangling
    // IoPtr<QueueEventResult> after QueueCustomEvent is freed.
    this.stashedBatch = batch;
  }

  async fetch(req) {
    const { pathname } = new URL(req.url);

    if (pathname === '/warmup') {
      // This request triggers the previous request's
      // drainFulfiller to be fulfilled, which (pre-fix) would
      // cause QueueCustomEvent to be freed.
      return new Response('ok');
    }

    if (pathname === '/trigger') {
      // Pre-fix: dereferences a dangling IoPtr, causing a heap
      // use-after-free (ASAN: READ of size 1 in retryAll).
      // Post-fix: QueueEventResult is kept alive by IoOwn.
      assert.notStrictEqual(
        this.stashedBatch,
        null,
        'batch should have been stashed'
      );

      // Exercise retryAll — the primary UAF sink.
      this.stashedBatch.retryAll({ delaySeconds: 5 });

      // Exercise per-message retry — the stronger UAF primitive
      // (kj::HashMap::upsert on freed memory).
      assert(
        this.stashedBatch.messages.length > 0,
        'should have at least one message'
      );
      this.stashedBatch.messages[0].retry({ delaySeconds: 10 });

      return new Response('ok');
    }

    return new Response('not found', { status: 404 });
  }
}

export default {
  async test(ctrl, env) {
    const stub = env.ns.get(env.ns.idFromName('uaf-regression'));

    // 1. Dispatch a queue event to the DO. The DO stashes the
    //    QueueController.
    const _queueResult = await stub.queue('test-queue', [
      {
        id: 'msg-1',
        timestamp: new Date(),
        body: 'hello',
        attempts: 1,
      },
    ]);

    // 2. Send a follow-up fetch to the same DO. This triggers
    //    the previous request's drain, which (pre-fix) would
    //    free QueueCustomEvent.
    const warmupResp = await stub.fetch('http://x/warmup');
    assert.strictEqual(await warmupResp.text(), 'ok');

    // 3. Now trigger the stashed batch operations. Pre-fix,
    //    this would be a UAF. Post-fix, QueueEventResult is
    //    still alive via IoOwn in QueueEvent.
    const triggerResp = await stub.fetch('http://x/trigger');
    assert.strictEqual(await triggerResp.text(), 'ok');
  },
};
