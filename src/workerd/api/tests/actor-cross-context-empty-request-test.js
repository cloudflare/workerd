// Regression test for STOR-5008: IoContext::run() crash when incomingRequests
// is empty in an actor context.
//
// The bug: startDeleteQueueSignalTask() calls context->run(), which calls
// getCurrentIncomingRequest() via now()/getMetrics(). In a Durable Object,
// incomingRequests can be empty between events (after an IncomingRequest
// drains and before the next arrives). If a cross-context promise resolution
// fires scheduleAction() in this window, startDeleteQueueSignalTask wakes
// up and crashes, breaking the actor.
//
// This test triggers the race by:
// 1. Making a JS-RPC call to a DO that creates a promise and stores the
//    resolver in globalThis (shared V8 context between worker and DO).
// 2. The RPC call returns without ctx.waitUntil() or timers, so the
//    IncomingRequest drains immediately.
// 3. The test worker resolves the DO's promise from its own IoContext,
//    triggering cross-context resolution via scheduleAction().
// 4. startDeleteQueueSignalTask wakes up and calls context->run() with
//    empty incomingRequests -> crash -> actor breaks.
//
// Commit 1: Proves the bug — the actor breaks and subsequent calls fail.
// Commit 2 (fix): The test will be updated to show the actor survives.

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
    // check it later (if the actor survives).
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

// This test demonstrates the broken behavior (STOR-5008).
// The actor breaks when startDeleteQueueSignalTask tries to call
// context->run() with empty incomingRequests.
export const actorBreaksOnCrossContextResolveWithEmptyRequests = {
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
    // -> calls context->run() -> getCurrentIncomingRequest() -> CRASH.
    strictEqual(typeof globalThis.actorResolver, 'function');
    globalThis.actorResolver('cross-context-value');
    globalThis.actorResolver = undefined;

    // Step 4: Yield so the startDeleteQueueSignalTask processes the signal.
    // The crash in context->run() triggers context->abort(), which breaks
    // the actor.
    await scheduler.wait(10);

    // Step 5: The actor is now broken. Attempting another RPC call should
    // fail. This proves the bug exists.
    try {
      await stub.getResult();
      // If we get here, the bug is fixed (or was not triggered).
      // For commit 1 (demonstrating the bug), this line should NOT execute.
      throw new Error(
        'Expected actor to be broken, but getResult() succeeded. ' +
          'If the fix has been applied, update this test to expect success.'
      );
    } catch (e) {
      // The actor broke due to the IoContext abort from the crash in
      // startDeleteQueueSignalTask. The exact error message may vary,
      // but it should indicate the actor is broken / internal error.
      // We just verify the call failed and it's not our own throw.
      if (e.message?.startsWith('Expected actor to be broken')) {
        throw e;
      }
      // Good — the actor is broken as expected. The bug is reproduced.
    }
  },
};
