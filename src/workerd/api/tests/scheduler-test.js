// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual, ok, rejects } from 'node:assert';

// Test for the Event and EventTarget standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

export const wait = {
  async test(ctrl, env, ctx) {
    strictEqual(typeof scheduler, 'object');
    strictEqual(typeof scheduler.wait, 'function');

    // regular wait...
    await scheduler.wait(10);

    // get's coerced to a number value (in this case 0)
    await scheduler.wait('foo');

    // waiting can be canceled
    try {
      await scheduler.wait(100000, { signal: AbortSignal.timeout(100) });
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted due to timeout');
    }

    try {
      await scheduler.wait(100000, { signal: AbortSignal.abort() });
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted');
    }

    try {
      const ac = new AbortController();
      const promise = scheduler.wait(10000, { signal: ac.signal });
      ac.abort();
      await promise;
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, 'The operation was aborted');
    }

    // Verify that ties are broken in the order the timeouts are scheduled
    let order = [];
    await Promise.all([
      scheduler.wait(100).then(() => order.push(1)),
      scheduler.wait(100).then(() => order.push(2)),
      scheduler.wait(50).then(() => order.push(0)),
      scheduler.wait(200).then(() => order.push(3)),
    ]);
    deepStrictEqual(order, [0, 1, 2, 3]);

    // Verify that timeouts are properly canceled when IoContext is destroyed.
    await env.subrequest.fetch('http://example.com');
    await env.subrequest.fetch('http://example.com');

    // globalThis.scheduler can be monkeypatched over...
    scheduler = 'foo';
    strictEqual(globalThis.scheduler, 'foo');
  },
};

export const sequentialAbort = {
  async test(ctrl, env, ctx) {
    // Aborting a wait must free its slot in the quota of 10,000 active timeouts. Fill all but one
    // slot with waits that never end, then repeatedly wait in the last free slot and abort: if an
    // abort leaked its slot, the next wait would exceed the quota. (Aborting 11,000 waits would
    // check the same, but each abort throws an exception, which takes about 5 ms under TSan.)
    const quota = 10_000;
    for (let i = 0; i < quota - 1; i++) {
      scheduler.wait(1_000_000);
    }
    for (let i = 0; i < 100; i++) {
      const ac = new AbortController();
      const promise = scheduler.wait(1_000_000, { signal: ac.signal });
      if (i === 0) {
        // The quota is full now, so this check would notice if the quota grew.
        await rejects(async () => scheduler.wait(1_000_000), {
          name: 'QuotaExceededError',
        });
      }
      ac.abort();
      await rejects(promise, { message: 'The operation was aborted' });
    }
  },
};

export default {
  async fetch() {
    // If globalThis.longWait exists, that means the timer fired after the
    // IoContext was destroyed. Boo!
    ok(!globalThis.longWait);
    scheduler.wait(10).then(() => {
      globalThis.longWait = true;
    });
    return new Response('not waiting');
  },
};
