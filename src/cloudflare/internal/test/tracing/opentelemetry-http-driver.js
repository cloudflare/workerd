// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
const traceId = 'a3ce929d0e0e47364bf92f3577b34da6';
const spanId = 'c0ffee0ba902b7aa';
const state = 'upstream=1,vendor=value';
const parent = (flags) => `00-${traceId}-${spanId}-${flags}`;

export default {
  async test(ctrl, env) {
    for (const [binding, enabled] of [
      [env.ON, true],
      [env.OFF, false],
    ]) {
      for (const flags of ['01', '00', '03']) {
        const result = await (
          await binding.fetch(`http://worker/valid-${flags}`, {
            headers: { traceparent: parent(flags), tracestate: state },
          })
        ).json();
        assert.strictEqual(result.enabled, enabled);
        if (enabled) {
          assert.strictEqual(result.active.traceId, traceId);
          assert.strictEqual(
            result.active.traceFlags,
            Number.parseInt(flags, 16)
          );
          assert.strictEqual(result.active.traceState, state);
          assert.strictEqual(result.childRecording, flags !== '00');
          assert.strictEqual(result.echo.traceparent.split('-')[1], traceId);
          assert.strictEqual(result.echo.traceparent.split('-')[3], flags);
          assert.strictEqual(result.echo.tracestate, state);
        } else {
          assert.notStrictEqual(result.active.traceId, traceId);
          assert.strictEqual(result.active.traceFlags, 0);
          assert.strictEqual(result.echo.traceparent, undefined);
          assert.strictEqual(result.echo.tracestate, undefined);
        }
      }
    }
    for (const value of [
      'garbage',
      parent('01').toUpperCase(),
      parent('zz'),
      `00-${'0'.repeat(32)}-${spanId}-01`,
      `00-${traceId}-${'0'.repeat(16)}-01`,
      parent('01') + '-extra',
      parent('01').replace(/^00/, 'ff'),
      parent('01') + ',' + parent('01'),
    ]) {
      const result = await (
        await env.ON.fetch('http://worker/invalid', {
          headers: { traceparent: value, tracestate: state },
        })
      ).json();
      assert.notStrictEqual(result.active.traceId, traceId);
      assert.strictEqual(result.active.traceState, undefined);
      assert.strictEqual(result.active.traceFlags, 1);
      assert.strictEqual(result.echo.traceparent.split('-')[3], '01');
    }
    for (const value of [
      'bad',
      'vendor=one,vendor=two',
      'Vendor=value',
      'v=a=b',
      'v=' + 'x'.repeat(257),
    ]) {
      const result = await (
        await env.ON.fetch('http://worker/bad-state', {
          headers: { traceparent: parent('01'), tracestate: value },
        })
      ).json();
      assert.strictEqual(result.active.traceId, traceId);
      assert.strictEqual(result.active.traceState, undefined);
      assert.strictEqual(result.echo.tracestate, undefined);
    }
    const future = await (
      await env.ON.fetch('http://worker/future', {
        headers: {
          traceparent: parent('01').replace(/^00/, '01') + '-extension',
        },
      })
    ).json();
    assert.strictEqual(future.active.traceId, traceId);
    await (await env.ON.fetch('http://worker/no-parent')).text();
    for (const flags of ['00', '01']) {
      const result = await (
        await env.UNTRACED.fetch(`http://worker/pass-${flags}`)
      ).json();
      assert.strictEqual(result.echo.traceparent, parent(flags));
      assert.strictEqual(result.echo.tracestate, 'vendor=pass');
      assert.strictEqual(result.empty.traceparent, undefined);
      assert.strictEqual(result.invalid.traceparent, undefined);
    }
  },
};
