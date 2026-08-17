// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual } from 'node:assert';

// Tests for `on<type>` event handler attributes on EventTarget and its subclasses.
// See https://github.com/cloudflare/workerd/issues/6022
//
// Historically, EventTarget itself looked for an `on<type>` property on the object
// whenever an event was dispatched, and invoked it before any listener registered
// with addEventListener(). That is not what the standard says: only specific
// interfaces define `on<type>` accessors, and those accessors register a regular
// listener, so they fire in registration order along with everything else.
//
// The spec_compliant_event_handler_attributes compat flag switches to the standard
// behavior. Every test below runs twice, once per behavior, with env.MODE telling
// us which one to expect.

function isSpecCompliant(env) {
  strictEqual(typeof env.MODE, 'string');
  return env.MODE === 'spec';
}

export const plainEventTargetIgnoresOnProperties = {
  test(ctrl, env) {
    const calls = [];
    const target = new EventTarget();
    target.addEventListener('hit', () => {
      calls.push('a');
    });
    target.onhit = () => {
      calls.push('on');
    };
    target.addEventListener('hit', () => {
      calls.push('c');
    });
    target.dispatchEvent(new Event('hit'));

    // A plain EventTarget defines no event handler attributes, so `onhit` is just
    // an ordinary property that nothing ever reads.
    deepStrictEqual(
      calls,
      isSpecCompliant(env) ? ['a', 'c'] : ['on', 'a', 'c']
    );
  },
};

export const subclassDefinedOnPropertyFiresOnceInOrder = {
  test(ctrl, env) {
    const calls = [];
    const target = new EventTarget();

    // This is how a subclass is expected to implement an event handler attribute.
    let current = null;
    Object.defineProperty(target, 'onhit', {
      get: () => current,
      set(value) {
        if (current) this.removeEventListener('hit', current);
        current = value;
        if (current) this.addEventListener('hit', current);
      },
    });

    target.addEventListener('hit', () => {
      calls.push('a');
    });
    target.onhit = () => {
      calls.push('b');
    };
    target.addEventListener('hit', () => {
      calls.push('c');
    });
    target.dispatchEvent(new Event('hit'));

    // Without the flag the handler is invoked twice: once by EventTarget's own
    // `on<type>` lookup and once as the listener the subclass registered.
    deepStrictEqual(
      calls,
      isSpecCompliant(env) ? ['a', 'b', 'c'] : ['b', 'a', 'b', 'c']
    );
  },
};

export const abortSignalOnAbortFiresInRegistrationOrder = {
  test(ctrl, env) {
    const calls = [];
    const ac = new AbortController();
    ac.signal.addEventListener('abort', () => {
      calls.push('a');
    });
    const handler = () => {
      calls.push('on');
    };
    ac.signal.onabort = handler;
    strictEqual(ac.signal.onabort, handler);
    ac.signal.addEventListener('abort', () => {
      calls.push('c');
    });
    ac.abort();

    deepStrictEqual(
      calls,
      isSpecCompliant(env) ? ['a', 'on', 'c'] : ['on', 'a', 'c']
    );
  },
};

export const abortSignalOnAbortKeepsItsPositionWhenReassigned = {
  test(ctrl, env) {
    const calls = [];
    const ac = new AbortController();
    ac.signal.addEventListener('abort', () => {
      calls.push('a');
    });
    ac.signal.onabort = () => {
      calls.push('first');
    };
    ac.signal.addEventListener('abort', () => {
      calls.push('c');
    });
    // Replacing the handler must not move it to the end of the listener list.
    ac.signal.onabort = () => {
      calls.push('second');
    };
    ac.abort();

    deepStrictEqual(
      calls,
      isSpecCompliant(env) ? ['a', 'second', 'c'] : ['second', 'a', 'c']
    );
  },
};

export const abortSignalOnAbortCanBeCleared = {
  test(ctrl, env) {
    const calls = [];
    const ac = new AbortController();
    ac.signal.addEventListener('abort', () => {
      calls.push('a');
    });
    ac.signal.onabort = () => {
      calls.push('on');
    };
    ac.signal.addEventListener('abort', () => {
      calls.push('c');
    });
    ac.signal.onabort = null;
    strictEqual(ac.signal.onabort, null);
    ac.abort();

    deepStrictEqual(calls, ['a', 'c']);
  },
};

