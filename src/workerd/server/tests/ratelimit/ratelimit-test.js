// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `/workerd/ratelimit.js`, after Miniflare's rate-limiting tests
// (packages/miniflare/test/plugins/ratelimit/index.spec.ts). Each test
// first moves the clock to the start of a minute, so its calls fall in one
// window of both periods.

import assert from 'node:assert';

async function clock(env, advance) {
  const res = await env.CLOCK.fetch(`http://x/clock?advance=${advance}`, {
    method: 'POST',
  });
  return (await res.json()).now;
}

async function align(env) {
  const now = await clock(env, 0);
  await clock(env, 60_000 - (now % 60_000));
}

const ok = async (binding, options = { key: 'k' }) =>
  (await binding.limit(options)).success;

export const limited = {
  async test(ctrl, env) {
    await align(env);
    assert.strictEqual(await ok(env.BASIC), true);
    assert.strictEqual(await ok(env.BASIC), true);
    assert.strictEqual(await ok(env.BASIC), false);
    assert.strictEqual(await ok(env.BASIC, { key: 'other' }), true);
  },
};

export const validation = {
  async test(ctrl, env) {
    const cases = [
      ['invalid', 'Error: invalid rate limit options'],
      [{ invalid: 'foo' }, 'Error: bad rate limit options: [invalid]'],
      [{ limit: 'bad' }, 'Error: limit must be a number: bad'],
      [{ period: 'bad' }, 'Error: period must be a number: bad'],
      [{ period: 1 }, 'Error: unsupported period: 1'],
      [
        { delta: 'bad' },
        'TypeError: The value cannot be converted because it is not an integer.',
      ],
      [
        { delta: -1 },
        'TypeError: The value cannot be converted because it is negative ' +
          'and this API expects a positive number.',
      ],
      [
        { delta: 2 ** 32 },
        'TypeError: Value out of range. Must be less than or equal to ' +
          '4294967295.',
      ],
      [{ delta: 0 }, 'RangeError: delta must be non-zero'],
      [{ delta: 0.5 }, 'RangeError: delta must be non-zero'],
      [
        { key: 'a', delta: 2n },
        'TypeError: Cannot convert a BigInt value to a number',
      ],
    ];
    for (const [options, error] of cases) {
      await assert.rejects(env.VALIDATE.limit(options), (e) => {
        assert.strictEqual(String(e), error);
        return true;
      });
    }
  },
};

export const weighted = {
  async test(ctrl, env) {
    await align(env);
    const call = (key, delta) => ok(env.WEIGHTED, { key, delta });
    assert.strictEqual(await call('a', 2), true);
    assert.strictEqual(await call('a', 2), true);
    assert.strictEqual(await call('a'), true);
    assert.strictEqual(await call('a'), false);
    // Decided on the count before the call; a failed call adds nothing.
    assert.strictEqual(await call('b', 4), true);
    assert.strictEqual(await call('b', 100), true);
    assert.strictEqual(await call('b', 1), false);
  },
};

export const sharedNamespace = {
  async test(ctrl, env) {
    await align(env);
    assert.strictEqual(await ok(env.RATE_A), true);
    assert.strictEqual(await ok(env.RATE_B), true);
    assert.strictEqual(await ok(env.RATE_A), false);
    assert.strictEqual(await ok(env.RATE_B), false);
    assert.strictEqual(await ok(env.RATE_C), true);
    assert.strictEqual(await ok(env.RATE_C), true);
    assert.strictEqual(await ok(env.RATE_C), false);
  },
};

export const perPeriod = {
  async test(ctrl, env) {
    await align(env);
    assert.strictEqual(await ok(env.RATE_SLOW), true);
    assert.strictEqual(await ok(env.RATE_FAST), true);
    assert.strictEqual(await ok(env.RATE_SLOW), false);
    assert.strictEqual(await ok(env.RATE_FAST), false);
  },
};

export const nextWindow = {
  async test(ctrl, env) {
    await align(env);
    assert.strictEqual(await ok(env.WINDOW), true);
    assert.strictEqual(await ok(env.WINDOW), false);
    await clock(env, 10_000);
    assert.strictEqual(await ok(env.WINDOW), true);
    assert.strictEqual(await ok(env.WINDOW), false);
  },
};
