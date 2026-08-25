// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  createTailStreamCollector,
  groupSpansBy,
} from 'instrumentation-test-helper';

// Create the collector and export it for the tail worker
const collector = createTailStreamCollector();
const rootAttributes = [];
const overlappingRequests = new Map();
export default {
  ...collector,
  tailStream(onset, env, ctx) {
    const handleEvent = collector.tailStream(onset, env, ctx);
    const isOverlappingRequest =
      onset.event.executionModel === 'durableObject' &&
      onset.event.entrypoint === 'OverlappingRequestsObject';
    const overlappingRequest = isOverlappingRequest
      ? { rootSpanId: onset.event.spanId, attributes: [] }
      : undefined;
    if (overlappingRequest) {
      overlappingRequests.set(
        new URL(onset.event.info.url).pathname.slice(1),
        overlappingRequest
      );
    }
    return (event) => {
      if (event.event.type === 'attributes' && overlappingRequest) {
        for (const attribute of event.event.info) {
          if (attribute.name === 'overlapping.request') {
            overlappingRequest.attributes.push({
              spanId: event.spanContext.spanId,
              value: attribute.value,
            });
          }
        }
      }
      if (
        event.event.type === 'attributes' &&
        event.spanContext.spanId === onset.event.spanId
      ) {
        rootAttributes.push(...event.event.info);
        return;
      }
      return handleEvent(event);
    };
  },
};

