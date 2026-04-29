// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Callee worker for binding-span-enrichment tests. ctx.tracing.setBindingSpan() is a public API:
// any callee may write attributes back to the caller's jsRpcSession user span. The special key
// "span.name" renames the span; other keys become tags. Last call before return wins.

import { WorkerEntrypoint } from 'cloudflare:workers';

export class CalleeEntrypoint extends WorkerEntrypoint {
  // Simulates what an AI Gateway worker would do: rename the caller's jsRpcSession
  // and attach gen_ai attributes.
  async run(model) {
    this.ctx.tracing.setBindingSpan({
      'span.name': 'ai_gateway.run',
      'gen_ai.request.model': model,
      'gen_ai.usage.input_tokens': 42,
      'cf.aig.gateway_id': 'my-gateway',
    });
    return { answer: 42 };
  }
}
