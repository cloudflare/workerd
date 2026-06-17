// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests that streaming tail workers (`tailStream`) receive a positional
// `errorInfo` array on log events whose originating console.* call had at
// least one native Error among its arguments. See WO-1390.
import * as assert from 'node:assert';

const capturedLogEvents = [];

export default {
  async fetch(request, env) {
    // Same matrix as the batched tail test for parity.
    console.log('plain string');
    console.error(new TypeError('boom'));
    console.warn('context:', new RangeError('out of range'));
    console.error({ name: 'FakeError', message: 'duck', stack: 'fake stack' });
    console.error(
      'before:',
      new Error('first'),
      { plain: 'obj' },
      new TypeError('second')
    );
    return new Response('ok');
  },

  tailStream(onsetEvent, env) {
    return (event) => {
      if (event.event.type === 'log') {
        capturedLogEvents.push(event.event);
      }
    };
  },
};

export const test = {
  async test(ctrl, env) {
    capturedLogEvents.length = 0;

    const response = await env.SERVICE.fetch('http://example.com/');
    assert.strictEqual(await response.text(), 'ok');
    await scheduler.wait(100);

    assert.strictEqual(
      capturedLogEvents.length,
      5,
      `expected 5 streamed log events, got ${capturedLogEvents.length}`
    );

    const [plain, typeErr, ctxRange, fakeErr, mixed] = capturedLogEvents;

    assert.strictEqual(plain.errorInfo, undefined);

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

    assert.ok(Array.isArray(ctxRange.errorInfo));
    assert.strictEqual(ctxRange.errorInfo.length, 2);
    assert.strictEqual(ctxRange.errorInfo[0], null);
    assert.ok(ctxRange.errorInfo[1]);
    assert.strictEqual(ctxRange.errorInfo[1].name, 'RangeError');
    assert.strictEqual(ctxRange.errorInfo[1].message, 'out of range');

    assert.strictEqual(fakeErr.errorInfo, undefined);

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
  },
};
