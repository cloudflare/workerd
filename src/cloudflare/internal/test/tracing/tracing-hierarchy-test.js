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

export const startSpanDoesNotBecomeActive = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    const detached = ctx.tracing.startSpan('hierarchy-detached-start-span');
    detached.setAttribute('case', 'startSpanDoesNotBecomeActive');
    detached.setAttribute('role', 'detached');

    withSpan('hierarchy-after-detached-start-span', (span) => {
      span.setAttribute('case', 'startSpanDoesNotBecomeActive');
      span.setAttribute('role', 'after-detached');
    });

    detached.end();
  },
};

export const startSpanUsesCurrentActiveSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    withSpan('hierarchy-start-span-active-parent', (outer) => {
      outer.setAttribute('case', 'startSpanUsesCurrentActiveSpan');
      outer.setAttribute('role', 'outer');

      const detached = ctx.tracing.startSpan('hierarchy-detached-active-child');
      detached.setAttribute('case', 'startSpanUsesCurrentActiveSpan');
      detached.setAttribute('role', 'detached');

      withSpan('hierarchy-after-detached-active-child', (span) => {
        span.setAttribute('case', 'startSpanUsesCurrentActiveSpan');
        span.setAttribute('role', 'after-detached');
      });

      detached.end();
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

export const withActiveSpanReentersStartSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    const { tracing } = ctx;
    // A startSpan() span is never active on its own; withActiveSpan() re-enters it so
    // user spans and runtime spans (fetch), including after an await, nest under it.
    const parent = tracing.startSpan('hierarchy-reentered');
    parent.setAttribute('case', 'withActiveSpanReentersStartSpan');
    parent.setAttribute('role', 'parent');

    const sum = tracing.withActiveSpan(
      parent,
      (a, b) => {
        assert.strictEqual(tracing.getActiveSpan(), parent);
        withSpan('hierarchy-reentered-sync-child', (span) => {
          span.setAttribute('case', 'withActiveSpanReentersStartSpan');
        });
        return a + b;
      },
      1,
      2
    );
    assert.strictEqual(sum, 3);
    assert.notStrictEqual(tracing.getActiveSpan(), parent);

    await tracing.withActiveSpan(parent, async () => {
      await new Promise((r) => setTimeout(r, 5));
      assert.strictEqual(tracing.getActiveSpan(), parent);
      const response = await env.fetchTarget.fetch('http://mock/reentered');
      assert.strictEqual(response.status, 200);
      await response.text();
    });

    assert.throws(
      () =>
        tracing.withActiveSpan(parent, () => {
          throw new Error('boom');
        }),
      /boom/
    );
    assert.ok(parent.isTraced, 'withActiveSpan must not end the span');
    parent.end();
  },
};

export const withActiveSpanOverridesAmbient = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    const { tracing } = ctx;
    // One callback drives two tasks, as a shared executor tick does: each task's spans
    // must nest under its own span rather than under the ambient span, and the ambient
    // span must be restored afterwards.
    const a = tracing.startSpan('hierarchy-task-a');
    const b = tracing.startSpan('hierarchy-task-b');
    for (const span of [a, b]) {
      span.setAttribute('case', 'withActiveSpanOverridesAmbient');
    }

    withSpan('hierarchy-task-ambient', (ambient) => {
      ambient.setAttribute('case', 'withActiveSpanOverridesAmbient');
      tracing.withActiveSpan(a, () => {
        withSpan('hierarchy-task-a-child', () => {});
      });
      tracing.withActiveSpan(b, () => {
        withSpan('hierarchy-task-b-child', () => {});
      });
      assert.strictEqual(tracing.getActiveSpan(), ambient);
      withSpan('hierarchy-task-ambient-child', () => {});
    });

    a.end();
    b.end();
  },
};

export const withActiveSpanEndedSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    const { tracing } = ctx;
    // An ended span has nothing to parent under, so spans created inside keep the
    // enclosing parent.
    const ended = tracing.startSpan('hierarchy-ended');
    ended.end();
    withSpan('hierarchy-ended-ambient', (ambient) => {
      ambient.setAttribute('case', 'withActiveSpanEndedSpan');
      tracing.withActiveSpan(ended, () => {
        assert.strictEqual(tracing.getActiveSpan(), ended);
        withSpan('hierarchy-ended-child', () => {});
      });
    });
  },
};

let foreignSpan;

export const withActiveSpanForeignA = {
  async test(ctrl, env, ctx) {
    foreignSpan = ctx.tracing.startSpan('hierarchy-foreign');
    foreignSpan.end();
  },
};

export const withActiveSpanForeignB = {
  async test(ctrl, env, ctx) {
    // A span is owned by the request that created it and cannot be re-entered by another.
    assert.ok(foreignSpan, 'withActiveSpanForeignA must run first');
    assert.throws(
      () => ctx.tracing.withActiveSpan(foreignSpan, () => {}),
      /different request/
    );
  },
};
