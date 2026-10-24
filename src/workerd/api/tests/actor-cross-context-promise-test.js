// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for promises that an actor event creates and another IoContext settles later. The
// settlement runs back in the actor's IoContext, which outlives each event's IncomingRequest, and
// needs one: the continuations use timers and storage.

import { DurableObject } from 'cloudflare:workers';
import { deepStrictEqual, strictEqual } from 'node:assert';

// Polls until `read()` returns a value, for waiting on progress without a fixed sleep.
async function waitFor(what, read) {
  for (let i = 0; i < 2000; i++) {
    const value = read();
    if (value !== undefined) return value;
    await scheduler.wait(1);
  }
  throw new Error(`timed out waiting for ${what}`);
}

// Uses the request's timer and the actor's storage, which both need a current request.
async function useRequestScopedApis(ctx, key, value) {
  await scheduler.wait(1);
  await ctx.storage.put(key, value);
  return { value, stored: await ctx.storage.get(key) };
}

export class TestActor extends DurableObject {
  // Awaits the promise, so its request is still current when the settlement arrives.
  async awaitFromActiveRequest() {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.activeResolve = resolve;
    const value = await promise;
    return await useRequestScopedApis(this.ctx, 'active', value);
  }

  // Responds at once, but ctx.waitUntil() keeps the request current for the settlement.
  async createRetainedPromise() {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.retainedResolve = resolve;
    this.ctx.waitUntil(
      promise.then(async (value) => {
        globalThis.retainedResult = await useRequestScopedApis(
          this.ctx,
          'retained',
          value
        );
      })
    );
    return 'created';
  }

  // Responds without retaining the continuation, so the actor is idle once the session ends.
  async createDetachedPromise() {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.detachedResolve = resolve;
    this.detached = promise.then(async (value) => {
      // Set before the first await, to tell a started reaction from a finished one.
      globalThis.detachedReactionRan = true;
      return await useRequestScopedApis(this.ctx, 'detached', value);
    });
    return 'created';
  }

  async getDetachedResult() {
    return await this.detached;
  }

  // Like createDetachedPromise(), but nothing awaits the reaction; it records the value once it
  // has used the request-scoped APIs.
  createUnobservedPromise() {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.unobservedResolve = resolve;
    promise.then(async (value) => {
      await useRequestScopedApis(this.ctx, 'unobserved', value);
      globalThis.unobservedValue = value;
    });
    return 'created';
  }

  // An event that ends at once, without awaiting anything.
  ping() {
    return 'pong';
  }

  // Keeps a signal received over RPC without observing it, so its RPC subscription stays unarmed.
  holdSignal(signal) {
    globalThis.heldSignal = signal;
    return 'held';
  }

  // Adds and removes an abort listener on the held signal `times` times, from a context other than
  // the receiver's. Returns the number of actions then queued on the receiver.
  churnHeldSignalListener(times) {
    const signal = globalThis.heldSignal;
    const listener = () => {};
    for (let i = 0; i < times; i++) {
      signal.addEventListener('abort', listener);
      signal.removeEventListener('abort', listener);
    }
    return signal.getRpcReceiverQueuedActionCountForTest();
  }

  // Publishes a resolver in blockConcurrencyWhile(), then holds the input lock past the settlement.
  async settleDuringCriticalSection() {
    const order = [];
    const { promise, resolve } = Promise.withResolvers();
    const settled = promise.then(() => order.push('settled'));
    await this.ctx.blockConcurrencyWhile(async () => {
      globalThis.criticalResolve = resolve;
      while (!globalThis.criticalResolved) {
        await scheduler.wait(1);
      }
      await scheduler.wait(10);
      order.push('critical section done');
    });
    await settled;
    return order;
  }
}

export const settlementRunsUnderAnActiveRequest = {
  async test(_, env) {
    const stub = env.ns.get(env.ns.idFromName('active'));
    const pending = stub.awaitFromActiveRequest();

    // Publishing the resolver is the actor's last act before parking on the await.
    const resolve = await waitFor(
      'the actor to publish its resolver',
      () => globalThis.activeResolve
    );
    globalThis.activeResolve = undefined;
    resolve('active-value');

    const result = await pending;
    strictEqual(result.value, 'active-value');
    strictEqual(result.stored, 'active-value');
  },
};

