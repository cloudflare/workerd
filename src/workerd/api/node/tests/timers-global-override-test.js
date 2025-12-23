// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for the enable_nodejs_global_timers compat flag.
// When this flag is enabled:
// - globalThis.setTimeout and globalThis.setInterval return Node.js-compatible
//   Timeout objects instead of numeric IDs
// - globalThis.setImmediate and globalThis.clearImmediate are available

import { strictEqual, ok } from 'node:assert';

// Test that globalThis.setTimeout returns a Timeout object with expected methods
export const testGlobalSetTimeoutReturnsObject = {
  async test() {
    const timeout = globalThis.setTimeout(() => {}, 1000);

    // Verify it's an object, not a number
    strictEqual(typeof timeout, 'object', 'setTimeout should return an object');

    // Verify it has the expected methods
    strictEqual(
      typeof timeout.refresh,
      'function',
      'Timeout should have refresh method'
    );
    strictEqual(
      typeof timeout.ref,
      'function',
      'Timeout should have ref method'
    );
    strictEqual(
      typeof timeout.unref,
      'function',
      'Timeout should have unref method'
    );
    strictEqual(
      typeof timeout.hasRef,
      'function',
      'Timeout should have hasRef method'
    );
    strictEqual(
      typeof timeout.close,
      'function',
      'Timeout should have close method'
    );

    // Verify Symbol.toPrimitive allows using as numeric ID for backward compat
    strictEqual(
      typeof +timeout,
      'number',
      'Timeout should be convertible to number'
    );
    ok(+timeout > 0, 'Timeout numeric ID should be positive');

    // Verify Symbol.dispose is present
    strictEqual(
      typeof timeout[Symbol.dispose],
      'function',
      'Timeout should have Symbol.dispose method'
    );

    // Clean up
    globalThis.clearTimeout(timeout);
  },
};

// Test that globalThis.setInterval returns a Timeout object with expected methods
export const testGlobalSetIntervalReturnsObject = {
  async test() {
    const interval = globalThis.setInterval(() => {}, 1000);

    // Verify it's an object, not a number
    strictEqual(
      typeof interval,
      'object',
      'setInterval should return an object'
    );

    // Verify it has the expected methods
    strictEqual(
      typeof interval.refresh,
      'function',
      'Interval should have refresh method'
    );
    strictEqual(
      typeof interval.ref,
      'function',
      'Interval should have ref method'
    );
    strictEqual(
      typeof interval.unref,
      'function',
      'Interval should have unref method'
    );
    strictEqual(
      typeof interval.hasRef,
      'function',
      'Interval should have hasRef method'
    );

    // Verify Symbol.toPrimitive allows using as numeric ID for backward compat
    strictEqual(
      typeof +interval,
      'number',
      'Interval should be convertible to number'
    );
    ok(+interval > 0, 'Interval numeric ID should be positive');

    // Clean up
    globalThis.clearInterval(interval);
  },
};

// Test that clearTimeout accepts both Timeout objects and numeric IDs
export const testClearTimeoutAcceptsBothTypes = {
  async test() {
    // Test clearing with Timeout object
    const t1 = globalThis.setTimeout(() => {
      throw new Error('timeout1 should have been cleared');
    }, 100);
    globalThis.clearTimeout(t1);

    // Test clearing with numeric ID (backward compatibility via Symbol.toPrimitive)
    const t2 = globalThis.setTimeout(() => {
      throw new Error('timeout2 should have been cleared');
    }, 100);
    globalThis.clearTimeout(+t2);

    // Wait to ensure cleared timeouts don't fire
    await new Promise((r) => globalThis.setTimeout(r, 200));
  },
};

// Test that clearInterval accepts both Timeout objects and numeric IDs
export const testClearIntervalAcceptsBothTypes = {
  async test() {
    // Test clearing with Timeout object
    const i1 = globalThis.setInterval(() => {
      throw new Error('interval1 should have been cleared');
    }, 50);
    globalThis.clearInterval(i1);

    // Test clearing with numeric ID (backward compatibility via Symbol.toPrimitive)
    const i2 = globalThis.setInterval(() => {
      throw new Error('interval2 should have been cleared');
    }, 50);
    globalThis.clearInterval(+i2);

    // Wait to ensure cleared intervals don't fire
    await new Promise((r) => globalThis.setTimeout(r, 200));
  },
};

// Test that the refresh() method works correctly
export const testTimeoutRefresh = {
  async test() {
    let callCount = 0;

    // Create a timeout that would fire in 50ms
    const timeout = globalThis.setTimeout(() => {
      callCount++;
    }, 50);

    // Wait 30ms, then refresh (reset the timer)
    await new Promise((r) => globalThis.setTimeout(r, 30));
    timeout.refresh();

    // Wait another 30ms (total 60ms from start, but only 30ms from refresh)
    await new Promise((r) => globalThis.setTimeout(r, 30));

    // The callback shouldn't have fired yet since we refreshed
    strictEqual(
      callCount,
      0,
      'Callback should not have fired yet after refresh'
    );

    // Wait another 30ms for the refreshed timer to fire
    await new Promise((r) => globalThis.setTimeout(r, 30));

    // Now it should have fired
    strictEqual(callCount, 1, 'Callback should have fired once');

    // Clean up (already fired, but good practice)
    globalThis.clearTimeout(timeout);
  },
};

