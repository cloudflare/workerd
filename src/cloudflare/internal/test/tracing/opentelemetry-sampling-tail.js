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
    // Only the explicitly unsampled call may carry flags. The JS-only sampled bit must not.
    assert.ok(
      onset.spanContext.traceFlags === undefined ||
        onset.spanContext.traceFlags === 0
    );
    onsets.push(onset);
    const collect = collector.tailStream(onset);
    return (event) => {
      assert.ok(
        event.spanContext.traceFlags === undefined ||
          event.spanContext.traceFlags === 0
      );
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
    assert.ok(logs.some((message) => message.includes('sampling-rpc-log')));
    assert.ok(
      logs.some((message) => message.includes('sampling-websocket-log'))
    );
  },
};
