// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';
import { scheduler } from 'node:timers/promises';

// Read from the runtime rather than a binding, so the all-compat-flags variant stays correct.
const CANCELS =
  globalThis.Cloudflare.compatibilityFlags['rpc_dispose_cancels_call'];

// How long the callee's `hang()` waits before it finishes.
const HANG_MS = 100;

// How long `hangWithProbe()` waits, long enough that its return can't release the probe in time.
const LONG_HANG_MS = 10_000;

// The state of each `hang()` call, by the ID the caller passed.
const calls = new Map();

class Hanger extends RpcTarget {
  hang(id) {
    return hang(id);
  }
}

async function hang(id) {
  calls.set(id, 'started');
  await scheduler.wait(HANG_MS);
  calls.set(id, 'finished');
  return 'done';
}

// Passed to a callee, which calls `start()` once it's running. Disposed once the callee's stub
// to it is.
class Probe extends RpcTarget {
  started = Promise.withResolvers();
  disposed = Promise.withResolvers();

  start() {
    this.started.resolve();
  }

  [Symbol.dispose]() {
    this.disposed.resolve();
  }
}

export class Callee extends WorkerEntrypoint {
  hang(id) {
    return hang(id);
  }

  // The callee holds `callback` until the method returns.
  hangWithCallback(id, _callback) {
    return hang(id);
  }

  // Holds `probe` until the method returns.
  async hangWithProbe(probe) {
    await probe.start();
    await scheduler.wait(LONG_HANG_MS);
  }

  getHanger() {
    return new Hanger();
  }
}

// Starts a call to `target.hang()`, or to whatever `start()` calls, and waits until the callee is
// running it. The call's promise is wrapped in an object because returning it from an async
// function would await it.
//
// The callee shares this isolate, so the tests watch its progress through `calls`.
async function startHang(target, id, start = () => target.hang(id)) {
  const call = start();
  while (calls.get(id) !== 'started') {
    await scheduler.wait(1);
  }
  return { call };
}

// JsRpcPromise is callable, so `assert.rejects()` would call it rather than await it.
function awaitCall(promise) {
  return (async () => await promise)();
}

function isAbortError(error) {
  assert.strictEqual(
    `${error.name}: ${error.message}`,
    'AbortError: The RPC call was canceled because its promise was disposed.'
  );
  return true;
}

export const disposeCancelsCall = {
  async test(_controller, env) {
    const { call: promise } = await startHang(env.SELF, 'dispose');
    promise[Symbol.dispose]();

    if (CANCELS) {
      await assert.rejects(awaitCall(promise), isAbortError);
      // The canceled call ended the callee's request, so its timer never fires.
      await scheduler.wait(HANG_MS * 3);
      assert.strictEqual(calls.get('dispose'), 'started');
    } else {
      assert.strictEqual(await promise, 'done');
      assert.strictEqual(calls.get('dispose'), 'finished');
    }
  },
};

// Canceling the call disposes the stubs the caller passed as arguments, so the callee holding
// them doesn't keep the session, and with it the callee's request, open.
export const disposeCancelsCallHoldingCallback = {
  async test(_controller, env) {
    const { call: promise } = await startHang(env.SELF, 'callback', () =>
      env.SELF.hangWithCallback('callback', () => {})
    );
    promise[Symbol.dispose]();

    if (CANCELS) {
      await assert.rejects(awaitCall(promise), isAbortError);
      await scheduler.wait(HANG_MS * 3);
      assert.strictEqual(calls.get('callback'), 'started');
    } else {
      assert.strictEqual(await promise, 'done');
      assert.strictEqual(calls.get('callback'), 'finished');
    }
  },
};

// How the callee handles a canceled call doesn't depend on its own flags: `UNFLAGGED` is the
// service without the flag, which releases the probe as soon as the call is canceled.
export const disposeCancelsCallToCalleeWithoutFlag = {
  async test(_controller, env) {
    if (!CANCELS) return;
    const probe = new Probe();
    const promise = env.UNFLAGGED.hangWithProbe(probe);
    await probe.started.promise;
    promise[Symbol.dispose]();

    await assert.rejects(awaitCall(promise), isAbortError);
    await Promise.race([
      probe.disposed.promise,
      scheduler.wait(LONG_HANG_MS / 2).then(() => {
        throw new Error('The callee did not release the probe.');
      }),
    ]);
  },
};

export const disposeCancelsCallOnReturnedStub = {
  async test(_controller, env) {
    using hanger = await env.SELF.getHanger();
    const { call: promise } = await startHang(hanger, 'stub');
    promise[Symbol.dispose]();

    if (CANCELS) {
      await assert.rejects(awaitCall(promise), isAbortError);
    } else {
      assert.strictEqual(await promise, 'done');
    }
  },
};

export const usingCancelsWithoutUnhandledRejection = {
  async test(_controller, env) {
    const events = [];
    const handler = (event) => {
      events.push(event);
      event.preventDefault();
    };
    addEventListener('unhandledrejection', handler);
    try {
      {
        using _promise = (await startHang(env.SELF, 'using')).call;
      }
      await scheduler.wait(HANG_MS * 3);
      assert.strictEqual(events.length, 0);
      assert.strictEqual(calls.get('using'), CANCELS ? 'started' : 'finished');
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};

// Once the call has completed, disposing its promise disposes the result, as without the flag.
export const disposeAfterCompletionDisposesResult = {
  async test(_controller, env) {
    const promise = env.SELF.getHanger();
    const hanger = await promise;
    promise[Symbol.dispose]();
    await assert.rejects(awaitCall(hanger.hang('after')), {
      message: 'RPC stub used after being disposed.',
    });
  },
};
