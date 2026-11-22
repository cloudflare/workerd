// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This worker has no tail worker, so its requests observe no spans.

import assert from 'node:assert';
import { AsyncLocalStorage } from 'node:async_hooks';
import { tracing } from 'cloudflare:workers';

// Bound outside every request, so it runs with no trace context of its own.
const getActiveSpanBoundAtModuleScope = AsyncLocalStorage.bind(() =>
  tracing.getActiveSpan()
);

export const getActiveSpan = {
  async test(ctrl, env, ctx) {
    const invocationSpan = tracing.getActiveSpan();
    assert.ok(invocationSpan);
    assert.strictEqual(invocationSpan.isTraced, false);
    assert.strictEqual(tracing.getActiveSpan(), invocationSpan);
    assert.strictEqual(ctx.tracing.getActiveSpan(), invocationSpan);

    // An unobserved request runs without a trace frame of its own, so this is also how the
    // request's own code finds its span.
    assert.strictEqual(getActiveSpanBoundAtModuleScope(), invocationSpan);

    await ctx.tracing.startActiveSpan('untraced-op', async (span) => {
      assert.strictEqual(span.isTraced, false);
      assert.strictEqual(tracing.getActiveSpan(), span);
      await Promise.resolve();
      assert.strictEqual(tracing.getActiveSpan(), span);
    });

    await Promise.resolve();
    assert.strictEqual(tracing.getActiveSpan(), invocationSpan);
  },
};
