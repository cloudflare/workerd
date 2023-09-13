import {
  deepStrictEqual,
  strictEqual,
  throws
} from 'node:assert';

// Test for the Event and EventTarget standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

export const wait = {
  async test() {
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

    // globalThis.scheduler can be monkeypatched over...
    scheduler = 'foo';
    strictEqual(globalThis.scheduler, 'foo');
  }
};


