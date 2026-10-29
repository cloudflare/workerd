// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { createTailStreamCollector } from 'instrumentation-test-helper';

const collector = createTailStreamCollector();
export default { tailStream: collector.tailStream };

export const validate = {
  async test() {
    await collector.waitForCompletion();
    const spans = [...collector.spans.entries()];
    assert.equal(spans.length, 7);
    assert.deepEqual(
      spans.map(([, span]) => span['http.response.status_code']),
      [200n, 399n, 400n, 404n, 409n, 503n, 599n]
    );
    for (const [key, span] of spans) {
      assert.equal(span.name, 'fetch');
      assert.equal(span.closed, true);
      const updates = collector.spanUpdates
        .filter(
          ({ invocationId, spanContext }) =>
            `${invocationId}#${spanContext.spanId}` === key
        )
        .map(({ event }) => event.info);
      if (span['http.response.status_code'] >= 400n) {
        assert.equal(
          span['error.type'],
          String(span['http.response.status_code'])
        );
        assert.deepEqual(updates, [
          { type: 'status', status: { code: 'error' } },
        ]);
      } else {
        assert.equal(span['error.type'], undefined);
        assert.deepEqual(updates, []);
      }
    }
  },
};
