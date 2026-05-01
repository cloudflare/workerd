// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Callee worker for binding-span-enrichment tests. ctx.tracing.enrichBindingSpan() is a public API:
// any callee may rename the caller's binding-call span and/or attach attributes to it. Both
// `name` and `attributes` are optional. Last call before return wins.

import { WorkerEntrypoint } from 'cloudflare:workers';

export class CalleeEntrypoint extends WorkerEntrypoint {
  // Simulates what an AI Gateway worker would do: rename the caller's binding-call span
  // and attach gen_ai attributes.
  async run(model) {
    this.ctx.tracing.enrichBindingSpan({
      name: 'ai_gateway.run',
      attributes: {
        'gen_ai.request.model': model,
        'gen_ai.usage.input_tokens': 42,
        'cf.aig.gateway_id': 'my-gateway',
      },
    });
    return { answer: 42 };
  }

  // Edge cases: Infinity / NaN must not crash and must round-trip as numbers (they fall
  // through to the double branch in the C++ guard).
  async runEdgeCases() {
    this.ctx.tracing.enrichBindingSpan({
      name: 'edge_case.run',
      attributes: {
        'gen_ai.temperature': Infinity,
        'gen_ai.usage.cost': NaN,
        'gen_ai.tag': 'edge_case_marker',
      },
    });
    return 'ok';
  }
}
