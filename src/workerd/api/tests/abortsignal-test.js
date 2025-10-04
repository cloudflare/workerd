import { strictEqual, ok, throws, rejects } from 'node:assert';
import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';

// Test for the AbortSignal and AbortController standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

class WrappedAbortSignal extends RpcTarget {
  constructor() {
    super();
    this.ac = new AbortController();
  }

  forget() {
    this.ac.signal.skipReleaseForTest();
  }

  getSignal() {
    return this.ac.signal;
  }
}

let globalAbortController;
export class RpcRemoteEnd extends WorkerEntrypoint {
  async echo(signal) {
    return signal;
  }

  async countToInfinity(signal) {
    let onAbortWasFired = false;

    signal.onabort = () => {
      onAbortWasFired = true;
    };

    for (let i = 0; ; i++) {
      await scheduler.wait(50);
      if (signal.aborted) {
        return { counter: i, reason: signal.reason, onAbortWasFired };
      }
    }
  }

  async countToInfinityWithRequest(req) {
    return this.countToInfinity(req.signal);
  }

  async countToInfinityWithTimeout(remoteSignal) {
    let timeout = AbortSignal.timeout(1000);
    let signal = AbortSignal.any([timeout, remoteSignal]);
    return this.countToInfinity(signal);
  }

  async ignoreSignal(signal) {
    let i = 0;

    for (i = 0; i < 10; i++) {
      await scheduler.wait(50);
    }

    return { counter: i, reason: signal.reason };
  }

  async chainReaction(signal) {
    let onAbortWasFired = false;

    signal.onabort = () => {
      onAbortWasFired = true;
    };

    const inner = await this.env.RpcRemoteEnd.countToInfinity(signal);
    return { inner, reason: signal.reason, onAbortWasFired };
  }

  async tryUsingGlobalAbortController() {
    if (globalAbortController === undefined) {
      globalAbortController = new AbortController();
      await this.env.RpcRemoteEnd.echo(globalAbortController.signal); // send the signal over
    } else {
      globalAbortController.abort(new Error('boom?'));
    }
  }

  async getWrappedSignal() {
    return new WrappedAbortSignal();
  }
}

export const abortcontroller = {
  test() {
    // AbortSignal is not directly creatable
    throws(() => new AbortSignal());

    const ac = new AbortController();
    ok(ac.signal instanceof AbortSignal);
    strictEqual(ac.signal.aborted, false);

    // every call to ac.signal should always be the same value.
    strictEqual(ac.signal, ac.signal);

    // signal is read only
    throws(() => (ac.signal = 1));

    let invoked = 0;
    ac.signal.onabort = (event) => {
      invoked++;
      strictEqual(event.type, 'abort');
    };

    // Will not throw because the signal is not aborted
    ac.signal.throwIfAborted();

    // reason and aborted are read only
    throws(() => (ac.signal.reason = 1));
    throws(() => (ac.signal.aborted = 'foo'));

    // trigger our abort with a default reason...
    ac.abort();

    // This one shouldn't get called since it is added after the abort
    ac.signal.addEventListener('abort', () => {
      throw new Error('should not have been called');
    });

    // Will throw because the signal is now aborted.
    throws(() => ac.signal.throwIfAborted());

    strictEqual(ac.signal.aborted, true);
    strictEqual(ac.signal.reason.message, 'The operation was aborted');
    strictEqual(ac.signal.reason.name, 'AbortError');

    // Abort can be called multiple times with no effect.
    ac.abort();

    strictEqual(invoked, 1);
  },
};

export const abortcontrollerWithReason = {
  test() {
    const ac = new AbortController();
    ok(ac.signal instanceof AbortSignal);
    strictEqual(ac.signal.aborted, false);

    let invoked = 0;

    ac.signal.addEventListener('abort', (event) => {
      invoked++;
      strictEqual(ac.signal.reason, 'foo');
    });

    ac.abort('foo');
    strictEqual(ac.signal.aborted, true);
    strictEqual(ac.signal.reason, 'foo');

    strictEqual(invoked, 1);
  },
};

