// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests that non-span tail events (Log, Exception, DiagnosticChannelEvent) emitted
// to a streaming tail worker carry the spanContext of the currently-active user
// span (pushed via ctx.tracing.enterSpan / withSpan), rather than always pointing
// at the root invocation span. See EW-10672.
//
// The companion tail worker in tracing-log-attribution-instrumentation-test.js
// collects the streaming tail events and runs the assertions in a follow-up
// `validate` test.

import { channel } from 'node:diagnostics_channel';

export const logInsideEnterSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Baseline: a log inside an enterSpan must be attributed to that span.
    withSpan('log-attr-single', (span) => {
      span.setAttribute('case', 'logInsideEnterSpan');
      console.log('log-attr-single-message');
    });
  },
};

export const logInsideNestedEnterSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Nested spans: the log inside the inner span must be attributed to the inner
    // span (not the outer, and not the root invocation).
    withSpan('log-attr-outer', (outer) => {
      outer.setAttribute('case', 'logInsideNestedEnterSpan');
      outer.setAttribute('role', 'outer');
      // Log inside outer (before opening inner) -- attributed to outer.
      console.log('log-attr-outer-before-inner');
      withSpan('log-attr-inner', (inner) => {
        inner.setAttribute('case', 'logInsideNestedEnterSpan');
        inner.setAttribute('role', 'inner');
        console.log('log-attr-inner-message');
      });
      // After inner closes, this log should be re-attributed to outer.
      console.log('log-attr-outer-after-inner');
    });
  },
};

export const logAcrossAwait = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // Log inside an async enterSpan AFTER an await. The fix only works if the
    // AsyncContextFrame carrying the user span propagates across microtasks.
    await withSpan('log-attr-async', async (span) => {
      span.setAttribute('case', 'logAcrossAwait');
      await new Promise((r) => setTimeout(r, 5));
      console.log('log-attr-async-message');
    });
  },
};

export const logOutsideAnySpan = {
  async test(ctrl, env, ctx) {
    // Logs outside any enterSpan should still land at the top-level onset span
    // (i.e., their parentSpanId equals the root). This confirms we haven't
    // broken the fall-back path.
    console.log('log-attr-root-message');
  },
};

export const diagnosticsChannelInsideEnterSpan = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;
    // diagnostics_channel.publish() emits a DiagnosticChannelEvent tail event when
    // there is at least one subscriber. The event should be attributed to the
    // enclosing user span.
    const ch = channel('log-attr-dc-channel');
    ch.subscribe(() => {});
    withSpan('log-attr-dc', (span) => {
      span.setAttribute('case', 'diagnosticsChannelInsideEnterSpan');
      ch.publish({ hello: 'world' });
    });
  },
};
