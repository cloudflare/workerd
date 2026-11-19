// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Durable Object that holds a WorkerStub from LOADER.get() can still go idle and be evicted:
// the stub's getCode callback holds the actor's IoContext weakly, so it does not keep the
// actor's request open.
import assert from 'node:assert';
import unsafe from 'workerd:unsafe';
import { DurableObject } from 'cloudflare:workers';

export class LoaderHolder extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.worker = null;
  }

  // Keeps the stub in a field. workerd fetches a dynamic worker's code once, at load, and drops
  // the getCode callback when the worker has started, so getCode() here never resolves: that
  // keeps the callback alive for as long as the stub.
  hold() {
    this.worker = this.env.LOADER.get('child', () => new Promise(() => {}));
  }

  holdsWorker() {
    return this.worker !== null;
  }
}

export const evictWhileHoldingWorkerStub = {
  async test(ctrl, env) {
    const stub = env.HOLDER.getByName('holder');
    await stub.hold();
    assert.strictEqual(await stub.holdsWorker(), true);

    // evict() waits for the actor to become idle. Its own deadline outlasts the test's, so a
    // shorter one turns an actor that never goes idle into a failure here.
    const deadline = new Promise((resolve, reject) => {
      setTimeout(
        () => reject(new Error('the Durable Object never became idle')),
        5000
      );
    });
    await Promise.race([unsafe.evict(stub), deadline]);

    // The next call reaches a new instance, which holds no stub.
    assert.strictEqual(await stub.holdsWorker(), false);
  },
};