export const alreadyAborted = {
  test() {
    const aborted = AbortSignal.abort();
    strictEqual(aborted.aborted, true);
    throws(() => aborted.throwIfAborted());

    const abortedWithReason = AbortSignal.abort('foo');
    strictEqual(abortedWithReason.aborted, true);
    try {
      abortedWithReason.throwIfAborted();
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err, 'foo');
    }
  },
};

export const timedAbort = {
  async test() {
    const timed = AbortSignal.timeout(100);
    let resolve;
    const promise = new Promise((r) => (resolve = r));
    let invoked = 0;
    timed.onabort = () => {
      invoked++;
      resolve();
    };
    await promise;
    strictEqual(invoked, 1);
  },
};

export const anyAbort = {
  async test() {
    // Set a timeout way in the future so this one doesn't happen first.
    const timed = AbortSignal.timeout(1000000);
    const ac = new AbortController();

    // Creates an AbortSignal that will be triggered when either of the two
    // given signals is triggered.
    const any = AbortSignal.any([timed, ac.signal]);

    let invoked = 0;
    any.onabort = () => {
      invoked++;
    };

    ac.abort();

    strictEqual(invoked, 1);
  },
};

export const anyAbort2 = {
  async test() {
    const timed = AbortSignal.timeout(100);
    const ac = new AbortController();
    const any = AbortSignal.any([timed, ac.signal]);

    let invoked = 0;
    let resolve;
    const promise = new Promise((r) => (resolve = r));

    any.onabort = () => {
      invoked++;
      resolve();
    };

    await promise;

    strictEqual(invoked, 1);
  },
};

export const anyAbort3 = {
  async test() {
    const timed = AbortSignal.timeout(1000000);
    const aborted = AbortSignal.abort(123);
    // If one of the signals is already abort, the any signal will be
    // immediately aborted also.
    const any = AbortSignal.any([timed, aborted]);
    strictEqual(any.aborted, true);
    strictEqual(any.reason, 123);
  },
};

function initAny(signal, resolve) {
  const any = AbortSignal.any([signal]);
  any.onabort = () => {
    resolve();
  };
}

export const anyAbort4 = {
  async test() {
    // Reproduces a failure seen under asan.
    const ac = new AbortController();
    ac.signal.addEventListener('abort', (event) => {});
    const { promise, resolve } = Promise.withResolvers();

    // Set up AbortSignal.any() to call "resolve" when ac.signal aborts.  We use a separate
    // function to avoid accidentally capturing references in this scope.
    initAny(ac.signal, resolve);

    gc();
    ac.abort();
    await promise;
  },
};

export const onabortPrototypeProperty = {
  test() {
    const ac = new AbortController();
    ok('onabort' in AbortSignal.prototype);
    strictEqual(ac.signal.onabort, null);
    delete ac.signal.onabort;
    ok('onabort' in AbortSignal.prototype);
    strictEqual(ac.signal.onabort, null);
    let called = false;
    ac.signal.onabort = () => {
      called = true;
    };
    ac.abort();
    ok(called);

    // Setting the value to something other than a function or object
    // should cause the value to become null.
    [123, null, 'foo'].forEach((v) => {
      ac.signal.onabort = () => {};
      ac.signal.onabort = v;
      strictEqual(ac.signal.onabort, null);
    });

    const handler = {};
    ac.signal.onabort = handler;
    strictEqual(ac.signal.onabort, handler);
  },
};

export const rpcUnusedSignal = {
  async test(ctrl, env, ctx) {
    const ac = new AbortController();
    const responseSignal = await env.RpcRemoteEnd.echo(ac.signal);

    ok(responseSignal instanceof AbortSignal);
    strictEqual(responseSignal.aborted, false);
    strictEqual(responseSignal.reason, undefined);
  },
};

export const rpcNeverAbortsSignal = {
  async test(ctrl, env, ctx) {
    const otherRequest = new Request('http://example.com');

    const responseSignal = await env.RpcRemoteEnd.echo(otherRequest.signal);
    ok(responseSignal instanceof AbortSignal);
    strictEqual(responseSignal.aborted, false);
    strictEqual(responseSignal.reason, undefined);
  },
};

