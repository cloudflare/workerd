// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';

import {
  hasSubscribers,
  channel,
  subscribe,
  unsubscribe,
  tracingChannel,
  Channel,
} from 'node:diagnostics_channel';

import { AsyncLocalStorage } from 'node:async_hooks';

export const test_basics = {
  async test(ctrl, env, ctx) {
    ok(!hasSubscribers('foo'));
    const channel1 = channel('foo');
    const channel2 = channel('foo');
    strictEqual(channel1, channel2);
    ok(channel1 instanceof Channel);

    const messagePromise = Promise.withResolvers();

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
  },
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
      Promise.withResolvers(),
      Promise.withResolvers(),
      Promise.withResolvers(),
      Promise.withResolvers(),
      Promise.withResolvers(),
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
      asyncStart() {
        promises[2].resolve();
      },
      asyncEnd() {
        promises[3].resolve();
      },
      error() {
        promises[4].resolve();
      },
    });

    tc.tracePromise(async () => {
      throw new Error('boom');
    }, context);

    await Promise.all(promises.map((p) => p.promise));
  },
};

export const serFailureTest = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const channel1 = channel('ser');
    subscribe('ser', () => {
      resolve();
    });
    channel1.publish(function () {});
    await promise;
  },
};

export const DiagChannelSubscribeDuringPublish = {
  async test() {
    const ch = channel('test-publish');
    let callCount = 0;
    let duringPublishCallCount = 0;

    for (let i = 0; i < 32; i++) {
      ch.subscribe(() => {
        callCount++;
      });
    }

    ch.subscribe(() => {
      callCount++;
      for (let i = 0; i < 64; i++) {
        ch.subscribe(() => {
          duringPublishCallCount++;
        });
      }
    });

    for (let i = 0; i < 16; i++) {
      ch.subscribe(() => {
        callCount++;
      });
    }

    ch.publish({ data: 'trigger' });

    strictEqual(callCount, 49);
    strictEqual(duringPublishCallCount, 0);
  },
};

export const DiagChannelUnbindDuringRunStores = {
  async test() {
    const ch = channel('test');
    const als = new AsyncLocalStorage();
    let transformCallCount = 0;

    ch.bindStore(als, (msg) => {
      transformCallCount++;
      ch.unbindStore(als);
      return msg;
    });

    const result = ch.runStores({}, () => 'done');
    strictEqual(result, 'done');
    strictEqual(transformCallCount, 1);

    ch.runStores({}, () => {});
    strictEqual(transformCallCount, 1);
  },
};

// Legacy-method behavior: when the `diagnostics_channel_has_subscribers_getter`
// compat flag is NOT enabled, `Channel.hasSubscribers` and
// `TracingChannel.hasSubscribers` are registered as methods. The associated
// `.wd-test` opts out of the flag explicitly so that this behavior is exercised
// under both the default and `@all-compat-flags` test variants.
export const test_channel_hasSubscribers_is_a_method = {
  async test() {
    const ch = channel('method-test');

    // It is a method, not a boolean property.
    strictEqual(typeof ch.hasSubscribers, 'function');
    strictEqual(ch.hasSubscribers(), false);

    const listener = () => {};
    ch.subscribe(listener);
    strictEqual(ch.hasSubscribers(), true);

    ch.unsubscribe(listener);
    strictEqual(ch.hasSubscribers(), false);

    // Defined on the prototype as a method (value descriptor, no getter).
    const desc = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(ch),
      'hasSubscribers'
    );
    ok(desc);
    strictEqual(typeof desc.value, 'function');
    strictEqual(desc.get, undefined);
    strictEqual(desc.set, undefined);
  },
};

