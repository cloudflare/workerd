// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker for binding-span-enrichment tests. Validates that the callee's
// enrichBindingSpan() attributes (including "span.name" rename) appear on the caller's
// jsRpcSession span in the STW stream.

import assert from 'node:assert';
import {
  createHierarchyAwareCollector,
  findSpanByName,
} from 'instrumentation-test-helper';

const collector = createHierarchyAwareCollector();

export default {
  tailStream(onsetEvent, env, ctx) {
    const inner = collector.tailStream(onsetEvent, env, ctx);
    return (event) => {
      // Collect tag attributes onto spans; treat "span.name" as a rename signal.
      if (event.event.type === 'attributes') {
        const spanKey = `${event.invocationId}#${event.spanContext.spanId}`;
        const span = collector.state.spans.get(spanKey);
        if (span) {
          if (!span.attributes) span.attributes = {};
          for (const { name, value } of event.event.info) {
            if (name === 'span.name') {
              span.name = String(value);
            } else {
              span.attributes[name] = value;
            }
          }
        }
      }
      return inner?.(event);
    };
  },
};

export const validate = {
  async test() {
    await collector.waitForCompletion();
    const { state } = collector;

    // The callee called enrichBindingSpan({ "span.name": "ai_gateway.run", ... }).
    // The jsRpcSession span must have been renamed and carry the attributes.
    const enrichedSpan = findSpanByName(state, 'ai_gateway.run');
    assert.ok(
      enrichedSpan,
      'Expected a span named "ai_gateway.run" in the STW stream'
    );

    assert.strictEqual(
      enrichedSpan.attributes?.['gen_ai.request.model'],
      'text-embedding-3-small'
    );
    assert.strictEqual(
      Number(enrichedSpan.attributes?.['gen_ai.usage.input_tokens']),
      42
    );
    assert.strictEqual(
      enrichedSpan.attributes?.['cf.aig.gateway_id'],
      'my-gateway'
    );
    assert.strictEqual(enrichedSpan.closed, true);

    // Merge invocation (callee.runMerge): two enrichBindingSpan calls in one method.
    // Latest name wins; same-key attribute is replaced; new key is appended; first-only
    // key is preserved.
    const mergedSpan = findSpanByName(state, 'final.name');
    assert.ok(
      mergedSpan,
      'Expected the second enrichBindingSpan call to win the rename'
    );
    const firstNameSurvived = [...state.spans.values()].some(
      (s) => s.name === 'first.name'
    );
    assert.strictEqual(
      firstNameSurvived,
      false,
      'first.name must not survive: it is overwritten by the second call'
    );
    assert.strictEqual(
      mergedSpan.attributes?.['merge.kept'],
      'from_first_call',
      'first-only key must survive the merge'
    );
    assert.strictEqual(
      mergedSpan.attributes?.['merge.overwritten'],
      'second_value',
      'same-key attribute must be replaced by the second call'
    );
    assert.strictEqual(
      mergedSpan.attributes?.['merge.added'],
      'from_second_call',
      'new key from the second call must be added'
    );

    // Edge-case invocation (callee.runEdgeCases): the marker attribute identifies the span;
    // the other assertions verify the B1 (finite-guard) C++ behaviour.
    const edgeSpans = [...state.spans.values()].filter(
      (s) => s.attributes?.['gen_ai.tag'] === 'edge_case_marker'
    );
    assert.strictEqual(
      edgeSpans.length,
      1,
      'Expected exactly one span tagged with the edge_case_marker'
    );
    const edgeSpan = edgeSpans[0];

    // B1: Infinity / NaN must round-trip as finite-or-non-finite numbers without crashing
    // the runtime. They go through the double branch (no int64 cast).
    const temp = edgeSpan.attributes?.['gen_ai.temperature'];
    const cost = edgeSpan.attributes?.['gen_ai.usage.cost'];
    assert.strictEqual(
      typeof temp === 'number' || typeof temp === 'bigint',
      true,
      `gen_ai.temperature should be numeric, got ${typeof temp}`
    );
    assert.strictEqual(
      typeof cost === 'number' || typeof cost === 'bigint',
      true,
      `gen_ai.usage.cost should be numeric, got ${typeof cost}`
    );
  },
};
