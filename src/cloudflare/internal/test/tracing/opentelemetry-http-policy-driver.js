// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
const traceId = 'a3ce929d0e0e47364bf92f3577b34da6';
const parentId = 'c0ffee0ba902b7aa';
const parent = (flags, version = '00') =>
  `${version}-${traceId}-${parentId}-${flags}`;
const call = (binding, path, headers) =>
  binding.fetch(`http://worker${path}`, { headers }).then((r) => r.json());

export default {
  async test(ctrl, env) {
    for (const binding of [env.ON, env.UNTRACED, env.BUFFERED]) {
      for (const flags of ['00', '01', '03', 'ff']) {
        const result = await call(binding, '/forward', {
          traceparent: parent(flags),
          tracestate: 'upstream=1',
        });
        assert.strictEqual(result.active.traceId, traceId);
        assert.strictEqual(
          result.active.traceFlags,
          Number.parseInt(flags, 16) & 3
        );
        assert.strictEqual(result.active.state, 'upstream=1');
        assert.strictEqual(result.echo.traceparent.split('-')[1], traceId);
        if (binding === env.UNTRACED) {
          assert.strictEqual(result.active.spanId, parentId);
          assert.strictEqual(result.echo.traceparent.split('-')[2], parentId);
        } else
          assert.notStrictEqual(
            result.echo.traceparent.split('-')[2],
            parentId
          );
        assert.strictEqual(
          result.echo.traceparent.split('-')[3],
          flags === 'ff' ? '03' : flags
        );
        assert.strictEqual(result.echo.tracestate, 'upstream=1');
        assert.strictEqual(result.original, parent(flags));
        const fresh = await call(binding, '/fresh', {
          traceparent: parent(flags),
          tracestate: 'upstream=1',
        });
        assert.strictEqual(fresh.echo.traceparent.split('-')[1], traceId);
        if (binding === env.UNTRACED) {
          assert.strictEqual(fresh.echo.traceparent.split('-')[2], parentId);
        }
        assert.strictEqual(
          fresh.echo.traceparent.split('-')[3],
          flags === 'ff' ? '03' : flags
        );
        assert.strictEqual(fresh.echo.tracestate, 'upstream=1');
      }
    }
    const buffered = await call(env.BUFFERED, '/fresh');
    assert.strictEqual(buffered.echo.traceparent.split('-')[3], '00');
    await call(env.ON, '/platform-unsampled', { traceparent: parent('00') });
    await call(env.ON, '/state-multi', [
      ['traceparent', parent('01')],
      ['tracestate', 'upstream=1'],
      ['tracestate', 'other=2'],
    ]);
    const future = await call(env.ON, '/fresh', {
      traceparent: parent('ff', '01') + '-extension',
    });
    assert.strictEqual(future.active.traceFlags, 3);
    assert.strictEqual(future.echo.traceparent.split('-')[3], '03');
    const duplicate = await call(env.ON, '/fresh', [
      ['traceparent', parent('01')],
      ['traceparent', parent('01')],
    ]);
    assert.notStrictEqual(duplicate.active.traceId, traceId);
    for (const path of [
      '/clone',
      '/override',
      '/redirect',
      '/exclude',
      '/suppress',
      '/state-foreign',
      '/state-drop',
      '/state-updated',
      '/state-custom',
      '/nested-sampling',
      '/malformed-context',
    ]) {
      await call(env.ON, path, {
        traceparent: parent('01'),
        tracestate: 'upstream=1',
      });
    }
    // No HTTP parent: only the native binding trigger reaches the no-tail callee.
    await call(env.ON, '/native-untraced');
    // The buffered caller manually propagates its JS projection (-00) to a streaming callee.
    const mixed = await call(env.BUFFERED, '/mixed');
    assert.strictEqual(mixed.recording, true);
    for (const [state, expected] of [
      [' , vendor=one,,other=two, ', 'vendor=one,other=two'],
      [Array.from({ length: 32 }, (_, i) => `v${i}=a`).join(','), undefined],
      [Array.from({ length: 33 }, (_, i) => `v${i}=a`).join(','), null],
      ['a=' + 'x'.repeat(255) + ',b=' + 'x'.repeat(254), null],
    ]) {
      const result = await call(env.ON, '/fresh', {
        traceparent: parent('01'),
        tracestate: state,
      });
      assert.strictEqual(
        result.active.state ?? null,
        expected === undefined ? state : expected
      );
    }
  },
};
