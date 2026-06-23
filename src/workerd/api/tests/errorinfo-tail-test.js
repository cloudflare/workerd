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

    // 6. Native Error whose `stack` getter throws. Extraction must be skipped
    //    for that arg while a normal Error in the same call is still captured.
    const throwingStack = new Error('explode');
    Object.defineProperty(throwingStack, 'stack', {
      get() {
        throw new Error('stack getter blew up');
      },
    });
    console.error(throwingStack, new TypeError('survivor'));

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
      6,
      `expected 6 log entries, got ${capturedLogs.length}`
    );

    const [plain, typeErr, ctxRange, fakeErr, mixed, throwy] = capturedLogs;

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
        typeErr.errorInfo[0].stack.length > 0,
      `stack should be a non-empty string, got: ${typeErr.errorInfo[0].stack}`
    );
    assert.ok(
      typeErr.errorInfo[0].stack.includes('at '),
      `stack should contain call frames, got: ${typeErr.errorInfo[0].stack}`
    );
    assert.ok(
      !typeErr.errorInfo[0].stack.includes('TypeError'),
      `stack should have the "Name: message" prefix stripped, got: ${typeErr.errorInfo[0].stack}`
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

    // 6. Error with a throwing `stack` getter: extraction throws and is
    //    swallowed, so slot 0 is null, but the request still completed and
    //    the sibling Error in slot 1 is captured normally.
    assert.ok(Array.isArray(throwy.errorInfo));
    assert.strictEqual(throwy.errorInfo.length, 2);
    assert.strictEqual(throwy.errorInfo[0], null);
    assert.ok(throwy.errorInfo[1]);
    assert.strictEqual(throwy.errorInfo[1].name, 'TypeError');
    assert.strictEqual(throwy.errorInfo[1].message, 'survivor');

    // Backwards compat: existing message field still present and an object.
    assert.strictEqual(typeof typeErr.message, 'object');
  },
};
