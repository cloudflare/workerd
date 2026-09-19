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
    const invocations = [...collector.invocations.values()];
    const rootAttributes = invocations.flatMap(
      (invocation) => invocation.attributes
    );

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
      { test: 'arrayAttributes', expectedSpan: 'array-attrs-op' },
      {
        test: 'arrayAttributeByteLimit',
        expectedSpan: 'array-limit-strings-op',
      },
      {
        test: 'arrayAttributeByteLimit',
        expectedSpan: 'array-limit-nullish-op',
      },
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
      const request = invocations.find(
        (invocation) =>
          invocation.onset.executionModel === 'durableObject' &&
          invocation.onset.entrypoint === 'OverlappingRequestsObject' &&
          new URL(invocation.onset.info.url).pathname === `/${requestName}`
      );
      assert(
        request,
        `Missing tail trace for overlapping request ${requestName}`
      );
      assert.deepStrictEqual(
        request.attributeEvents
          .filter(({ name }) => name === 'overlapping.request')
          .map(({ spanId, value }) => ({ spanId, value })),
        [{ spanId: request.rootSpanId, value: requestName }]
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

    // Array attributes on a user-created span: arrays arrive as arrays with scalar-vs-array
    // identity preserved, nullish positions retained, and invalid arrays omitted.
    {
      const span = (spansByTest.get('arrayAttributes') || []).find(
        (s) => s.name === 'array-attrs-op'
      );
      assert(span, 'arrayAttributes: span present');

      assert.deepStrictEqual(span.strings, ['a', 'b', 'c']);
      assert.deepStrictEqual(span.numbers, [1, 2.5, -3, 0]);
      assert.deepStrictEqual(span.booleans, [true, false, true]);

      assert.deepStrictEqual(span['single.string'], ['stop']);
      assert.deepStrictEqual(span['single.number'], [42]);
      assert.deepStrictEqual(span['single.boolean'], [false]);

      assert.deepStrictEqual(span.empty, []);

      assert.deepStrictEqual(span['nullish.strings'], [
        null,
        'a',
        null,
        'b',
        null,
      ]);
      assert.deepStrictEqual(span['nullish.numbers'], [null, 1, null, 2]);
      assert.deepStrictEqual(span['nullish.booleans'], [null, true]);
      assert.deepStrictEqual(span['nullish.only'], [null, null]);

      for (const key of [
        'invalid.mixed',
        'invalid.mixedBoolNumber',
        'invalid.mixedAfterNull',
        'invalid.nested',
        'invalid.objects',
        'invalid.bigint',
        'invalid.tooLong',
        'invalid.sparse',
        'set.invalid',
        'set.skipped',
      ]) {
        assert(
          !(key in span),
          `arrayAttributes: '${key}' should not be recorded`
        );
      }

      assert.deepStrictEqual(span['set.strings'], ['x', 'y']);
      assert.deepStrictEqual(span['set.numbers'], [7]);
      assert.deepStrictEqual(span['set.booleans'], [true, null, false]);
      assert.deepStrictEqual(span['set.empty'], []);

      assert.deepStrictEqual(span['overwrite.toArray'], ['a', 'b']);
      assert.strictEqual(span['overwrite.toScalar'], 'scalar');
      assert.deepStrictEqual(span['overwrite.arrayType'], ['one', 'two']);

      // Every array attribute above stayed within the span data limit.
      assert(
        !('cloudflare.warning.type' in span),
        'arrayAttributes: no span data limit warning expected'
      );
    }

    // Array attributes on the invocation span are delivered as attribute events on the root span.
    {
      const invocation = invocations.find((invocation) =>
        invocation.attributes.some(
          ({ name, value }) =>
            name === 'invocation.test' &&
            value === 'invocationSpanArrayAttributes'
        )
      );
      assert(invocation, 'invocationSpanArrayAttributes: invocation present');
      const attributes = new Map(
        invocation.attributes.map(({ name, value }) => [name, value])
      );
      assert.deepStrictEqual(attributes.get('invocation.strings'), ['a', 'b']);
      assert.deepStrictEqual(attributes.get('invocation.numbers'), [1.5, 2]);
      assert.deepStrictEqual(attributes.get('invocation.booleans'), [false]);
      assert.deepStrictEqual(attributes.get('invocation.single'), ['stop']);
      assert.deepStrictEqual(attributes.get('invocation.empty'), []);
      assert.deepStrictEqual(attributes.get('invocation.nullish'), [
        null,
        'a',
        null,
      ]);
      assert.deepStrictEqual(attributes.get('invocation.set.numbers'), [
        3,
        null,
        4,
      ]);
      assert(
        !attributes.has('invocation.invalid'),
        'invocationSpanArrayAttributes: heterogeneous array should be omitted'
      );
      assert(
        !attributes.has('invocation.set.invalid'),
        'invocationSpanArrayAttributes: nested array should be omitted'
      );
    }

    // Oversized arrays are dropped and degrade into a span data limit warning, while arrays
    // within the limit are recorded in full.
    {
      const testSpans = spansByTest.get('arrayAttributeByteLimit') || [];
      const span = testSpans.find((s) => s.name === 'array-limit-strings-op');
      assert(
        span,
        "arrayAttributeByteLimit: span 'array-limit-strings-op' present"
      );
      assert(!('big.strings' in span));
      assert.strictEqual(
        span['cloudflare.warning.type'],
        'span_data_limit_exceeded'
      );
      assert.match(
        span['cloudflare.warning.message'],
        /attribute "big\.strings" of size 75776$/
      );
      assert.deepStrictEqual(span.fits, new Array(512).fill('abcd'));

      const nullishSpan = testSpans.find(
        (s) => s.name === 'array-limit-nullish-op'
      );
      assert(nullishSpan, 'arrayAttributeByteLimit: nullish span present');
      assert(!('nulls.15' in nullishSpan));
      assert.deepStrictEqual(
        nullishSpan['nulls.14'],
        new Array(512).fill(null)
      );
      assert.strictEqual(
        nullishSpan['cloudflare.warning.type'],
        'span_data_limit_exceeded'
      );
      assert.match(
        nullishSpan['cloudflare.warning.message'],
        /attribute "nulls\.15" of size 4096$/
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