// Test ref() and unref() methods (no-ops but should work without errors)
export const testRefUnref = {
  async test() {
    const timeout = globalThis.setTimeout(() => {}, 1000);

    // Initially, hasRef should return true (default behavior)
    strictEqual(timeout.hasRef(), true, 'hasRef should return true by default');

    // unref() should return the timeout object (chainable)
    const unrefResult = timeout.unref();
    strictEqual(unrefResult, timeout, 'unref should return the timeout object');
    strictEqual(
      timeout.hasRef(),
      false,
      'hasRef should return false after unref'
    );

    // ref() should return the timeout object (chainable)
    const refResult = timeout.ref();
    strictEqual(refResult, timeout, 'ref should return the timeout object');
    strictEqual(timeout.hasRef(), true, 'hasRef should return true after ref');

    // Clean up
    globalThis.clearTimeout(timeout);
  },
};

// Test that close() method clears the timeout
export const testClose = {
  async test() {
    let called = false;
    const timeout = globalThis.setTimeout(() => {
      called = true;
    }, 50);

    // close() should return the timeout object (chainable)
    const closeResult = timeout.close();
    strictEqual(closeResult, timeout, 'close should return the timeout object');

    // Wait to ensure the callback doesn't fire
    await new Promise((r) => globalThis.setTimeout(r, 100));
    strictEqual(
      called,
      false,
      'Callback should not have been called after close'
    );
  },
};

// Test that setTimeout works with callback arguments
export const testSetTimeoutWithArgs = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();

    globalThis.setTimeout(
      (a, b, c) => {
        strictEqual(a, 1, 'First arg should be 1');
        strictEqual(b, 'hello', 'Second arg should be "hello"');
        strictEqual(c, true, 'Third arg should be true');
        resolve();
      },
      10,
      1,
      'hello',
      true
    );

    await promise;
  },
};

// Test that setInterval works with callback arguments
export const testSetIntervalWithArgs = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();

    const interval = globalThis.setInterval(
      (a, b) => {
        strictEqual(a, 'foo', 'First arg should be "foo"');
        strictEqual(b, 42, 'Second arg should be 42');
        globalThis.clearInterval(interval);
        resolve();
      },
      10,
      'foo',
      42
    );

    await promise;
  },
};

// Test Symbol.dispose functionality
export const testSymbolDispose = {
  async test() {
    let called = false;
    const timeout = globalThis.setTimeout(() => {
      called = true;
    }, 50);

    // Use Symbol.dispose to clean up
    timeout[Symbol.dispose]();

    // Wait to ensure the callback doesn't fire
    await new Promise((r) => globalThis.setTimeout(r, 100));
    strictEqual(
      called,
      false,
      'Callback should not have been called after dispose'
    );
  },
};

// Test that globalThis.setImmediate is available and returns an Immediate object
export const testGlobalSetImmediateAvailable = {
  async test() {
    strictEqual(
      typeof globalThis.setImmediate,
      'function',
      'setImmediate should be available on globalThis'
    );
    strictEqual(
      typeof globalThis.clearImmediate,
      'function',
      'clearImmediate should be available on globalThis'
    );

    const { promise, resolve } = Promise.withResolvers();

    const immediate = globalThis.setImmediate(() => {
      resolve();
    });

    // Verify it's an object with expected methods
    strictEqual(
      typeof immediate,
      'object',
      'setImmediate should return an object'
    );
    strictEqual(
      typeof immediate.ref,
      'function',
      'Immediate should have ref method'
    );
    strictEqual(
      typeof immediate.unref,
      'function',
      'Immediate should have unref method'
    );
    strictEqual(
      typeof immediate.hasRef,
      'function',
      'Immediate should have hasRef method'
    );

    await promise;
  },
};

// Test that clearImmediate works correctly
export const testGlobalClearImmediate = {
  async test() {
    let called = false;
    const immediate = globalThis.setImmediate(() => {
      called = true;
    });

    globalThis.clearImmediate(immediate);

    // Wait a bit to ensure the callback doesn't fire
    await new Promise((r) => globalThis.setTimeout(r, 50));
    strictEqual(
      called,
      false,
      'Callback should not have been called after clearImmediate'
    );
  },
};

// Test that setImmediate works with callback arguments
export const testSetImmediateWithArgs = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();

    globalThis.setImmediate(
      (a, b) => {
        strictEqual(a, 'test', 'First arg should be "test"');
        strictEqual(b, 123, 'Second arg should be 123');
        resolve();
      },
      'test',
      123
    );

    await promise;
  },
};
