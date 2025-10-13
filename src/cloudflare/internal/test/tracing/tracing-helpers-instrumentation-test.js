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
    const allSpans = collector.getSpans();
    const spansByTest = groupSpansBy(allSpans, 'test');

    // Define the core tests that validate withSpan behavior
    const testValidations = [
      { test: 'syncFunction', expectedSpan: 'sync-op' },
      { test: 'asyncFunction', expectedSpan: 'async-op' },
      { test: 'syncError', expectedSpan: 'sync-error-op' },
      { test: 'asyncError', expectedSpan: 'async-error-op' },
    ];

    // Validate each test's span
    for (const { test, expectedSpan } of testValidations) {
      const testSpans = spansByTest.get(test) || [];
      const span = testSpans.find((s) => s.name === expectedSpan);

      assert(span, `${test}: Should have created span '${expectedSpan}'`);
      assert(span.closed, `${test}: Span '${expectedSpan}' should be closed`);
    }

    console.log('All tracing-helpers tests passed!');
  },
};
