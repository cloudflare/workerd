// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-102:
// GC of an anonymous WorkerStub must not abort the process when the child
// v8::Isolate is torn down inside the parent's cppgc finalizer.
//
// Before the fix, when DeleteQueue::scheduleDeletion() synchronously destroyed
// the WorkerStubImpl (and its child WorkerService / v8::Isolate) while the
// thread-local inCppgcShimDestructor flag was set by the parent's
// CppgcShim::~CppgcShim(), causing HeapTracer::clearWrappers() in the child
// isolate to hit KJ_ASSERT(!inCppgcShimDestructor) and std::terminate().
// After the fix, we defer destruction of the `WorkerService` owned by `WorkerStubImpl`
// to the next turn of the event loop, skipping the nested teardown
import assert from 'node:assert';

export let gcAnonymousWorkerStub = {
  async test(ctrl, env, ctx) {
    // Load an anonymous child worker (no name → sole owner is the JSG WorkerStub).
    let stub = env.loader.load({
      compatibilityDate: '2025-01-01',
      mainModule: 'main.js',
      modules: {
        'main.js': `export default { fetch() { return new Response('ok'); } }`,
      },
    });

    // Force the child WorkerService to be fully constructed by making a request.
    let resp = await stub.getEntrypoint().fetch('http://x/');
    assert.strictEqual(await resp.text(), 'ok');

    // Drop the only JS reference so the WorkerStub becomes unreachable.
    stub = null;

    // Trigger a major GC. Pre-fix this would abort the process
    gc();

    // If we reach here the process did not abort — the fix is working.
    // Give the event loop a turn so any deferred destruction can complete.
    await new Promise((resolve) => setTimeout(resolve, 0));
  },
};
