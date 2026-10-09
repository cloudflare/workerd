// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test: when garbage collection destroys an RpcStub that was never disposed, its
// destructor logs a warning from inside a V8 GC callback, and the warning is attributed to the
// current user span. If the async context frame active at that point was captured in a different
// request, looking up the span threw a C++ exception out of the GC callback. Unwinding through V8
// skipped V8's DisallowJavascriptExecution scope, so the isolate was left unable to run JavaScript
// and the next call into it was a V8 fatal error ("Invoke in DisallowJavascriptExecutionScope").
//
// The span lookup now falls back to the current request's root span instead of throwing, so the
// warning is still logged; the tail worker in rpc-stub-gc-foreign-span-tail.js checks for it.

import { AsyncLocalStorage } from 'node:async_hooks';
import { RpcStub } from 'cloudflare:workers';
import * as assert from 'node:assert';

let runInCapturedFrame;

function leakStubs() {
  // Never disposed, so each stub's destructor warns when it is garbage collected.
  for (let i = 0; i < 10; i++) {
    new RpcStub({});
  }
}

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/capture') {
      // Snapshot an async context frame that carries this request's user span.
      ctx.tracing.enterSpan('captured-span', () => {
        runInCapturedFrame = AsyncLocalStorage.snapshot();
      });
      return new Response('captured');
    }
    if (pathname === '/collect') {
      // Run under the other request's frame while this request's IoContext is current, and
      // collect the stubs from there. A minor GC releases them from the GC epilogue callback,
      // and a major GC releases them from cppgc finalizers; both run inside V8's GC.
      runInCapturedFrame(() => {
        leakStubs();
        gc({ type: 'minor' });
        leakStubs();
        gc();
      });
      return new Response('collected');
    }
    if (pathname === '/ping') {
      return new Response('pong');
    }
    return new Response('not found', { status: 404 });
  },
};

export const gcDuringForeignSpanDoesNotBreakIsolate = {
  async test(ctrl, env) {
    let resp = await env.SELF.fetch('http://example.com/capture');
    assert.strictEqual(await resp.text(), 'captured');

    resp = await env.SELF.fetch('http://example.com/collect');
    assert.strictEqual(await resp.text(), 'collected');

    // The isolate must still be able to run JavaScript after that GC.
    resp = await env.SELF.fetch('http://example.com/ping');
    assert.strictEqual(await resp.text(), 'pong');
  },
};
