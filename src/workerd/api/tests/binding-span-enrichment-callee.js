// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Callee worker for binding-span-enrichment tests. ctx.tracing.enrichBindingSpan() is a public API:
// any callee may write attributes back to the caller's jsRpcSession user span. The special key
// "span.name" renames the span; other keys become tags. Last call before return wins.

import { WorkerEntrypoint } from 'cloudflare:workers';

export class CalleeEntrypoint extends WorkerEntrypoint {
  // Simulates what an AI Gateway worker would do: rename the caller's jsRpcSession
  // and attach gen_ai attributes.
  async run(model) {
    this.ctx.tracing.enrichBindingSpan({
      'span.name': 'ai_gateway.run',
      'gen_ai.request.model': model,
      'gen_ai.usage.input_tokens': 42,
      'cf.aig.gateway_id': 'my-gateway',
    });
    return { answer: 42 };
  }

  // Edge cases: non-string span.name must not rename the span (it's reserved and silently
  // ignored when the value isn't a string). Infinity / NaN must not crash and must round-trip
  // as numbers (they fall through to the double branch in the C++ guard).
  async runEdgeCases() {
    this.ctx.tracing.enrichBindingSpan({
      'span.name': 42, // non-string -> ignored, span keeps default name
      'gen_ai.temperature': Infinity,
      'gen_ai.usage.cost': NaN,
      'gen_ai.tag': 'edge_case_marker',
    });
    return 'ok';
  }
}