export const rpcAbortSignalTimeout = {
  async test(ctrl, env, ctx) {
    const signal = AbortSignal.timeout(200);
    const res = await env.RpcRemoteEnd.countToInfinity(signal);

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.counter >= 1);

    // Make sure the reason was passed without being garbled
    ok(res.reason instanceof DOMException);
    strictEqual(res.reason.message, 'The operation was aborted due to timeout');

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
  },
};

export const rpcAbortSignalAbort = {
  async test(ctrl, env, ctx) {
    // NB: AbortSignal.abort returns an abort signal that is already aborted
    const expectedReason = "just didn't feel like it";
    const signal = AbortSignal.abort(expectedReason);
    const res = await env.RpcRemoteEnd.countToInfinity(signal);

    // No iterations should have happened
    strictEqual(res.counter, 0);

    // Make sure the reason was passed without being garbled
    strictEqual(res.reason, "just didn't feel like it");

    // No event is dispatched on an already aborted signal
    ok(!res.onAbortWasFired);
  },
};

export const rpcAbortControllerSignal = {
  async test(ctrl, env, ctx) {
    const ac = new AbortController();
    const resPromise = env.RpcRemoteEnd.countToInfinity(ac.signal);

    // Wait an arbitrary amount of time, then use the AbortController to abort the remote end.
    await scheduler.wait(200);
    const expectedReason = 'changed my mind';
    ac.abort(expectedReason);

    const res = await resPromise;

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.counter >= 1);

    // Make sure the reason was passed without being garbled
    strictEqual(res.reason, expectedReason);

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
  },
};

export const rpcAbortControllerSignalNoReasonProvided = {
  async test(ctrl, env, ctx) {
    const ac = new AbortController();
    const resPromise = env.RpcRemoteEnd.countToInfinity(ac.signal);

    // Wait an arbitrary amount of time, then use the AbortController to abort the remote end.
    await scheduler.wait(200);
    ac.abort();

    const res = await resPromise;

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.counter >= 1);

    // Make sure the reason was passed without being garbled
    ok(res.reason instanceof DOMException);
    strictEqual(res.reason.message, 'The operation was aborted');

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
  },
};

export const rpcAbortSignalFurtherCloned = {
  async test(ctrl, env, ctx) {
    const ac = new AbortController();
    const resPromise = env.RpcRemoteEnd.chainReaction(ac.signal);

    // Wait an arbitrary amount of time, then use the AbortController to abort the remote end.
    await scheduler.wait(200);
    const expectedReason = 'changed my mind';
    ac.abort(expectedReason);

    const res = await resPromise;

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.inner.counter >= 1);

    // Make sure the reason was passed without being garbled
    strictEqual(res.reason, expectedReason);
    strictEqual(res.inner.reason, expectedReason);

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
    ok(res.inner.onAbortWasFired);
  },
};

export const rpcAbortSignalManyClients = {
  async test(ctrl, env, ctx) {
    const signal = AbortSignal.timeout(200);

    const responses = await Promise.all(
      Array.from({ length: 5 }, () => env.RpcRemoteEnd.countToInfinity(signal))
    );
    strictEqual(responses.length, 5);

    for (const res of responses) {
      // We don't care the exact value it got to, but at least 1 iteration should have happened
      ok(res.counter >= 1);

      // Make sure the reason was passed without being garbled
      ok(res.reason instanceof DOMException);
      strictEqual(
        res.reason.message,
        'The operation was aborted due to timeout'
      );

      // Make sure an event was dispatched on the remote side
      ok(res.onAbortWasFired);
    }
  },
};

export const rpcAbortSignalAny = {
  async test(ctrl, env, ctx) {
    const unusedAc = new AbortController();
    const signal = AbortSignal.any([AbortSignal.timeout(200), unusedAc.signal]);
    const res = await env.RpcRemoteEnd.countToInfinity(signal);

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.counter >= 1);

    // Make sure the reason was passed without being garbled
    ok(res.reason instanceof DOMException);
    strictEqual(res.reason.message, 'The operation was aborted due to timeout');

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
  },
};

