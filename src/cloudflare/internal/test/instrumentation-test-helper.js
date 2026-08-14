// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO(o11y): Refactor to remove redundant code, and merge with
// src/workerd/api/tests/instrumentation-tail-worker.js

import * as assert from 'node:assert';

/**
 * Common helper functions for instrumentation tests.
 * This module provides utilities for collecting and processing spans
 * from streaming tail workers during tests.
 */

/**
 * Creates module-level state for instrumentation tests.
 * This mirrors the original test pattern with module-level variables.
 * @returns {Object} State object with invocationPromises and spans
 */
export function createInstrumentationState() {
  return {
    invocationPromises: [],
    spans: new Map(),
  };
}

/**
 * Creates the tailStream handler for instrumentation tests.
 * @param {Object} state - The state object from createInstrumentationState
 * @returns {Function} The tailStream handler function
 */
export function createTailStreamHandler(state) {
  return (event, env, ctx) => {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    state.invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Accumulate the span info for easier testing
    return (event) => {
      let spanKey = `${event.invocationId}#${event.event.spanId || event.spanContext.spanId}`;
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          state.spans.set(spanKey, { name: event.event.name });
          break;
        case 'attributes': {
          let span = state.spans.get(spanKey);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          state.spans.set(spanKey, span);
          break;
        }
        case 'spanClose': {
          let span = state.spans.get(spanKey);
          span['closed'] = true;
          state.spans.set(spanKey, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  };
}

/**
 * Creates a tail-stream handler that records span parent/child relationships in addition
 * to the span attributes. Use this when your test needs to assert on hierarchy (e.g.,
 * "the fetch span's parent is the enterSpan I just opened").
 *
 * The returned state shape is:
 *   state.spans: Map<spanKey, {
 *     name, spanId, parentSpanId, invocationId, closed, ...attributes
 *   }>
 *   state.topLevelSpans: Map<invocationId, topLevelSpanId>
 *     (the span ID from the onset event, i.e. the implicit request root)
 *
 * @returns {{state: Object, tailStream: Function, waitForCompletion: Function}}
 */
export function createHierarchyAwareCollector() {
  const state = {
    invocationPromises: [],
    spans: new Map(),
    topLevelSpans: new Map(),
  };

  const tailStream = (event, env, ctx) => {
    // Record the onset event's spanId as the top-level span for this invocation.
    state.topLevelSpans.set(event.invocationId, event.event.spanId);

    let resolveFn;
    state.invocationPromises.push(
      new Promise((resolve) => {
        resolveFn = resolve;
      })
    );

    return (event) => {
      const spanKey = `${event.invocationId}#${event.event.spanId || event.spanContext.spanId}`;
      switch (event.event.type) {
        case 'spanOpen':
          // event.event.spanId is the new span's id; event.spanContext.spanId is the
          // span that was active when this one was opened (i.e., its parent).
          state.spans.set(spanKey, {
            name: event.event.name,
            spanId: event.event.spanId,
            parentSpanId: event.spanContext.spanId,
            invocationId: event.invocationId,
          });
          break;
        case 'attributes': {
          const span = state.spans.get(spanKey);
          if (!span) break;
          for (const { name, value } of event.event.info) {
            span[name] = value;
          }
          break;
        }
        case 'spanClose': {
          const span = state.spans.get(spanKey);
          if (!span) break;
          span.closed = true;
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  };

  const waitForCompletion = () => Promise.allSettled(state.invocationPromises);

  return { state, tailStream, waitForCompletion };
}

/**
 * Find a span whose `name` field matches (after an optional filter function).
 * Throws if there is not exactly one such span in the collector's state.
 */
export function findSpanByName(state, name, filterFn = () => true) {
  const matches = [];
  for (const span of state.spans.values()) {
    if (span.name === name && filterFn(span)) matches.push(span);
  }
  if (matches.length === 0) {
    throw new Error(`No span found with name='${name}'`);
  }
  if (matches.length > 1) {
    throw new Error(
      `Expected exactly one span with name='${name}', found ${matches.length}: ` +
        JSON.stringify(matches, null, 2)
    );
  }
  return matches[0];
}

/**
 * Runs instrumentation test assertions.
 * This mirrors the original test logic exactly.
 * @param {Object} state - The state object from createInstrumentationState
 * @param {Array} expectedSpans - The expected spans to compare against
 * @param {Object} options - Options for the test
 * @param {Function} options.mapFn - Map function to transform spans before comparison (default: x => x)
 * @param {Function} options.filterFn - Filter function for spans (default: filters out jsRpcSession)
 * @param {string} options.testName - Name for the test (default: 'instrumentation')
 * @param {boolean} options.logReceived - Log received spans for debugging (default: false)
 *
 * Usage for updating tests:
 *   await runInstrumentationTest(state, expectedSpans, { logReceived: true });
 *   // Copy the logged output to update expectedSpans
 */
export async function runInstrumentationTest(
  state,
  expectedSpans,
  options = {}
) {
  const {
    mapFn = (x) => x,
    filterFn = (span) => span.name !== 'jsRpcSession',
    testName = 'instrumentation',
    logReceived = false,
  } = options;

  // Wait for all the tailStream executions to finish
  await Promise.allSettled(state.invocationPromises);

  // Recorded streaming tail worker events, in insertion order,
  // mapping and filtering spans not associated with the test
  let received = Array.from(state.spans.values()).map(mapFn).filter(filterFn);

  // Log received spans for debugging/updating tests
  if (logReceived) {
    console.log(`Received spans for ${testName}:\n`, received);
  }

  let failed = 0;
  let i = -1;

  try {
    assert.equal(received.length, expectedSpans.length);
    for (i = 0; i < received.length; i++) {
      assert.deepStrictEqual(received[i], expectedSpans[i]);
    }
  } catch (e) {
    failed++;
    if (i >= 0) {
      console.log('spans are not identical', e);
    } else {
      console.error(e);
    }
  }

  if (failed > 0) {
    throw `${testName} test failed`;
  }
}

/**
 * Creates a tail stream collector for instrumentation tests with encapsulated state.
 * This provides a different API style where state is encapsulated in the returned object.
 * @returns {Object} An object with methods to handle spans
 */
export function createTailStreamCollector() {
  let state = createInstrumentationState();

  const tailStream = createTailStreamHandler(state);

  let spans = state.spans;
  let invocationPromises = state.invocationPromises;
  const waitForCompletion = () => {
    return Promise.allSettled(invocationPromises);
  };

  return {
    tailStream,
    waitForCompletion,
    spans,
  };
}

/**
 * Groups spans by a specific attribute.
 * @param {Array} spans - The spans to group
 * @param {string} attribute - The attribute to group by
 * @returns {Map} A map of attribute values to arrays of spans
 */
export function groupSpansBy(spans, attribute) {
  const groups = new Map();

  for (const span of spans) {
    const key = span[attribute] || 'unknown';
    if (!groups.has(key)) {
      groups.set(key, []);
    }
    groups.get(key).push(span);
  }

  return groups;
}
