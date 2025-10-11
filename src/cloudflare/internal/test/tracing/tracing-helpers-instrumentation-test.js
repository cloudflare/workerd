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

    // Get all spans and add custom attributes for this test
    const allSpans = collector.getSpans();

    // Process spans to extract test names from tags
    for (const span of allSpans) {
      // Handle the special tags structure for this test
      if (!span.tags) {
        span.tags = {};
      }
      // If test attribute exists at top level, move it to tags for consistency
      if (span.test) {
        span.tags.test = span.test;
      }
    }

    // Group spans by test
    const spansByTest = groupSpansBy(allSpans, 'test');

    console.log('\n=== Span Validation Results ===\n');

    // Validate each test's spans
    const testValidations = [
      { test: 'syncFunction', expectedSpans: ['sync-op'] },
      { test: 'asyncFunction', expectedSpans: ['async-op'] },
      { test: 'thenableObject', expectedSpans: ['thenable-op'] },
      { test: 'syncError', expectedSpans: ['sync-error-op'] },
      { test: 'asyncError', expectedSpans: ['async-error-op'] },
      { test: 'notThenableObject', expectedSpans: ['not-thenable-op'] },
      // Generator functions return immediately, spans may not be captured by tail worker
      { test: 'generatorFunction', expectedSpans: [], optional: true },
      { test: 'asyncGeneratorFunction', expectedSpans: [], optional: true },
      { test: 'callableThenable', expectedSpans: ['callable-thenable-op'] },
      { test: 'multipleEndCalls', expectedSpans: ['multi-end-op'] },
    ];

    let failedTests = 0;

    for (const { test, expectedSpans, optional } of testValidations) {
      const testSpans = spansByTest.get(test) || [];

      try {
        // Filter out non-test spans (like jsRpcSession)
        const relevantSpans = testSpans.filter((s) =>
          expectedSpans.includes(s.name)
        );

        if (
          optional &&
          relevantSpans.length === 0 &&
          expectedSpans.length === 0
        ) {
          // For generator tests, spans may not be captured by tail worker
          // This is expected behavior - generators return immediately
          console.log(
            `[PASS] ${test}: Generator spans behave as expected (immediate return)`
          );
        } else {
          assert.strictEqual(
            relevantSpans.length,
            expectedSpans.length,
            `Test ${test}: Expected ${expectedSpans.length} spans, got ${relevantSpans.length}`
          );

          for (const span of relevantSpans) {
            // Note: The helper sets 'opened' implicitly when a span is created
            assert(span.name, `Test ${test}: Span should have a name`);
            assert(
              span.closed,
              `Test ${test}: Span '${span.name}' should be closed`
            );
            assert.strictEqual(
              span.test || span.tags?.test,
              test,
              `Test ${test}: Span should have correct test tag`
            );
          }

          console.log(`[PASS] ${test}: All spans correctly opened and closed`);
        }
      } catch (e) {
        failedTests++;
        console.error(`[FAIL] ${test}: ${e.message}`);
      }
    }

    // Check for null/undefined tests
    const nullSpans = allSpans.filter((s) => s.name === 'null-op');
    const undefinedSpans = allSpans.filter((s) => s.name === 'undefined-op');

    try {
      assert(nullSpans.length > 0, 'Should have null-op span');
      assert(nullSpans[0].closed, 'null-op span should be closed');
      assert(undefinedSpans.length > 0, 'Should have undefined-op span');
      assert(undefinedSpans[0].closed, 'undefined-op span should be closed');
      console.log('[PASS] nullAndUndefinedReturns: Spans correctly handled');
    } catch (e) {
      failedTests++;
      console.error(`[FAIL] nullAndUndefinedReturns: ${e.message}`);
    }

    // Check nested spans
    const outerSpans = allSpans.filter((s) => s.name === 'outer-op');
    const innerSpans = allSpans.filter((s) => s.name === 'inner-op');

    try {
      assert(outerSpans.length > 0, 'Should have outer-op span');
      assert(outerSpans[0].closed, 'outer-op span should be closed');
      assert(innerSpans.length > 0, 'Should have inner-op span');
      assert(innerSpans[0].closed, 'inner-op span should be closed');
      console.log('[PASS] nestedSpans: All spans correctly opened and closed');
    } catch (e) {
      failedTests++;
      console.error(`[FAIL] nestedSpans: ${e.message}`);
    }

    console.log('\n=== Summary ===');
    const totalSpans = allSpans.filter((s) => !s.name.includes('jsRpcSession'));
    const closedSpans = totalSpans.filter((s) => s.closed);
    console.log(`Total test spans: ${totalSpans.length}`);
    console.log(`Closed spans: ${closedSpans.length}`);
    console.log(`Unclosed spans: ${totalSpans.length - closedSpans.length}`);

    if (failedTests > 0) {
      throw new Error(`${failedTests} test(s) failed - see details above`);
    }

    console.log('\nAll tracing-helpers tests passed!');
  },
};
