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

// After all test cases above have run (triggering tailStream invocations per-request),
// validate the parent/child relationships encoded in the streaming-tail events.
export const validateHierarchy = {
  async test() {
    await collector.waitForCompletion();
    const { state } = collector;

    // Assert an edge in the span tree: `child` is a direct child of `parent`.
    const assertParent = (child, parent, label) => {
      assert.strictEqual(
        child.parentSpanId,
        parent.spanId,
        `${label}: expected span "${child.name}" (id=${child.spanId}) to have ` +
          `parent "${parent.name}" (id=${parent.spanId}), but got parentSpanId=${child.parentSpanId}`
      );
      assert.strictEqual(
        child.invocationId,
        parent.invocationId,
        `${label}: child and parent should share invocationId`
      );
    };

    // Assert `span` is a direct child of the top-level (onset) span for its invocation.
    const assertTopLevelParent = (span, label) => {
      const topLevelSpanId = state.topLevelSpans.get(span.invocationId);
      assert.ok(
        topLevelSpanId,
        `${label}: missing topLevelSpanId for invocation ${span.invocationId}`
      );
      assert.strictEqual(
        span.parentSpanId,
        topLevelSpanId,
        `${label}: expected span "${span.name}" to be a direct child of the onset ` +
          `span (id=${topLevelSpanId}), but parentSpanId=${span.parentSpanId}`
      );
    };

    // ---------- Case 1: nestedEnterSpan (sync) ----------
    {
      const outer = findSpanByName(state, 'hierarchy-outer');
      const inner = findSpanByName(state, 'hierarchy-inner');
      assert.strictEqual(outer.role, 'outer');
      assert.strictEqual(inner.role, 'inner');
      assert.ok(outer.closed, 'outer sync span should be closed');
      assert.ok(inner.closed, 'inner sync span should be closed');
      assertParent(inner, outer, 'nestedEnterSpan');
      assertTopLevelParent(outer, 'nestedEnterSpan');
    }

    // ---------- Case 2: nestedEnterSpanAsync ----------
    // The important assertion here: the inner span is created AFTER an `await`, so its
    // parent is only correctly reported if the AsyncContextFrame push propagates through
    // the microtask boundary.
    {
      const outer = findSpanByName(state, 'hierarchy-async-outer');
      const inner = findSpanByName(state, 'hierarchy-async-inner');
      assert.strictEqual(outer.role, 'outer');
      assert.strictEqual(inner.role, 'inner');
      assert.ok(outer.closed);
      assert.ok(inner.closed);
      assertParent(inner, outer, 'nestedEnterSpanAsync');
      assertTopLevelParent(outer, 'nestedEnterSpanAsync');
    }

    // ---------- Case 3: fetchInsideEnterSpan ----------
    // The runtime-created "fetch" span must be nested under our enterSpan. This test
    // is the key proof that enterSpan's AsyncContextFrame push is observed by lower-level
    // runtime tracing paths (IoContext::makeUserTraceSpan -> getCurrentUserTraceSpan).
    {
      const outer = findSpanByName(state, 'hierarchy-fetch-outer');
      assert.strictEqual(outer.case, 'fetchInsideEnterSpan');
      assert.ok(outer.closed);
      // Find the fetch span that belongs to this invocation (there are other fetch
      // spans in other test invocations).
      const fetchSpan = findSpanByName(
        state,
        'fetch',
        (s) => s.invocationId === outer.invocationId
      );
      assertParent(fetchSpan, outer, 'fetchInsideEnterSpan');
      assertTopLevelParent(outer, 'fetchInsideEnterSpan');
    }

    // ---------- Case 4: fetchInsideNestedEnterSpan ----------
    // Three-level chain: onset -> outer -> inner -> fetch.
    {
      const outer = findSpanByName(state, 'hierarchy-deep-outer');
      const inner = findSpanByName(state, 'hierarchy-deep-inner');
      assert.strictEqual(outer.role, 'outer');
      assert.strictEqual(inner.role, 'inner');
      assert.ok(outer.closed);
      assert.ok(inner.closed);
      const fetchSpan = findSpanByName(
        state,
        'fetch',
        (s) => s.invocationId === outer.invocationId
      );
      assertParent(inner, outer, 'fetchInsideNestedEnterSpan (inner->outer)');
      assertParent(
        fetchSpan,
        inner,
        'fetchInsideNestedEnterSpan (fetch->inner)'
      );
      assertTopLevelParent(outer, 'fetchInsideNestedEnterSpan');
    }

    // ---------- Case 5: siblingEnterSpans ----------
    // Sequential (non-nested) spans should each hang directly off the onset, not off
    // each other - the first span's AsyncContextFrame push must be fully popped before
    // the second span opens.
    {
      const a = findSpanByName(state, 'hierarchy-sibling-a');
      const b = findSpanByName(state, 'hierarchy-sibling-b');
      assert.strictEqual(a.role, 'sibling-a');
      assert.strictEqual(b.role, 'sibling-b');
      assert.ok(a.closed);
      assert.ok(b.closed);
      assert.strictEqual(
        a.invocationId,
        b.invocationId,
        'siblingEnterSpans: both should share the same invocation'
      );
      assertTopLevelParent(a, 'siblingEnterSpans (a)');
      assertTopLevelParent(b, 'siblingEnterSpans (b)');
    }

    // ---------- Case 6: abandonedPromiseSpan ----------
    // Reaching this point proves the outcome event for the abandoned-promise invocation
    // was emitted in order: invocationPromises only resolve on "outcome", and
    // waitForCompletion() awaits them all. BaseTracer::WeakRef prevents the abandoned
    // SpanImpl from pinning the tracer past end-of-request.

    console.log('All tracing-hierarchy tests passed!');
  },
};
