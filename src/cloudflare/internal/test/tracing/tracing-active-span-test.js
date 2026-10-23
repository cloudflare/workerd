// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Scenarios for tracing.withActiveSpan(). Each test asserts what is visible from JavaScript; the
// tail worker in tracing-active-span-instrumentation-test.js then validates the parent/child
// relationships the runtime reported in the streaming-tail events.

import assert from 'node:assert';
import { tracing } from 'cloudflare:workers';

// Created outside any invocation, so this is a no-op span.
const moduleScopeSpan = tracing.startSpan('was-module-scope');

// withActiveSpan works without an IoContext; it only changes what getActiveSpan() returns.
assert.strictEqual(
  tracing.withActiveSpan(moduleScopeSpan, () => tracing.getActiveSpan()),
  moduleScopeSpan
);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export const activeSpanIdentity = {
  test() {
    const invocationSpan = tracing.getActiveSpan();
    const span = tracing.startSpan('was-identity');

    const result = tracing.withActiveSpan(
      span,
      (a, b) => {
        // getActiveSpan() returns the same object, not a wrapper.
        assert.strictEqual(tracing.getActiveSpan(), span);
        tracing.startSpan('was-identity-start-span-child').end();
        return a + b;
      },
      1,
      2
    );
    assert.strictEqual(result, 3);

    // The previous active span is restored, and withActiveSpan does not end the span.
    assert.strictEqual(tracing.getActiveSpan(), invocationSpan);
    assert.strictEqual(span.isTraced, true);
    span.end();
  },
};

export const syncThrowRestoresActiveSpan = {
  test() {
    const invocationSpan = tracing.getActiveSpan();
    const span = tracing.startSpan('was-sync-throw');
    assert.throws(
      () =>
        tracing.withActiveSpan(span, () => {
          throw new Error('boom');
        }),
      { message: 'boom' }
    );
    assert.strictEqual(tracing.getActiveSpan(), invocationSpan);
    assert.strictEqual(span.isTraced, true);
    span.end();
  },
};

export const rejectsNonRuntimeSpans = {
  test() {
    // Only runtime-created spans are accepted. Objects that merely implement spanContext() are
    // rejected.
    const jsSpan = {
      spanContext() {
        return {
          traceId: '4bf92f3577b34da6a3ce929d0e0e4736',
          spanId: '00f067aa0ba902b7',
          traceFlags: 1,
        };
      },
    };
    assert.throws(() => tracing.withActiveSpan(jsSpan, () => {}), TypeError);
    assert.throws(() => tracing.withActiveSpan(undefined, () => {}), TypeError);
  },
};

export const fetchInsideWithActiveSpan = {
  async test(ctrl, env) {
    const span = tracing.startSpan('was-fetch-parent');
    await tracing.withActiveSpan(span, async () => {
      const response = await env.fetchTarget.fetch('http://mock/was-fetch');
      assert.strictEqual(response.status, 200);
      await response.text();
    });
    span.end();
  },
};

export const asyncContinuation = {
  async test(ctrl, env, ctx) {
    const span = tracing.startSpan('was-async-parent');

    const promise = tracing.withActiveSpan(span, async () => {
      await sleep(5);
      assert.strictEqual(tracing.getActiveSpan(), span);
      ctx.tracing.enterSpan('was-async-child', () => {});
      const response = await env.fetchTarget.fetch('http://mock/was-async');
      await response.text();
      return 'done';
    });

    // The callback's promise is returned without being awaited, and code that runs after
    // withActiveSpan returns is outside the active scope.
    assert.ok(promise instanceof Promise);
    assert.notStrictEqual(tracing.getActiveSpan(), span);
    assert.strictEqual(await promise, 'done');
    span.end();
  },
};

export const nestedWithActiveSpan = {
  test(ctrl, env, ctx) {
    const outer = tracing.startSpan('was-nested-outer');
    const inner = tracing.startSpan('was-nested-inner');

    tracing.withActiveSpan(outer, () => {
      tracing.withActiveSpan(inner, () => {
        assert.strictEqual(tracing.getActiveSpan(), inner);
        ctx.tracing.enterSpan('was-nested-inner-child', () => {});
      });
      assert.strictEqual(tracing.getActiveSpan(), outer);
      ctx.tracing.enterSpan('was-nested-outer-child', () => {});
    });

    inner.end();
    outer.end();
  },
};

export const overridesEnclosingSpan = {
  test(ctrl, env, ctx) {
    const target = tracing.startSpan('was-override-target');
    ctx.tracing.enterSpan('was-override-enclosing', () => {
      tracing.withActiveSpan(target, () => {
        ctx.tracing.enterSpan('was-override-child', () => {});
      });
    });
    target.end();
  },
};

export const endedSpanRemainsParent = {
  test(ctrl, env, ctx) {
    const span = tracing.startSpan('was-ended-parent');
    span.end();
    tracing.withActiveSpan(span, () => {
      assert.strictEqual(tracing.getActiveSpan(), span);
      assert.strictEqual(span.isTraced, false);
      ctx.tracing.enterSpan('was-ended-child', () => {});
    });
  },
};

export const invocationSpanResetsNesting = {
  test(ctrl, env, ctx) {
    const invocationSpan = tracing.getActiveSpan();
    ctx.tracing.enterSpan('was-invocation-enclosing', () => {
      tracing.withActiveSpan(invocationSpan, () => {
        assert.strictEqual(tracing.getActiveSpan(), invocationSpan);
        ctx.tracing.enterSpan('was-invocation-child', () => {});
      });
    });
    // Ending the invocation span is still a no-op.
    invocationSpan.end();
    assert.strictEqual(invocationSpan.isTraced, true);
  },
};

export const noopSpanKeepsEnclosingParent = {
  test(ctrl, env, ctx) {
    assert.strictEqual(moduleScopeSpan.isTraced, false);
    ctx.tracing.enterSpan('was-noop-enclosing', () => {
      tracing.withActiveSpan(moduleScopeSpan, () => {
        assert.strictEqual(tracing.getActiveSpan(), moduleScopeSpan);
        ctx.tracing.enterSpan('was-noop-child', () => {});
      });
    });
  },
};
