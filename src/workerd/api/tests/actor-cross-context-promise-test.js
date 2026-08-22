// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Durable Object keeps a single IoContext for its whole lifetime, but gets a separate
// IncomingRequest for each event delivered to it. These tests cover promises that an actor event
// creates and that some other IoContext settles later, which routes the settlement back through the
// actor's delete queue.
//
// Settling such a promise runs application JavaScript, which needs the timer, metrics, tracing and
// IoChannelFactory that only a current IncomingRequest provides. Every continuation below therefore
// reaches for those services, so a settlement processed without a request would fail rather than
// quietly appear to work.

import { DurableObject } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';

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

// Exercises the request-scoped services. Date.now() reads the request's timer, scheduler.wait()
// schedules against it, and storage records through the actor's cache.
async function useRequestScopedApis(ctx, key, value) {
  const startedAt = Date.now();
  await scheduler.wait(1);
  await ctx.storage.put(key, value);
  return {
    value,
    stored: await ctx.storage.get(key),
    clockWorks: Date.now() >= startedAt,
  };
}

export class TestActor extends DurableObject {
  // Holds its event open by awaiting the promise. No further event is coming, so the settlement has
  // to be processed under this request.
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
    strictEqual(result.clockWorks, true);
  },
};

export const settlementRunsUnderADrainingRequest = {
  async test(_, env) {
    const stub = env.ns.get(env.ns.idFromName('retained'));
    strictEqual(await stub.createRetainedPromise(), 'created');

    const resolve = globalThis.retainedResolve;
    globalThis.retainedResolve = undefined;
    resolve('retained-value');

    // The actor receives no further event. Its waitUntil task is the only thing keeping the request
    // alive, so the settlement has to be processed while that request drains.
    const result = await waitFor(
      'the retained continuation to run',
      () => globalThis.retainedResult
    );
    strictEqual(result.value, 'retained-value');
    strictEqual(result.stored, 'retained-value');
    strictEqual(result.clockWorks, true);
  },
};

export const settlementWaitsForTheNextEventWhenTheActorIsIdle = {
  async test(_, env) {
    const stub = env.ns.get(env.ns.idFromName('detached'));
    strictEqual(await stub.createDetachedPromise(), 'created');

    // Nothing retains that event's request, so it drains once the RPC session ends. Wait long
    // enough that it is certainly gone before settling the promise.
    await scheduler.wait(100);

    globalThis.detachedResolve('detached-value');
    globalThis.detachedResolve = undefined;

    // With no request to run under, the settlement stays queued rather than executing JavaScript in
    // a context that has no timer, metrics or I/O channels.
    await scheduler.wait(100);
    strictEqual(globalThis.detachedReactionRan, undefined);

    // The actor is still healthy, and its next event picks the settlement up.
    const result = await stub.getDetachedResult();
    strictEqual(globalThis.detachedReactionRan, true);
    strictEqual(result.value, 'detached-value');
    strictEqual(result.stored, 'detached-value');
    strictEqual(result.clockWorks, true);
  },
};