export const test_tracingChannel_hasSubscribers_is_a_method = {
  async test() {
    const tc = tracingChannel('tracing-method-test');

    strictEqual(typeof tc.hasSubscribers, 'function');
    strictEqual(tc.hasSubscribers(), false);

    const listener = () => {};

    // Each sub-channel independently flips the aggregate method result.
    for (const sub of ['start', 'end', 'asyncStart', 'asyncEnd', 'error']) {
      tc[sub].subscribe(listener);
      strictEqual(tc.hasSubscribers(), true, `via ${sub}`);
      tc[sub].unsubscribe(listener);
      strictEqual(tc.hasSubscribers(), false, `after unsubscribing ${sub}`);
    }
  },
};

export const test_subscriber_order_after_unsubscribe = {
  test() {
    // Removing a subscriber moves the last one into its place.
    const order = [];
    const ch = channel('order');
    const [a, b, c, d] = ['a', 'b', 'c', 'd'].map((n) => () => order.push(n));
    ch.subscribe(a);
    ch.subscribe(b);
    ch.subscribe(c);
    ch.subscribe(a);
    ch.unsubscribe(a);
    ch.subscribe(d);
    ch.publish(0);
    strictEqual(order.join(''), 'cbd');
  },
};

export const test_subscriber_arguments = {
  test() {
    const sym = Symbol('sym');
    let seen;
    subscribe(sym, function (message, name) {
      seen = [this, message, name, arguments.length];
    });
    channel(sym).publish('m');
    deepStrictEqual(seen, [globalThis, 'm', sym, 2]);
  },
};

export const test_channel_names = {
  test() {
    // Channels are keyed by the name's string form.
    strictEqual(channel(Symbol('s')), channel(Symbol('s')));
    strictEqual(channel(Symbol('s')), channel('Symbol(s)'));
    strictEqual(channel(1), channel('1'));
  },
};

export const test_argument_errors = {
  test() {
    const ch = channel('errors');
    throws(() => ch.publish(), {
      name: 'TypeError',
      message:
        "Failed to execute 'publish' on 'Channel': parameter 1 is not of type 'Value'.",
    });
    throws(() => ch.subscribe({}), {
      name: 'TypeError',
      message:
        "Failed to execute 'subscribe' on 'Channel': parameter 1 is not of type 'function'.",
    });
    throws(() => ch.bindStore({}), {
      name: 'TypeError',
      message:
        "Failed to execute 'bindStore' on 'Channel': parameter 1 is not of type 'AsyncLocalStorage'.",
    });
    throws(() => subscribe('x', 1), {
      name: 'TypeError',
      message:
        "Failed to execute 'subscribe' on 'DiagnosticsChannelModule': parameter 2 is not of type 'function'.",
    });
    throws(() => Channel.prototype.publish.call({}, 0), {
      name: 'TypeError',
      message: /^Illegal invocation/,
    });
    throws(() => new Channel('x'), {
      name: 'TypeError',
      message: 'Illegal constructor',
    });
  },
};

export const test_to_string_tag = {
  test() {
    const expected = Cloudflare.compatibilityFlags['set_tostring_tag']
      ? '[object Channel]'
      : '[object Object]';
    strictEqual(Object.prototype.toString.call(channel('tag')), expected);
  },
};

export const test_run_stores_order = {
  test() {
    // Each transform runs with the stores bound before it already entered. Unbinding a store
    // moves the last one into its place.
    const stores = [1, 2, 3].map(() => new AsyncLocalStorage());
    const sc = channel('stores');
    const log = [];
    stores.forEach((als, i) =>
      sc.bindStore(als, (message) => {
        log.push(`${i}:${stores[0].getStore()}`);
        return `${message}${i}`;
      })
    );
    sc.unbindStore(stores[1]);
    const result = sc.runStores(
      'm',
      function (x) {
        log.push(`cb:${this === globalThis}:${x}`);
        return stores.map((als) => als.getStore()).join(',');
      },
      undefined,
      'x'
    );
    strictEqual(log.join(' '), '0:undefined 2:m0 cb:true:x');
    strictEqual(result, 'm0,,m2');
    strictEqual(stores[0].getStore(), undefined);
  },
};