export const rpcAbortSignalAnyOnRemoteEnd = {
  async test(ctrl, env, ctx) {
    const ac = new AbortController();
    const resPromise = env.RpcRemoteEnd.countToInfinityWithTimeout(ac.signal);

    // Wait an arbitrary amount of time, then use the AbortController to abort the remote end.
    await scheduler.wait(200);
    const expectedReason =
      'our timeout triggered before the 1000ms timeout on the other side';
    ac.abort(expectedReason);

    const res = await resPromise;

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.counter >= 1);

    // Make sure the reason was passed without being garbled
    strictEqual(res.reason, expectedReason);

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
  },
};

export const rpcRequestSignal = {
  async test(ctrl, env, ctx) {
    // Construct a request holding an AbortSignal, and then send this request to the other side
    // Note that this signal isn't affected by the request_signal_passthrough compat flag, which
    // only modifies the behaviour of the signal on the incoming request.
    const req = new Request('http://example.com', {
      signal: AbortSignal.timeout(200),
    });

    const res = await env.RpcRemoteEnd.countToInfinityWithRequest(req);

    // We don't care the exact value it got to, but at least 1 iteration should have happened
    ok(res.counter >= 1);

    // Make sure the reason was passed without being garbled
    ok(res.reason instanceof DOMException);
    strictEqual(res.reason.message, 'The operation was aborted due to timeout');

    // Make sure an event was dispatched on the remote side
    ok(res.onAbortWasFired);
  },
};

export const rpcCrossRequestSignal = {
  async test(ctrl, env, ctx) {
    // Save an AbortController in the global scope
    await env.RpcRemoteEnd.tryUsingGlobalAbortController();

    // Try to use it again
    await rejects(
      async () => env.RpcRemoteEnd.tryUsingGlobalAbortController(),
      {
        name: 'Error',
        message:
          "Cannot perform I/O on behalf of a different request. I/O objects (such as streams, request/response bodies, and others) created in the context of one request handler cannot be accessed from a different request's handler. This is a limitation of Cloudflare Workers which allows us to improve overall performance. (I/O type: RefcountedCanceler)",
      }
    );
  },
};

export const rpcRemoteCanIgnoreSignal = {
  async test(ctrl, env, ctx) {
    const ac = new AbortController();
    const resPromise = env.RpcRemoteEnd.ignoreSignal(ac.signal);

    // Wait an arbitrary amount of time, then use the AbortController to abort the remote end.
    await scheduler.wait(200);
    const expectedReason = 'changed my mind';
    ac.abort(expectedReason);

    const res = await resPromise;

    // Every iteration completes, the remote is not reacting to the abort
    strictEqual(res.counter, 10);

    // Make sure the reason was passed without being garbled
    strictEqual(res.reason, expectedReason);
  },
};

export const rpcDestroySignalUnclean = {
  async test(ctrl, env, ctx) {
    // wrapper is a RPCTarget that just holds an AbortSignal
    const wrapper = await env.RpcRemoteEnd.getWrappedSignal();

    // Get our clone of the signal
    const signal = await wrapper.getSignal();

    // Tell the AbortSignal not to send a release message on disposal
    await wrapper.forget();

    // Destroy the wrapper
    wrapper[Symbol.dispose]();

    // No release message was sent, our clone will provide a message explaining the other side is
    // gone.
    ok(signal.aborted);
    strictEqual(
      signal.reason.message,
      'An AbortSignal received over RPC was implicitly aborted because the connection back to its ' +
        'trigger was lost.'
    );
  },
};

export const rpcDestroySignalClean = {
  async test(ctrl, env, ctx) {
    // wrapper is a RPCTarget that just holds an AbortSignal
    const wrapper = await env.RpcRemoteEnd.getWrappedSignal();

    // Get our clone of the signal
    const signal = await wrapper.getSignal();

    // Destroy the wrapper
    wrapper[Symbol.dispose]();

    // A release message was sent, the signal will remain in an unaborted state
    ok(!signal.aborted);
    strictEqual(signal.reason, undefined);
  },
};
