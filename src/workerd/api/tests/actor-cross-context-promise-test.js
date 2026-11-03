// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for promises that an actor event creates and another IoContext settles later. The
// settlement runs back in the actor's IoContext, which outlives each event's IncomingRequest, and
// needs one: the continuations use timers and storage. Each case settles while the actor has a
// request, so it behaves the same with or without the DEFER_CROSS_CONTEXT_ACTIONS autogate, which
// the @all-autogates variant enables. Settlements on an idle actor are tested in io-own-test.c++,
// where a test can make an actor idle at a known point.

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