export const settlementRunsUnderADrainingRequest = {
  async test(_, env) {
    const stub = env.ns.get(env.ns.idFromName('retained'));
    strictEqual(await stub.createRetainedPromise(), 'created');

    const resolve = globalThis.retainedResolve;
    globalThis.retainedResolve = undefined;
    resolve('retained-value');

    // With no further event, the settlement runs under the draining request.
    const result = await waitFor(
      'the retained continuation to run',
      () => globalThis.retainedResult
    );
    strictEqual(result.value, 'retained-value');
    strictEqual(result.stored, 'retained-value');
  },
};

export const settlementWaitsForTheNextEventWhenTheActorIsIdle = {
  async test(_, env) {
    const stub = env.ns.get(env.ns.idFromName('detached'));
    strictEqual(await stub.createDetachedPromise(), 'created');

    // Let the actor go idle before settling the promise.
    await scheduler.wait(100);

    globalThis.detachedResolve('detached-value');
    globalThis.detachedResolve = undefined;

    // With no request to run under, the settlement stays queued.
    await scheduler.wait(100);
    strictEqual(globalThis.detachedReactionRan, undefined);

    // The actor is still healthy, and its next event picks the settlement up.
    const result = await stub.getDetachedResult();
    strictEqual(globalThis.detachedReactionRan, true);
    strictEqual(result.value, 'detached-value');
    strictEqual(result.stored, 'detached-value');
  },
};

export const settlementRunsAsPartOfAShortNextEvent = {
  async test(_, env) {
    const stub = env.ns.get(env.ns.idFromName('short'));
    strictEqual(await stub.createUnobservedPromise(), 'created');

    // As in the previous test, settle the promise once the actor is idle.
    await scheduler.wait(100);
    globalThis.unobservedResolve('unobserved-value');
    globalThis.unobservedResolve = undefined;
    await scheduler.wait(100);
    strictEqual(globalThis.unobservedValue, undefined);

    // The next event ends at once, but its request still runs the settlement before finishing.
    strictEqual(await stub.ping(), 'pong');
    strictEqual(
      await waitFor('the settlement to run', () => globalThis.unobservedValue),
      'unobserved-value'
    );
  },
};

export const anIdleActorQueuesOneSubscriptionRequestPerSignal = {
  async test(_, env) {
    const receiver = env.ns.get(env.ns.idFromName('signal-receiver'));
    const producer = env.ns.get(env.ns.idFromName('signal-producer'));
    const controller = new AbortController();
    strictEqual(await receiver.holdSignal(controller.signal), 'held');
    const signal = globalThis.heldSignal;

    // Aborting drops the capability that keeps the receiver's request open. The receiver doesn't
    // observe the signal, so observers added later still need it to arm the subscription.
    controller.abort('done');
    await waitFor('the abort to reach the receiver', () =>
      signal.aborted ? true : undefined
    );

    // Once the receiver is idle, the producer's observers queue one request there to arm the
    // subscription, and it stays queued.
    await scheduler.wait(100);
    strictEqual(await producer.churnHeldSignalListener(100), 1);
    await scheduler.wait(100);
    strictEqual(signal.getRpcReceiverQueuedActionCountForTest(), 1);

    // The producer's later requests and registrations don't add to it.
    for (let i = 0; i < 4; i++) {
      strictEqual(await producer.churnHeldSignalListener(100), 1);
    }

    // The receiver's next event arms the subscription, delivering the abort.
    const aborted = new Promise((resolve) => {
      signal.addEventListener('abort', () => resolve(signal.reason));
    });
    strictEqual(await receiver.ping(), 'pong');
    strictEqual(await aborted, 'done');
    strictEqual(signal.getRpcReceiverQueuedActionCountForTest(), 0);
  },
};

export const settlementWaitsForACriticalSectionToEnd = {
  async test(_, env) {
    const pending = env.ns
      .get(env.ns.idFromName('critical'))
      .settleDuringCriticalSection();
    const resolve = await waitFor(
      'the critical section to start',
      () => globalThis.criticalResolve
    );
    globalThis.criticalResolve = undefined;
    resolve();
    globalThis.criticalResolved = true;
    deepStrictEqual(await pending, ['critical section done', 'settled']);
  },
};
