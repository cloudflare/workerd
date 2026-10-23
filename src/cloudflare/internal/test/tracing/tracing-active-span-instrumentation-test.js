// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  createHierarchyAwareCollector,
  findSpanByName,
} from 'instrumentation-test-helper';

const collector = createHierarchyAwareCollector();
export default { tailStream: collector.tailStream };

// After the scenarios in tracing-active-span-test.js have run, validate the parent/child
// relationships encoded in the streaming-tail events.
export const validateHierarchy = {
  async test() {
    await collector.waitForCompletion();
    const { state } = collector;

    const span = (name) => findSpanByName(state, name);
    const fetchSpanFor = (s) =>
      findSpanByName(state, 'fetch', (f) => f.invocationId === s.invocationId);
    const assertParent = (child, parent) => {
      assert.strictEqual(
        child.parentSpanId,
        parent.spanId,
        `expected "${child.name}" to be a child of "${parent.name}"`
      );
      assert.strictEqual(child.invocationId, parent.invocationId);
    };
    const assertTopLevel = (s) => {
      assert.strictEqual(
        s.parentSpanId,
        state.topLevelSpans.get(s.invocationId),
        `expected "${s.name}" to be a child of the invocation span`
      );
    };

    // activeSpanIdentity: startSpan() inside the callback uses the activated span as parent.
    assertTopLevel(span('was-identity'));
    assertParent(span('was-identity-start-span-child'), span('was-identity'));
    assert.ok(span('was-identity').closed);

    // fetchInsideWithActiveSpan: built-in instrumentation nests under the activated span.
    assertParent(
      fetchSpanFor(span('was-fetch-parent')),
      span('was-fetch-parent')
    );

    // asyncContinuation: the activated span survives an await.
    assertParent(span('was-async-child'), span('was-async-parent'));
    assertParent(
      fetchSpanFor(span('was-async-parent')),
      span('was-async-parent')
    );

    // nestedWithActiveSpan: the innermost call wins, and the outer span is restored afterwards.
    assertTopLevel(span('was-nested-outer'));
    assertTopLevel(span('was-nested-inner'));
    assertParent(span('was-nested-inner-child'), span('was-nested-inner'));
    assertParent(span('was-nested-outer-child'), span('was-nested-outer'));

    // overridesEnclosingSpan: withActiveSpan replaces the enclosing enterSpan as the parent.
    assertTopLevel(span('was-override-target'));
    assertParent(span('was-override-child'), span('was-override-target'));

    // endedSpanRemainsParent: an ended span can still parent spans created later.
    assert.ok(span('was-ended-parent').closed);
    assertParent(span('was-ended-child'), span('was-ended-parent'));

    // invocationSpanResetsNesting: activating the invocation span escapes the enclosing span.
    assertTopLevel(span('was-invocation-child'));

    // noopSpanKeepsEnclosingParent: a no-op span does not suppress or move child spans.
    assertParent(span('was-noop-child'), span('was-noop-enclosing'));
  },
};
