import { deepStrictEqual, strictEqual, ok } from 'node:assert';

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
    // Sequentially wait a very long time 11k times, each time aborting via an
    // AbortController. This exercises the abort path under heavy repetition.
    const iterations = 11_000;
    let completed = 0;
    for (let i = 0; i < iterations; i++) {
      const ac = new AbortController();
      const promise = scheduler.wait(1_000_000, { signal: ac.signal });
      ac.abort();
      try {
        await promise;
        throw new Error('should have thrown');
      } catch (err) {
        strictEqual(err.message, 'The operation was aborted');
      }
      completed++;
    }
    strictEqual(
      completed,
      11_000,
      `Expected 11000 iterations but only completed ${completed}`
    );
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
