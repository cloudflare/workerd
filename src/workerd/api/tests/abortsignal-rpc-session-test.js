// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

// Polls the tail worker while the calling test keeps its AbortController, and so the trigger for
// the signal it sent, alive. The callee's tail event arrives only once its session has ended.
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

// An abort sent after the callee's request ended has no ordering relative to later calls, so poll
// until `getState()` reports it.
async function waitForState(getState, done) {
  for (let i = 0; i < 1000; i++) {
    const state = await getState();
    if (done(state)) return state;
    await scheduler.wait(10);
  }
  assert.fail('abort never arrived');
}

export const unusedSignalDoesNotHoldCallee = {
  async test(ctrl, env) {
    const ac = new AbortController();
    assert.strictEqual(await env.CALLEE.ignoresSignal(ac.signal), 'ok');
    await assertCalleeSucceeded(env, 'ignoresSignal');
    assert.strictEqual(ac.signal.aborted, false);
  },
};

// The callee sends the signal back, which pushes a new signal into the caller and leaves the
// callee holding its trigger.
export const echoedSignalDoesNotHoldCallee = {
  async test(ctrl, env) {
    const ac = new AbortController();
    const echoed = await env.CALLEE.echoesSignal(ac.signal);
    assert.ok(echoed instanceof AbortSignal);
    assert.strictEqual(echoed.aborted, false);
    await assertCalleeSucceeded(env, 'echoesSignal');
  },
};

export const unusedSignalDoesNotHoldActor = {
  async test(ctrl, env) {
    const ac = new AbortController();
    const actor = env.ACTOR.get(env.ACTOR.idFromName('ignores'));
    assert.strictEqual(await actor.actorIgnoresSignal(ac.signal), 'ok');
    await assertCalleeSucceeded(env, 'actorIgnoresSignal');
    assert.strictEqual(ac.signal.aborted, false);
  },
};

// The callee keeps the signal after returning. Its request, and the IoContext that received the
// signal, are gone by the time of the abort, which still reaches the signal's state.
export const abortAfterCalleeReturned = {
  async test(ctrl, env) {
    const ac = new AbortController();
    assert.strictEqual(await env.CALLEE.holdsSignal(ac.signal), 'ok');
    await assertCalleeSucceeded(env, 'holdsSignal');

    ac.abort('after return');
    const state = await waitForState(
      () => env.CALLEE.heldSignalState(),
      (s) => s.aborted
    );
    assert.strictEqual(state.reason, 'after return');
  },
};

export const abortAfterActorRequestEnded = {
  async test(ctrl, env) {
    const ac = new AbortController();
    const actor = env.ACTOR.get(env.ACTOR.idFromName('holds'));
    assert.strictEqual(await actor.actorHoldsSignal(ac.signal), 'ok');
    await assertCalleeSucceeded(env, 'actorHoldsSignal');

    ac.abort('after actor return');
    const state = await waitForState(
      () => actor.actorHeldSignalState(),
      (s) => s.aborted
    );
    assert.deepStrictEqual(state, {
      aborted: true,
      reason: 'after actor return',
      abortedWith: undefined,
    });
  },
};

export const abortAfterActorMethodReturnedFiresListener = {
  async test(ctrl, env) {
    const ac = new AbortController();
    const actor = env.ACTOR.get(env.ACTOR.idFromName('listens'));
    assert.strictEqual(await actor.actorListensForAbort(ac.signal), 'ok');

    ac.abort('after listener');
    const state = await waitForState(
      () => actor.actorHeldSignalState(),
      (s) => s.abortedWith !== undefined
    );
    assert.deepStrictEqual(state, {
      aborted: true,
      reason: 'after listener',
      abortedWith: 'after listener',
    });
    await assertCalleeSucceeded(env, 'actorListensForAbort');
  },
};

// A pending abort listener doesn't keep the actor's request open: its outcome is reported before
// the abort, which then still fires the listener.
export const pendingListenerDoesNotHoldActor = {
  async test(ctrl, env) {
    const ac = new AbortController();
    const actor = env.ACTOR.get(env.ACTOR.idFromName('listens-no-hold'));
    assert.strictEqual(await actor.actorListensForOnabort(ac.signal), 'ok');
    await assertCalleeSucceeded(env, 'actorListensForOnabort');

    ac.abort('after outcome');
    const state = await waitForState(
      () => actor.actorHeldSignalState(),
      (s) => s.abortedWith !== undefined
    );
    assert.deepStrictEqual(state, {
      aborted: true,
      reason: 'after outcome',
      abortedWith: 'after outcome',
    });
  },
};

// Resetting the actor destroys the IoContext that received the signal before the abort.
export const abortAfterActorReset = {
  async test(ctrl, env) {
    const ac = new AbortController();
    const id = env.ACTOR.idFromName('reset');
    let actor = env.ACTOR.get(id);
    assert.strictEqual(await actor.actorListensForAbort(ac.signal), 'ok');
    await assert.rejects(actor.actorReset(), { message: /actor reset/ });

    ac.abort('after reset');
    // The abort is sent as a task of this request; give it a turn to go out.
    await scheduler.wait(0);

    actor = env.ACTOR.get(id);
    assert.strictEqual(await actor.actorIgnoresSignal(undefined), 'ok');
  },
};
