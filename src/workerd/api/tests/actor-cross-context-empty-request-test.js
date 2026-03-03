// Regression test for STOR-5008: IoContext::run() crash when incomingRequests
// is empty in an actor context.
//
// The bug: startDeleteQueueSignalTask() calls context->run(), which called
// getCurrentIncomingRequest() via now()/getMetrics(). In a Durable Object,
// incomingRequests can be empty between events (after an IncomingRequest
// drains and before the next arrives). If a cross-context promise resolution
// fires scheduleAction() in this window, startDeleteQueueSignalTask wakes
// up and context->run() would crash.
//
// The fix: IoContext::run() now handles empty incomingRequests gracefully by
// falling back to takeAsyncLockWithoutRequest() and skipping request-scoped
// timer sync, tracing, and metrics attribution. The actor survives and the
// cross-context promise continuation runs correctly.

import { DurableObject } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';

export class TestActor extends DurableObject {
  // RPC method: create a promise whose resolver is stored in globalThis.
  // Returns immediately without calling ctx.waitUntil() or starting timers,
  // so the IncomingRequest drains immediately after the RPC call completes.
  async createCrossContextPromise() {
    const { promise, resolve } = Promise.withResolvers();

    // Store the resolver in globalThis so the test worker's IoContext can
    // access it. Both IoContexts share the same V8 isolate and context.
    globalThis.actorResolver = resolve;

    // Chain a .then() — this continuation must run in the actor's IoContext
    // via cross-context promise resolution. Stash the promise so we can
    // verify the result later.
    this.resultPromise = promise.then((value) => {
      this.resolvedValue = value;
      return value;
    });

    return 'created';
  }

  // RPC method: check the result of the cross-context promise resolution.
  async getResult() {
    await this.resultPromise;
    return this.resolvedValue;
  }
}

// Verify that cross-context promise resolution works correctly when the
// actor's incomingRequests list is empty. Before the fix, this would crash
// the actor. After the fix, the actor survives and the continuation runs.
export const actorSurvivesCrossContextResolveWithEmptyRequests = {
  async test(ctrl, env) {
    const id = env.ns.idFromName('test-actor');
    const stub = env.ns.get(id);

    // Step 1: RPC call to the DO. The DO creates a promise and stores
    // the resolver in globalThis. The RPC call returns, and because
    // the DO didn't call ctx.waitUntil() or start any timers,
    // waitUntilTasks is empty -> drain() completes immediately ->
    // IncomingRequest is destroyed -> incomingRequests becomes empty.
    const r1 = await stub.createCrossContextPromise();
    strictEqual(r1, 'created');

    // Step 2: Yield the event loop so the IncomingRequest fully drains
    // and is destroyed. The DO's incomingRequests list is now empty.
    await scheduler.wait(10);

    // Step 3: Resolve the DO's promise from this IoContext (the test
    // worker's IoContext, which has a different PromiseContextTag).
    // V8 detects the tag mismatch -> SetPromiseCrossContextResolveCallback
    // -> IoCrossContextExecutor -> scheduleAction() on the DO's DeleteQueue
    // -> fulfills crossThreadFulfiller -> startDeleteQueueSignalTask wakes
    // -> calls context->run().
    //
    // With the fix, context->run() handles empty incomingRequests by using
    // takeAsyncLockWithoutRequest and skipping request-scoped operations.
    strictEqual(typeof globalThis.actorResolver, 'function');
    globalThis.actorResolver('cross-context-value');
    globalThis.actorResolver = undefined;

    // Step 4: Yield so the startDeleteQueueSignalTask processes the signal
    // and the microtask queue drains in the DO's IoContext.
    await scheduler.wait(10);

    // Step 5: The actor is alive and the cross-context promise continuation
    // ran correctly. Verify the result.
    const result = await stub.getResult();
    strictEqual(result, 'cross-context-value');
  },
};
