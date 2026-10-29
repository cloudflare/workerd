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
const events = [];
const onsets = new Map();
export default {
  tailStream(onset) {
    assert.strictEqual(onset.spanContext.traceFlags, undefined);
    onsets.set(onset.invocationId, onset);
    const collect = collector.tailStream(onset);
    return (event) => {
      assert.strictEqual(
        event.invocationId,
        onset.invocationId,
        'event must use its owning invocation stream'
      );
      events.push(event);
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
    for (const name of [
      'otel-new-root',
      'otel-deleted-root',
      'otel-root-context',
    ]) {
      const child = span(name);
      const onset = onsets.get(child.invocationId);
      assert.strictEqual(child.parentSpanId, onset.event.spanId);
      assert.strictEqual(traceIds.get(name), onset.spanContext.traceId);
    }
    assert.ok(!traceIds.has('otel-unsampled'));
    assert.ok(!traceIds.has('otel-unsampled-child'));
    assert.ok(!traceIds.has('otel-unsampled-native-child'));
    assert.ok(
      exceptions.some(
        (event) =>
          event.code === 'E_CODE' &&
          event.name === 'Ignored' &&
          event.message === 'coded'
      )
    );
    assert.ok(
      exceptions.some((event) => event.code === 503 && event.name === '')
    );
    const foreignEvents = events.filter(
      (event) =>
        event.invocationId === remote.invocationId &&
        event.spanContext.spanId === remote.spanId
    );
    for (const event of foreignEvents) {
      assert.strictEqual(
        event.spanContext.traceId,
        '11111111111111111111111111111111'
      );
    }
    const log = foreignEvents.find((event) => event.event.type === 'log');
    assert.ok(log);
    assert.ok(log.event.message.includes('foreign-log'));
    assert.ok(
      foreignEvents.some((event) => event.event.type === 'diagnosticChannel')
    );
    assert.ok(
      foreignEvents.some(
        (event) =>
          event.event.type === 'exception' &&
          event.event.name === 'ForeignError'
      )
    );
    assert.ok(foreignEvents.some((event) => event.event.type === 'spanClose'));
    assert.strictEqual(remote['foreign.attribute'], 'value');
    const updates = foreignEvents.filter(
      (event) => event.event.type === 'spanUpdate'
    );
    assert.ok(
      updates.some((event) => event.event.info.name === 'otel-remote-renamed')
    );
    assert.ok(
      updates.some(
        (event) =>
          event.event.info.status?.code === 'error' &&
          event.event.info.status.message === 'foreign failure'
      )
    );
    // Verify all components of every span, including built-in descendants, use its trace ID.
    const identities = new Map(
      events
        .filter((event) => event.event.type === 'spanOpen')
        .map((event) => [
          `${event.invocationId}#${event.event.spanId}`,
          event.spanContext.traceId,
        ])
    );
    for (const event of events) {
      const traceId = identities.get(
        `${event.invocationId}#${event.spanContext.spanId}`
      );
      if (traceId !== undefined)
        assert.strictEqual(event.spanContext.traceId, traceId);
      if (event.event.type === 'spanOpen') {
        assert.ok(event.spanContext.spanId);
      }
    }
    for (const name of ['a', 'b']) {
      const owner = events.find(
        (event) =>
          event.event.type === 'log' &&
          event.event.message.includes(`otel-do-owner-${name}`)
      );
      assert.ok(owner);
      for (const kind of ['root', 'remote']) {
        assert.strictEqual(
          span(`otel-do-${kind}-${name}`).invocationId,
          owner.invocationId
        );
      }
      const childLog = events.find(
        (event) =>
          event.event.type === 'log' &&
          event.event.message.includes(`otel-do-child-${name}`)
      );
      assert.strictEqual(childLog.invocationId, owner.invocationId);
      assert.strictEqual(
        childLog.spanContext.traceId,
        '11111111111111111111111111111111'
      );
    }
  },
};
