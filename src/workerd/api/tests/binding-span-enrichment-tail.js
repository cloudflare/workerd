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
  },
};
