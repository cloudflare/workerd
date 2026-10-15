// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Durable Object keeps a single IoContext for its whole lifetime, but gets a separate
// IncomingRequest for each event delivered to it. These tests cover promises that an actor event
// creates and that some other IoContext settles later, which routes the settlement back through the
// actor's delete queue.
//
// Settling such a promise runs application JavaScript, which needs a current IncomingRequest. The
// continuations below use timers and storage, which both depend on one.

import { DurableObject } from 'cloudflare:workers';
import { deepStrictEqual, strictEqual } from 'node:assert';

// Polls until `read()` returns something. Used instead of a fixed sleep wherever a test is waiting
// for progress rather than for the absence of it.
async function waitFor(what, read) {
  for (let i = 0; i < 2000; i++) {
    const value = read();
    if (value !== undefined) return value;
    await scheduler.wait(1);
  }
  throw new Error(`timed out waiting for ${what}`);
}

// Exercises request-scoped services: scheduler.wait() schedules against the request's timer, and
// storage goes through the actor's cache.
async function useRequestScopedApis(ctx, key, value) {
  await scheduler.wait(1);
  await ctx.storage.put(key, value);
  return { value, stored: await ctx.storage.get(key) };
}

export class TestActor extends DurableObject {
  // Holds its event open by awaiting the promise, so this request is still current when the
  // settlement arrives.
  async awaitFromActiveRequest() {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.activeResolve = resolve;
    const value = await promise;
    return await useRequestScopedApis(this.ctx, 'active', value);
  }

  // Responds immediately but retains the continuation with ctx.waitUntil(), so the request outlives
  // the response and is still current when the settlement arrives.
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

  // Responds without retaining the continuation, so this event's IncomingRequest drains as soon as
  // the RPC session ends and the actor is left with no current request.
  async createDetachedPromise() {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.detachedResolve = resolve;
    this.detached = promise.then(async (value) => {
      // Recorded before the first await, so a test can distinguish "the reaction started" from "the
      // reaction finished".
      globalThis.detachedReactionRan = true;
      return await useRequestScopedApis(this.ctx, 'detached', value);
    });
    return 'created';
  }

  async getDetachedResult() {
    return await this.detached;
  }

  // Publishes a resolver from inside blockConcurrencyWhile(), then keeps the critical section, and
  // with it the input lock, held for a while after the test has settled the promise.
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

    // The actor receives no further event, so the settlement runs while the request drains, kept
    // alive by its waitUntil task.
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

    // Nothing retains that event's request, so it drains once the RPC session ends. Give it time to
    // do so before settling the promise.
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