export const abortSignalOnAbortIgnoresNonCallables = {
  test(ctrl, env) {
    const calls = [];
    const ac = new AbortController();
    ac.signal.addEventListener('abort', () => {
      calls.push('a');
    });

    // Primitives are coerced to null, ...
    for (const value of [1, 'a', true, Symbol('test')]) {
      ac.signal.onabort = value;
      strictEqual(ac.signal.onabort, null);
    }

    // ... while a non-callable object is retained but never invoked.
    const obj = {};
    ac.signal.onabort = obj;
    strictEqual(ac.signal.onabort, obj);

    ac.abort();
    deepStrictEqual(calls, ['a']);
  },
};

export const abortSignalOnAbortReceivesTheEvent = {
  test(ctrl, env) {
    const seen = [];
    const ac = new AbortController();
    ac.signal.onabort = function (event) {
      seen.push([event.type, this === ac.signal]);
    };
    ac.abort();
    // Per the standard, `this` inside the handler is the object it was set on. The legacy
    // `on<type>` lookup never bound a receiver, so there `this` is the global object.
    deepStrictEqual(seen, [['abort', isSpecCompliant(env)]]);
  },
};

export const webSocketOnMessageFiresInRegistrationOrder = {
  async test(ctrl, env) {
    const calls = [];
    const [client, server] = new WebSocketPair();
    server.accept();

    const { promise, resolve } = Promise.withResolvers();
    server.addEventListener('message', (event) => {
      calls.push('a:' + event.data);
    });
    const handler = (event) => {
      calls.push('on:' + event.data);
    };
    server.onmessage = handler;
    strictEqual(server.onmessage, handler);
    server.addEventListener('message', (event) => {
      calls.push('c:' + event.data);
      resolve();
    });

    client.accept();
    client.send('hi');
    await promise;

    deepStrictEqual(
      calls,
      isSpecCompliant(env)
        ? ['a:hi', 'on:hi', 'c:hi']
        : ['on:hi', 'a:hi', 'c:hi']
    );

    client.close();
    server.close();
  },
};

export const webSocketOnCloseCanBeCleared = {
  async test(ctrl, env) {
    const calls = [];
    const [client, server] = new WebSocketPair();
    server.accept();

    const { promise, resolve } = Promise.withResolvers();
    server.onclose = () => {
      calls.push('on');
    };
    server.onclose = null;
    strictEqual(server.onclose, null);
    server.addEventListener('close', () => {
      calls.push('listener');
      resolve();
    });

    client.accept();
    client.close(1000, 'done');
    await promise;

    deepStrictEqual(calls, ['listener']);

    server.close();
  },
};

export const onHandlerReturningFalseCancelsTheEvent = {
  test(ctrl, env) {
    const spec = isSpecCompliant(env);
    const signal = new AbortController().signal;
    signal.onabort = () => false;

    const event = new Event('abort', { cancelable: true });
    // Per the standard's event handler processing algorithm, returning false cancels the event.
    // The legacy `on<type>` lookup ignores false, because it applies the same rule to handlers
    // as to listeners: only returning true cancels.
    strictEqual(signal.dispatchEvent(event), !spec);
    strictEqual(event.defaultPrevented, spec);
  },
};

export const onHandlerReturningTrueIsIgnored = {
  test(ctrl, env) {
    const spec = isSpecCompliant(env);
    const signal = new AbortController().signal;
    signal.onabort = () => true;

    const event = new Event('abort', { cancelable: true });
    // True has no meaning in the standard's algorithm.
    strictEqual(signal.dispatchEvent(event), spec);
    strictEqual(event.defaultPrevented, !spec);
  },
};

export const onHandlerCannotCancelANonCancelableEvent = {
  test(ctrl, env) {
    const signal = new AbortController().signal;
    signal.onabort = () => false;

    // Cancelling is a no-op on an event that is not cancelable, in either mode.
    const event = new Event('abort');
    strictEqual(signal.dispatchEvent(event), true);
    strictEqual(event.defaultPrevented, false);
  },
};

export const listenerReturningTrueStillCancels = {
  test(ctrl, env) {
    // Listeners are not supposed to return anything, but a listener that returns true has always
    // been treated as calling preventDefault(), and the flag does not change that.
    const target = new EventTarget();
    target.addEventListener('hit', () => true);

    const event = new Event('hit', { cancelable: true });
    strictEqual(target.dispatchEvent(event), false);
    strictEqual(event.defaultPrevented, true);
  },
};