// After all tests complete, validate the spans
export const validateSpans = {
  async test() {
    // Wait for all the tailStream executions to finish
    await collector.waitForCompletion();

    // Get all spans and prepare for validation
    const allSpans = collector.spans.values();
    const spansByTest = groupSpansBy(allSpans, 'test');

    // Core tests that validate withSpan produces a correctly-closed span of the given name.
    const testValidations = [
      { test: 'syncFunction', expectedSpan: 'sync-op' },
      { test: 'asyncFunction', expectedSpan: 'async-op' },
      { test: 'syncError', expectedSpan: 'sync-error-op' },
      { test: 'asyncError', expectedSpan: 'async-error-op' },
      { test: 'spanClassName', expectedSpan: 'class-name-op' },
      { test: 'isTraced', expectedSpan: 'is-traced-op' },
      {
        test: 'setAttributeUndefined',
        expectedSpan: 'undefined-attr-op',
      },
      { test: 'setAttributes', expectedSpan: 'set-attributes-op' },
      { test: 'publicImportTracing', expectedSpan: 'public-import-op' },
      {
        test: 'publicImportStartActiveSpan',
        expectedSpan: 'public-start-active-op',
      },
      {
        test: 'publicImportStartSpan',
        expectedSpan: 'public-start-span-op',
      },
      { test: 'getActiveSpan', expectedSpan: 'get-active-span-op' },
      { test: 'ctxTracing', expectedSpan: 'ctx-tracing-op' },
      {
        test: 'detachedSpanEndsAfterStreamDrain',
        expectedSpan: 'detached-stream-op',
      },
      { test: 'helperStartActiveSpan', expectedSpan: 'helper-detached-op' },
      { test: 'startActiveSpanSyncThrow', expectedSpan: 'manual-throw-op' },
    ];

    for (const { test, expectedSpan } of testValidations) {
      const testSpans = spansByTest.get(test) || [];
      const span = testSpans.find((s) => s.name === expectedSpan);

      assert(span, `${test}: Should have created span '${expectedSpan}'`);
      assert(span.closed, `${test}: Span '${expectedSpan}' should be closed`);
    }

    // setAttributeUndefined should NOT have a 'skipped' attribute recorded.
    {
      const span = (spansByTest.get('setAttributeUndefined') || []).find(
        (s) => s.name === 'undefined-attr-op'
      );
      assert(span, 'setAttributeUndefined: span present');
      assert(
        !('skipped' in span),
        'setAttribute(key, undefined) should not record the attribute'
      );
    }

    {
      const span = (
        spansByTest.get('detachedSpanEndsAfterStreamDrain') || []
      ).find((s) => s.name === 'detached-stream-op');
      assert(span, 'detachedSpanEndsAfterStreamDrain: span present');
      assert.strictEqual(span['phase.created'], true);
      assert.strictEqual(span['phase.drained'], true);
      assert(span.closed, 'Detached stream span should be explicitly closed');
    }

    {
      const span = (spansByTest.get('publicImportStartActiveSpan') || []).find(
        (s) => s.name === 'public-start-active-op'
      );
      assert(span, 'publicImportStartActiveSpan: span present');
      assert.strictEqual(span.path, 'import-from-cloudflare-workers');
      assert.strictEqual(span['ended.explicitly'], true);
      assert(span.closed, 'Public startActiveSpan span should be closed');
    }

    {
      const span = (spansByTest.get('publicImportStartSpan') || []).find(
        (s) => s.name === 'public-start-span-op'
      );
      assert(span, 'publicImportStartSpan: span present');
      assert.strictEqual(span.path, 'import-from-cloudflare-workers');
      assert(span.closed, 'Public startSpan span should be closed');
    }

    {
      const span = (spansByTest.get('helperStartActiveSpan') || []).find(
        (s) => s.name === 'helper-detached-op'
      );
      assert(span, 'helperStartActiveSpan: span present');
      assert.strictEqual(span['ended.explicitly'], true);
      assert(span.closed, 'Helper-created span should be explicitly closed');
    }

    {
      const span = (spansByTest.get('startActiveSpanSyncThrow') || []).find(
        (s) => s.name === 'manual-throw-op'
      );
      assert(span, 'startActiveSpanSyncThrow: span present');
      assert.strictEqual(span['after.throw'], true);
      assert(span.closed, 'Manual throw span should be explicitly closed');
    }

    assert.deepStrictEqual(
      rootAttributes.find(({ name }) => name === 'test'),
      { name: 'test', value: 'getActiveSpanInvocation' }
    );

    for (const requestName of ['a', 'b']) {
      const request = overlappingRequests.get(requestName);
      assert(
        request,
        `Missing tail trace for overlapping request ${requestName}`
      );
      assert.deepStrictEqual(
        request.attributes,
        [{ spanId: request.rootSpanId, value: requestName }],
        JSON.stringify([...overlappingRequests])
      );
    }

    // setAttributes should record each supported value and ignore undefined values.
    {
      const span = (spansByTest.get('setAttributes') || []).find(
        (s) => s.name === 'set-attributes-op'
      );
      assert(span, 'setAttributes: span present');
      assert.strictEqual(span.stringValue, 'value');
      assert.strictEqual(span.numberValue, 42);
      assert.strictEqual(span.booleanValue, true);
      assert(
        !('skipped' in span),
        'setAttributes should ignore undefined values'
      );
    }

    // Nested spans: verify both outer and inner spans exist and both are closed.
    // This exercises the AsyncContextFrame push path used by enterSpan for nesting.
    for (const testName of ['nestedSyncSpans', 'nestedAsyncSpans']) {
      const testSpans = spansByTest.get(testName) || [];
      const outerName =
        testName === 'nestedSyncSpans'
          ? 'nested-outer-op'
          : 'nested-async-outer-op';
      const innerName =
        testName === 'nestedSyncSpans'
          ? 'nested-inner-op'
          : 'nested-async-inner-op';

      const outer = testSpans.find((s) => s.name === outerName);
      const inner = testSpans.find((s) => s.name === innerName);

      assert(outer, `${testName}: outer span '${outerName}' should exist`);
      assert(inner, `${testName}: inner span '${innerName}' should exist`);
      assert(outer.closed, `${testName}: outer span should be closed`);
      assert(inner.closed, `${testName}: inner span should be closed`);
      assert.strictEqual(outer.level, 'outer');
      assert.strictEqual(inner.level, 'inner');
    }

    console.log('All tracing-helpers tests passed!');
  },
};
