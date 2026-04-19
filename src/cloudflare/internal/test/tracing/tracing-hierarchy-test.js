// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests that verify the tracing hierarchy produced by enterSpan is correct. Each test
// case below runs a specific scenario (nested enterSpan, fetch inside enterSpan, etc.);
// the tail worker in tracing-hierarchy-instrumentation-test.js receives the streaming
// tail events and validates the parent/child relationships in a followup validation test.

import assert from 'node:assert';

export const nestedEnterSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Two nested synchronous enterSpan calls. The inner span's parent must be the outer
    // span, and the outer span's parent must be the top-level request span (onset).
    const result = withSpan('hierarchy-outer', (outer) => {
      outer.setAttribute('case', 'nestedEnterSpan');
      outer.setAttribute('role', 'outer');
      return withSpan('hierarchy-inner', (inner) => {
        inner.setAttribute('case', 'nestedEnterSpan');
        inner.setAttribute('role', 'inner');
        return 'ok';
      });
    });
    assert.strictEqual(result, 'ok');
  },
};

export const nestedEnterSpanAsync = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Async variant: the inner span is created after the outer span has awaited once,
    // so the correct parent is only reported if the AsyncContextFrame push propagates
    // through the microtask boundary.
    const result = await withSpan('hierarchy-async-outer', async (outer) => {
      outer.setAttribute('case', 'nestedEnterSpanAsync');
      outer.setAttribute('role', 'outer');
      await new Promise((r) => setTimeout(r, 5));
      return await withSpan('hierarchy-async-inner', async (inner) => {
        inner.setAttribute('case', 'nestedEnterSpanAsync');
        inner.setAttribute('role', 'inner');
        await new Promise((r) => setTimeout(r, 5));
        return 'ok';
      });
    });
    assert.strictEqual(result, 'ok');
  },
};

export const fetchInsideEnterSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // fetch() automatically creates a "fetch" user-trace span via IoContext::
    // makeUserTraceSpan, which reads getCurrentUserTraceSpan(). If our enterSpan
    // correctly pushed onto the AsyncContextFrame, that fetch span's parent will be
    // the outer span (not the top-level onset span).
    await withSpan('hierarchy-fetch-outer', async (outer) => {
      outer.setAttribute('case', 'fetchInsideEnterSpan');
      const response = await env.fetchTarget.fetch('http://mock/echo');
      assert.strictEqual(response.status, 200);
      await response.text();
    });
  },
};

export const fetchInsideNestedEnterSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Deeper nesting: fetch lives inside an inner enterSpan that is itself inside
    // an outer enterSpan. The fetch's parent must be the inner span.
    await withSpan('hierarchy-deep-outer', async (outer) => {
      outer.setAttribute('case', 'fetchInsideNestedEnterSpan');
      outer.setAttribute('role', 'outer');
      await withSpan('hierarchy-deep-inner', async (inner) => {
        inner.setAttribute('case', 'fetchInsideNestedEnterSpan');
        inner.setAttribute('role', 'inner');
        const response = await env.fetchTarget.fetch('http://mock/echo-deep');
        assert.strictEqual(response.status, 200);
        await response.text();
      });
    });
  },
};

export const siblingEnterSpans = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Two enterSpan calls at the same level (sequential, not nested) should each
    // produce a span whose parent is the top-level onset span, not each other.
    withSpan('hierarchy-sibling-a', (span) => {
      span.setAttribute('case', 'siblingEnterSpans');
      span.setAttribute('role', 'sibling-a');
    });
    withSpan('hierarchy-sibling-b', (span) => {
      span.setAttribute('case', 'siblingEnterSpans');
      span.setAttribute('role', 'sibling-b');
    });
  },
};

export const abandonedPromiseSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Wrap an async callback whose returned Promise never settles, and discard the
    // outer promise. The validator asserts that the request's outcome event still
    // fires in order; without BaseTracer::WeakRef, the abandoned SpanImpl would pin
    // the tracer via its SpanSubmitter and delay outcome until V8 GC.
    void withSpan('hierarchy-abandoned', async (span) => {
      span.setAttribute('case', 'abandonedPromiseSpan');
      await new Promise(() => {});
      span.setAttribute('reached', true); // unreachable
    });
  },
};
