// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  createHierarchyAwareCollector,
  findSpanByName,
} from 'instrumentation-test-helper';
const collector = createHierarchyAwareCollector();
const traceIds = new Map();
const exceptions = [];
export default {
  tailStream(onset) {
    const collect = collector.tailStream(onset);
    return (event) => {
      if (event.event.type === 'spanOpen')
        traceIds.set(event.event.name, event.spanContext.traceId);
      if (event.event.type === 'exception') exceptions.push(event.event);
      collect(event);
    };
  },
};
export const hierarchy = {
  async test() {
    await collector.waitForCompletion();
    const span = (name) => findSpanByName(collector.state, name);
    assert.strictEqual(
      span('otel-child').parentSpanId,
      span('otel-parent').spanId
    );
    assert.strictEqual(
      span('otel-native-child').parentSpanId,
      span('otel-native-parent').spanId
    );
    const remote = span('otel-remote-child');
    assert.strictEqual(remote.parentSpanId, '2222222222222222');
    assert.strictEqual(
      traceIds.get('otel-remote-child'),
      '11111111111111111111111111111111'
    );
    for (const name of ['otel-parent', 'otel-remote-child']) {
      const parent = span(name);
      const fetch = findSpanByName(
        collector.state,
        'fetch',
        (f) => f.invocationId === parent.invocationId
      );
      assert.strictEqual(fetch.parentSpanId, parent.spanId);
    }
    for (const name of ['otel-new-root', 'otel-deleted-root']) {
      assert.strictEqual(span(name).parentSpanId, undefined);
    }
    assert.ok(!traceIds.has('otel-unsampled'));
    assert.ok(!traceIds.has('otel-unsampled-child'));
    assert.ok(
      exceptions.some(
        (event) => event.name === 'E_CODE' && event.message === 'coded'
      )
    );
    assert.ok(exceptions.some((event) => event.name === '503'));
  },
};
