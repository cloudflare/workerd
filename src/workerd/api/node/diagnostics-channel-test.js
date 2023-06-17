import {
  ok,
  strictEqual,
} from 'node:assert';

import {
  hasSubscribers,
  channel,
  subscribe,
  unsubscribe,
  tracingChannel,
  Channel,
} from 'node:diagnostics_channel';

import {
  AsyncLocalStorage,
} from 'node:async_hooks';

function deferredPromise() {
  let resolve, reject;
  const promise = new Promise((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

export const test_basics = {
  async test(ctrl, env, ctx) {
    ok(!hasSubscribers('foo'));
    const channel1 = channel('foo');
    const channel2 = channel('foo');
    strictEqual(channel1, channel2);
    ok(channel1 instanceof Channel);

    const messagePromise = deferredPromise();

    const listener = (message) => {
      try {
        strictEqual(message, 'hello');
        messagePromise.resolve();
      } catch (err) {
        messagePromise.reject(err);
      }
    };

    subscribe('foo', listener);

    ok(hasSubscribers('foo'));

    channel1.publish('hello');

    unsubscribe('foo', listener);

    ok(!hasSubscribers('foo'));

    await messagePromise.promise;
  }
};

export const test_tracing = {
  async test(ctrl, env, ctx) {
    const tc = tracingChannel('bar');
    ok(tc.start instanceof Channel);
    ok(tc.end instanceof Channel);
    ok(tc.asyncStart instanceof Channel);
    ok(tc.asyncEnd instanceof Channel);
    ok(tc.error instanceof Channel);

    const als = new AsyncLocalStorage();
    tc.start.bindStore(als);

    const promises = [
      deferredPromise(),
      deferredPromise(),
      deferredPromise(),
      deferredPromise(),
      deferredPromise(),
    ];

    const context = {};

    tc.subscribe({
      start(_, name) {
        try {
          // Since the als is bound to tc.start, the context should be
          // propagated here to the listener.
          strictEqual(als.getStore(), context);
          strictEqual(name, 'tracing:bar:start');
          promises[0].resolve();
        } catch (err) {
          promises[0].reject(err);
        }
      },
      end(_, name) {
        try {
          // Since the als is bound to tc.start, the context should be
          // propagated here to the other listeners even if they aren't
          // explicitly bound to als.
          strictEqual(als.getStore(), context);
          strictEqual(name, 'tracing:bar:end');
          promises[1].resolve();
        } catch (err) {
          promises[1].reject(err);
        }
      },
      asyncStart() { promises[2].resolve(); },
      asyncEnd() { promises[3].resolve(); },
      error() { promises[4].resolve(); },
    });

    tc.tracePromise(async () => {
      throw new Error('boom');
    }, context);

    await Promise.all(promises.map(p => p.promise));
  }
};
