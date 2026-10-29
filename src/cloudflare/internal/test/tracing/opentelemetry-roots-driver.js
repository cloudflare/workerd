// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
export default {
  async test(_ctrl, env) {
    for (const [name, binding] of Object.entries(env)) {
      for (const flags of [undefined, '00', '01', '02', '03']) {
        const headers =
          flags === undefined
            ? {}
            : {
                traceparent: `00-11111111111111111111111111111111-2222222222222222-${flags}`,
                tracestate: 'upstream=1',
              };
        const r = await binding.fetch('http://worker/run', { headers });
        assert.strictEqual(r.status, 200);
        const result = await r.json();
        const untraced = name === 'UNTRACED';
        const sampled = flags !== '00' && flags !== '02';
        assert.strictEqual(result.recording, !untraced && sampled);
        if (untraced) {
          if (flags === undefined)
            assert.strictEqual(result.invocation, undefined);
          assert.strictEqual(result.root.traceId, '0'.repeat(32));
          assert.strictEqual(result.echo.traceparent, undefined);
          assert.strictEqual(result.childEcho.traceparent, undefined);
        } else {
          assert.notStrictEqual(result.root.traceId, result.invocation.traceId);
          assert.match(result.root.traceId, /^[0-9a-f]{32}$/);
          assert.strictEqual(
            result.root.traceFlags,
            flags === undefined
              ? name === 'ON'
                ? 1
                : 0
              : sampled
                ? Number.parseInt(flags, 16)
                : 0
          );
          assert.strictEqual(
            result.echo.traceparent.split('-')[1],
            result.root.traceId
          );
          assert.strictEqual(
            result.echo.traceparent.split('-')[3],
            result.root.traceFlags.toString(16).padStart(2, '0')
          );
        }
        assert.strictEqual(result.echo.tracestate, undefined);
        assert.strictEqual(result.childEcho.tracestate, undefined);
        if (!untraced)
          assert.strictEqual(
            result.childEcho.traceparent.split('-')[1],
            result.root.traceId
          );
      }
    }
  },
};
