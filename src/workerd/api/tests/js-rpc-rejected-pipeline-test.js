// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { RpcTarget } from 'cloudflare:workers';

// Keeps rejected promises reachable so that GC cannot release their pipelines
// and hide a pipeline that the runtime failed to drop.
const held = [];

class Step extends RpcTarget {
  async fail() {
    throw new Error('step failed');
  }

  async retry(callback) {
    const first = callback();
    held.push(first);
    await assert.rejects(first, { message: 'first attempt fails' });
    return await callback();
  }
}

// Polls the tail worker while this test stays alive and keeps holding its rejected promises.
// A pinned pipeline leaves the callee with nothing to wait on, so it is aborted as hung.
async function assertCalleeSucceeded(env, method) {
  for (let i = 0; i < 1000; i++) {
    const result = await env.RESULTS.outcome(method);
    if (result !== undefined) {
      assert.deepStrictEqual(result, { outcome: 'ok', exceptions: [] });
      return;
    }
    await scheduler.wait(10);
  }
  assert.fail(`no tail event for ${method}`);
}

export const callerHoldsRejectedCall = {
  async test(ctrl, env) {
    const promise = env.CALLEE.throwsDirectly();
    held.push(promise);
    await assert.rejects(promise, { message: 'boom from callee' });
    await assertCalleeSucceeded(env, 'throwsDirectly');

    // Pipelining on the rejected promise reports the call's error, both when
    // awaiting a property and when calling a method through it.
    await assert.rejects(Promise.resolve(promise.foo), {
      message: 'boom from callee',
    });
    await assert.rejects(promise.foo.bar(), { message: 'boom from callee' });
  },
};

export const pipeliningIgnoresMutatedRejection = {
  async test(ctrl, env) {
    const promise = env.CALLEE.throwsDirectly();
    held.push(promise);
    const error = await promise.then(
      () => assert.fail('call unexpectedly succeeded'),
      (e) => e
    );

    // `error` is the object the call rejected with. Changing it afterwards must not change the
    // error that operations pipelined on the rejected promise report.
    error.message = 'mutated by caller';
    await assert.rejects(promise.foo.bar(), { message: 'boom from callee' });
  },
};

// Converting a rejection reads `retryable` from the error, so a getter there runs application
// code. Collecting the RpcPromise from that getter must not lead to a use-after-free.
export const promiseCollectedWhileConvertingRejection = {
  async test(ctrl, env) {
    let promise = env.CALLEE.throwsDirectly();
    let collectedDuringConversion = false;
    Object.defineProperty(Object.prototype, 'retryable', {
      configurable: true,
      get() {
        // `promise` holds the last reference to the RpcPromise. Dropping it here lets gc()
        // destroy the RpcPromise while its rejection is still being converted.
        if (promise !== null && this.message === 'boom from callee') {
          promise = null;
          gc();
          collectedDuringConversion = true;
        }
        return undefined;
      },
    });
    try {
      const error = await promise.then(
        () => assert.fail('call unexpectedly succeeded'),
        (e) => e
      );
      assert.strictEqual(error.message, 'boom from callee');
      assert.ok(
        collectedDuringConversion,
        'rejection conversion never read `retryable`'
      );
    } finally {
      delete Object.prototype.retryable;
    }
  },
};

// Converting a rejection reads `overloaded` from the error. A getter there that throws must not
// replace the error the caller sees, nor leave the call's pipeline held.
export const throwingAccessorWhileConvertingRejection = {
  async test(ctrl, env) {
    const message = 'boom with throwing accessor';
    let accessorCalls = 0;
    Object.defineProperty(Object.prototype, 'overloaded', {
      configurable: true,
      get() {
        if (this.message !== message) return undefined;
        accessorCalls++;
        throw new Error('accessor threw');
      },
    });
    try {
      const promise = env.CALLEE.throwsWithMessage(message);
      held.push(promise);
      await assert.rejects(promise, { message });
      assert.ok(
        accessorCalls > 0,
        'rejection conversion never read `overloaded`'
      );
      await assertCalleeSucceeded(env, 'throwsWithMessage');
      await assert.rejects(promise.foo.bar(), {
        message:
          'The RPC call failed with an error that could not be serialized.',
      });
    } finally {
      delete Object.prototype.overloaded;
    }
  },
};

export const callerHoldsRejectedActorCall = {
  async test(ctrl, env) {
    const actor = env.ACTOR.get(env.ACTOR.idFromName('actor'));
    const promise = actor.actorThrowsDirectly();
    held.push(promise);
    await assert.rejects(promise, { message: 'boom from actor' });
    await assertCalleeSucceeded(env, 'actorThrowsDirectly');
    await assert.rejects(promise.foo.bar(), { message: 'boom from actor' });
  },
};

export const calleeHoldsRejectedCallOnCallerStub = {
  async test(ctrl, env) {
    assert.strictEqual(await env.CALLEE.rejectsOnCallerStub(new Step()), 'ok');
    await assertCalleeSucceeded(env, 'rejectsOnCallerStub');
  },
};

export const callerHoldsRejectedCallback = {
  async test(ctrl, env) {
    assert.strictEqual(await env.CALLEE.retriesCallback(new Step()), 'ok');
    await assertCalleeSucceeded(env, 'retriesCallback');
  },
};
