// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class MyService extends WorkerEntrypoint {
  // Returns a transient object rather than a plain value, so the caller receives a stub that it
  // is responsible for disposing.
  getCounter() {
    let value = 0;
    return {
      increment() {
        return ++value;
      },
    };
  }
}

// Obtains a stub and drops it without disposing. Kept in its own function so the stub is not
// still referenced by the caller's frame when the collection runs.
async function leakStub(env) {
  let counter = await env.MyService.getCounter();
  assert.strictEqual(await counter.increment(), 1);
}

export default {
  async test(ctrl, env, ctx) {
    // A leaked RPC stub makes ~JsRpcStub log a warning from a GC finalizer. Reaching that
    // warning requires the collection to happen while this request is still live, so that
    // IoContext::tryCurrent() and its tracer are both still available -- hence the explicit
    // gc() below rather than waiting for a collection to occur on its own.
    //
    // Logging the warning reads the current async context to attribute it to a span, which
    // creates V8 handles. Handle creation during a collection requires a fresh HandleScope,
    // because V8 seals the enclosing one for the duration of the collection. Without one this
    // aborts the process with "Cannot create a handle without a HandleScope".
    //
    // A streaming tail worker must be configured for this to bite: the tracer is what pulls the
    // warning down the path that touches the async context.
    await leakStub(env);

    // Unwind to a fresh task so the stub is not kept alive by a stack slot, which conservative
    // stack scanning would otherwise treat as a root.
    await scheduler.wait(0);

    // Reaching the end of this test at all is the assertion: the process must not abort.
    //
    // Whether the abort actually fires depends on the V8 build. V8 only refuses handle creation
    // while its seal is in force, which lasts for the duration of a collection; if sweeping is
    // deferred past the end of the collection then the finalizer runs outside the seal and the
    // handle is created silently. Builds with V8 checks enabled sweep within the collection and
    // do abort. The warning is emitted either way, so this test still drives the code path
    // everywhere -- it just cannot fail everywhere.
    gc();
    gc();
  },
};
