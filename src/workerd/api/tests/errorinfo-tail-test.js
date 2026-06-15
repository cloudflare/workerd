// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests that batched tail workers (`tail(events)`) receive a positional
// `errorInfo` array on TraceLog entries whose originating console.* call had at
// least one native Error among its arguments. See WO-1390.
//
// Shape: `log.errorInfo` is either undefined (no Errors in the args) or an
// array whose length matches the argument count, where each slot is either
// `{name, message, stack?}` (for native Error args) or `null` (for everything
// else).
import * as assert from 'node:assert';

// Captured by the tail handler so the `test` block can assert on it after the
// fetch invocation completes.
let capturedLogs = null;

export default {
  async fetch(request, env) {
    // 1. Plain string, no Errors anywhere.
    console.log('plain string');

    // 2. Single native Error as the only arg.
    console.error(new TypeError('boom'));

    // 3. Error in second position, string in first.
    console.warn('context:', new RangeError('out of range'));

    // 4. Duck-typed object that LOOKS like an Error but isn't.
    console.error({ name: 'FakeError', message: 'duck', stack: 'fake stack' });

    // 5. Multiple Errors interleaved with strings/objects: both Errors must
    //    appear in their respective positions; non-Error slots must be null.
    console.error(
      'before:',
      new Error('first'),
      { plain: 'obj' },
      new TypeError('second')
    );

    return new Response('ok');
  },

  tail(events) {
    const logs = [];
    for (const event of events) {
      if (event.logs) {
        for (const log of event.logs) {
          logs.push(log);
        }
      }
    }
    capturedLogs = logs;
  },
};

export const test = {
  async test(ctrl, env) {
    const response = await env.SERVICE.fetch('http://example.com/');
    assert.strictEqual(await response.text(), 'ok');

    // The tail handler runs after the fetch invocation completes.
    await scheduler.wait(100);

    assert.ok(capturedLogs, 'tail handler was never called');
    assert.strictEqual(
      capturedLogs.length,
      5,
      `expected 5 log entries, got ${capturedLogs.length}`
    );

    const [plain, typeErr, ctxRange, fakeErr, mixed] = capturedLogs;

    // 1. No Errors anywhere: field absent entirely.
    assert.strictEqual(
      plain.errorInfo,
      undefined,
      'plain string log should have no errorInfo'
    );

    // 2. Single Error in slot 0: array of length 1.
    assert.ok(Array.isArray(typeErr.errorInfo));
    assert.strictEqual(typeErr.errorInfo.length, 1);
    assert.strictEqual(typeErr.errorInfo[0].name, 'TypeError');
    assert.strictEqual(typeErr.errorInfo[0].message, 'boom');
    assert.ok(
      typeof typeErr.errorInfo[0].stack === 'string' &&
        typeErr.errorInfo[0].stack.includes('TypeError'),
      `stack should be a string containing "TypeError", got: ${typeErr.errorInfo[0].stack}`
    );

    // 3. Two-arg call with Error at index 1: positional shape preserved.
    assert.ok(Array.isArray(ctxRange.errorInfo));
    assert.strictEqual(ctxRange.errorInfo.length, 2);
    assert.strictEqual(ctxRange.errorInfo[0], null, 'string slot must be null');
    assert.ok(ctxRange.errorInfo[1]);
    assert.strictEqual(ctxRange.errorInfo[1].name, 'RangeError');
    assert.strictEqual(ctxRange.errorInfo[1].message, 'out of range');

    // 4. Duck-typed plain object: NOT treated as Error. Since the duck object
    //    is the only arg AND not an Error, errorInfo should be absent.
    assert.strictEqual(
      fakeErr.errorInfo,
      undefined,
      'plain object with Error-like fields should NOT produce errorInfo'
    );

    // 5. Four args: ['before:', Error('first'), {plain:'obj'}, TypeError('second')]
    //    → errorInfo = [null, {first}, null, {second}]
    assert.ok(Array.isArray(mixed.errorInfo));
    assert.strictEqual(mixed.errorInfo.length, 4);
    assert.strictEqual(mixed.errorInfo[0], null);
    assert.ok(mixed.errorInfo[1]);
    assert.strictEqual(mixed.errorInfo[1].name, 'Error');
    assert.strictEqual(mixed.errorInfo[1].message, 'first');
    assert.strictEqual(mixed.errorInfo[2], null);
    assert.ok(mixed.errorInfo[3]);
    assert.strictEqual(mixed.errorInfo[3].name, 'TypeError');
    assert.strictEqual(mixed.errorInfo[3].message, 'second');

    // Backwards compat: existing message field still present and an object.
    assert.strictEqual(typeof typeErr.message, 'object');
  },
};
