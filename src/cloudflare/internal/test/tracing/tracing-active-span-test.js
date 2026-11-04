// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Scenarios for tracing.withActiveSpan(). Each test asserts what is visible from JavaScript; the
// tail worker in tracing-active-span-instrumentation-test.js then validates the parent/child
// relationships the runtime reported in the streaming-tail events.

import assert from 'node:assert';
import { DurableObject, tracing } from 'cloudflare:workers';

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

export const rejectsNonFunctionCallback = {
  test() {
    const span = tracing.startSpan('was-non-function-callback');
    assert.throws(
      () => tracing.withActiveSpan(span, 'not a function'),
      TypeError
    );
    assert.throws(() => tracing.withActiveSpan(span), TypeError);
    span.end();
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

export const sequentialTasksUnderAmbientSpan = {
  test(ctrl, env, ctx) {
    // One callback drives two tasks, as a shared executor tick does. Each task's spans nest under
    // its own span rather than the ambient span, and the ambient span is restored afterwards.
    const a = tracing.startSpan('was-task-a');
    const b = tracing.startSpan('was-task-b');
    ctx.tracing.enterSpan('was-task-ambient', (ambient) => {
      tracing.withActiveSpan(a, () => {
        ctx.tracing.enterSpan('was-task-a-child', () => {});
      });
      tracing.withActiveSpan(b, () => {
        ctx.tracing.enterSpan('was-task-b-child', () => {});
      });
      assert.strictEqual(tracing.getActiveSpan(), ambient);
      ctx.tracing.enterSpan('was-task-ambient-child', () => {});
    });
    a.end();
    b.end();
  },
};

// Set by crossRequestSpanSetup and read by crossRequestSpanThrows, which runs after it as a
// separate request.
let crossRequestSpan;

export const crossRequestSpanSetup = {
  test() {
    crossRequestSpan = tracing.startSpan('was-cross-request');
    crossRequestSpan.end();
  },
};

export const crossRequestSpanThrows = {
  test() {
    assert.ok(crossRequestSpan, 'crossRequestSpanSetup must run first');
    assert.throws(
      () => tracing.withActiveSpan(crossRequestSpan, () => {}),
      /different request/
    );
  },
};

// Requests to a Durable Object share an IoContext, so request B can activate a span that request A
// created. While A is still running, B's child spans are reported under A's span.
export class SharedSpanObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.releaseA = new Promise((resolve) => {
      this.resolveReleaseA = resolve;
    });
  }

  async fetch(request) {
    const requestName = new URL(request.url).pathname.slice(1);

    if (requestName === 'a') {
      this.spanFromA = tracing.startSpan('was-do-span');
      await this.releaseA;
      this.spanFromA.end();
      return new Response('a');
    }

    assert.ok(this.spanFromA, 'request a must start before request b');
    tracing.withActiveSpan(this.spanFromA, () => {
      assert.strictEqual(tracing.getActiveSpan(), this.spanFromA);
      tracing.enterSpan('was-do-child', () => {});
    });
    this.resolveReleaseA();
    return new Response('b');
  }
}

export const durableObjectSharesSpanAcrossRequests = {
  async test(ctrl, env) {
    const stub = env.sharedSpan.get(env.sharedSpan.idFromName('test'));
    const first = stub.fetch('https://example.com/a');
    const second = await stub.fetch('https://example.com/b');
    assert.strictEqual(await second.text(), 'b');
    assert.strictEqual(await (await first).text(), 'a');
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
