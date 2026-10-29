// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  createHierarchyAwareCollector,
  findSpanByName,
} from 'instrumentation-test-helper';

const collector = createHierarchyAwareCollector();
const logs = [];
const onsets = [];
export default {
  tailStream(onset) {
    // Includes both flag states, service-binding/RPC triggers and hibernatable WebSocket turns.
    // Only this explicitly unsampled invocation may carry 00, never a projected JS 0 or 1.
    const expectedFlags =
      onset.event.info.type === 'fetch' &&
      new URL(onset.event.info.url).pathname === '/unsampled'
        ? 0
        : undefined;
    assert.strictEqual(onset.spanContext.traceFlags, expectedFlags);
    if (expectedFlags === 0) {
      assert.strictEqual(
        onset.spanContext.traceId,
        '11111111111111111111111111111111'
      );
    }
    onsets.push(onset);
    const collect = collector.tailStream(onset);
    return (event) => {
      assert.strictEqual(event.spanContext.traceFlags, expectedFlags);
      if (event.event.type === 'log') logs.push(event.event.message);
      collect(event);
    };
  },
};

export const exportedContexts = {
  async test() {
    await collector.waitForCompletion();
    const first = findSpanByName(collector.state, 'sampling-rebuilt-0');
    const parent = findSpanByName(
      collector.state,
      'sampling-native',
      (span) => span.invocationId === first.invocationId
    );
    assert.ok(parent);
    assert.ok(
      ![...collector.state.spans.values()].some((span) =>
        span.name.startsWith('sampling-orphan')
      )
    );
    for (let i = 0; i < 9; i++) {
      const child = findSpanByName(collector.state, `sampling-rebuilt-${i}`);
      assert.ok(child);
      assert.strictEqual(child.parentSpanId, parent.spanId);
    }
    assert.ok(onsets.length >= 6);
    assert.strictEqual(
      onsets.filter(
        (onset) =>
          onset.event.info.type === 'fetch' &&
          new URL(onset.event.info.url).pathname.startsWith(
            '/sampling-buffered-'
          )
      ).length,
      4
    );
    for (const kind of ['active', 'rebuilt']) {
      assert.ok(
        findSpanByName(
          collector.state,
          `sampling-callee/sampling-buffered-${kind}-on`
        )
      );
      assert.ok(
        ![...collector.state.spans.values()].some(
          (span) =>
            span.name === `sampling-callee/sampling-buffered-${kind}-off`
        )
      );
    }
    assert.ok(logs.some((message) => message.includes('sampling-rpc-log')));
    assert.ok(
      logs.some((message) => message.includes('sampling-websocket-log'))
    );
  },
};
