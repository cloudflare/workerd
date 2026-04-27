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
export default collector;

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
      { test: 'publicImportTracing', expectedSpan: 'public-import-op' },
      { test: 'ctxTracing', expectedSpan: 'ctx-tracing-op' },
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
