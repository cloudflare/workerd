// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual, throws, ok } from 'node:assert';
import { mock } from 'node:test';

// Test for the Event and EventTarget standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

export const event = {
  test() {
    // Any value that is not-stringifiable fails
    throws(() => new Event(Symbol('test')));

    // stringifiable values do work, however
    strictEqual(new Event({}).type, '[object Object]');
    strictEqual(new Event(null).type, 'null');
    strictEqual(new Event(1).type, '1');

    // Passing undefined explicitly works
    strictEqual(new Event(undefined).type, 'undefined');

    // But not passing a value for type fails
    throws(() => new Event());

    // We can create an Event object with no options and it works as expected.
    const event = new Event('foo');
    strictEqual(event.type, 'foo');
    strictEqual(event.bubbles, false);
    strictEqual(event.cancelable, false);
    strictEqual(event.composed, false);
    strictEqual(event.isTrusted, false);
    strictEqual(event.defaultPrevented, false);
    strictEqual(event.eventPhase, Event.NONE);
    strictEqual(event.returnValue, true);
    strictEqual(event.timeStamp, 0.0);
    strictEqual(event.cancelBubble, false);
    strictEqual(event.currentTarget, undefined);
    deepStrictEqual(event.composedPath(), []);
    event.stopImmediatePropagation();
    event.stopPropagation();
    event.preventDefault();
    // Even tho we called preventDefault, because the event is not cancelable
    // by default, defaultPrevented still is false.
    strictEqual(event.defaultPrevented, false);
    strictEqual(event.cancelBubble, true);
  },
};

export const eventWithOptions = {
  test() {
    // The options argument must be an object
    throws(() => new Event('foo', 1));
    throws(() => new Event('foo', 'bar'));

    // We can create an Event object with no options and it works as expected.
    const event = new Event('foo', {
      cancelable: true,
      bubbles: 'truthy values work also',
      composed: true,
    });
    strictEqual(event.type, 'foo');
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, true);
    strictEqual(event.composed, true);
    strictEqual(event.isTrusted, false);
    strictEqual(event.defaultPrevented, false);
    strictEqual(event.eventPhase, Event.NONE);
    strictEqual(event.returnValue, true);
    strictEqual(event.timeStamp, 0.0);
    strictEqual(event.cancelBubble, false);
    strictEqual(event.currentTarget, undefined);
    deepStrictEqual(event.composedPath(), []);
    event.stopImmediatePropagation();
    event.stopPropagation();
    event.preventDefault();
    // Because the event is cancelable, defaultPrevented is true.
    strictEqual(event.defaultPrevented, true);
    strictEqual(event.returnValue, false);
  },
};

export const eventSubclass = {
  test() {
    class Foo extends Event {
      constructor() {
        super('foo');
      }
    }
    const event = new Foo();
    strictEqual(event.type, 'foo');
    strictEqual(event.bubbles, false);
    strictEqual(event.cancelable, false);
    strictEqual(event.composed, false);
    strictEqual(event.isTrusted, false);
    strictEqual(event.defaultPrevented, false);
    strictEqual(event.eventPhase, Event.NONE);
    strictEqual(event.returnValue, true);
    strictEqual(event.timeStamp, 0.0);
    strictEqual(event.cancelBubble, false);
    strictEqual(event.currentTarget, undefined);

    // Everything except cancelBubble is read only and will throw
    // if attempts are made to modify
    throws(() => (event.type = 'foo'));
    throws(() => (event.bubbles = false));
    throws(() => (event.cancelable = false));
    throws(() => (event.composed = false));
    throws(() => (event.isTrusted = false));
    throws(() => (event.defaultPrevented = false));
    throws(() => (event.eventPhase = Event.NONE));
    throws(() => (event.returnValue = true));
    throws(() => (event.timeStamp = 0.0));
    throws(() => (event.currentTarget = undefined));
    event.cancelBubble = true;
    strictEqual(event.cancelBubble, true);

    // With the default compatibility flag set, the properties should
    // exist on the prototype and not as own properties on the event itself.
    strictEqual(
      Reflect.getOwnPropertyDescriptor(event, 'cancelable'),
      undefined
    );

    // Which means a subclass can replace the implementation successfully.
    class Bar extends Event {
      constructor() {
        super('bar');
      }
      get bubbles() {
        return 'hello';
      }
    }
    const bar = new Bar();
    strictEqual(bar.bubbles, 'hello');
    strictEqual(bar.composed, false);
  },
};

export const extendableEventNotConstructable = {
  test() {
    // While the spec defines ExtendableEvent to be consructable, we do not support
    // doing so. This is intentional because the only real use case of ExtendableEvent
    // is to allow calling waitUntil, which only works on trusted events which can only
    // originate from the runtime. That is, user code cannot create their own trusted
    // events.
    strictEqual(typeof ExtendableEvent, 'function');
    throws(() => new ExtendableEvent('foo'));
  },
};

export const basicEventTarget = {
  test() {
    const target = new EventTarget();

    const event = new Event('foo');
    strictEqual(event.eventPhase, Event.NONE);
    strictEqual(event.currentTarget, undefined);

    let dispatchCount = 0;

    const handler = (dispatched) => {
      strictEqual(dispatched, event);
      strictEqual(dispatched.eventPhase, Event.AT_TARGET);
      strictEqual(dispatched.currentTarget, target);
      deepStrictEqual(dispatched.composedPath(), [target]);
      dispatchCount++;

      // The event is already being dispatched so can't be again.
      throws(() => target.dispatchEvent(dispatched));
    };

    const handlerObj = {
      handleEvent: handler,
    };

    throws(() => target.addEventListener('foo', {}));
    throws(() => target.addEventListener('foo', 'hello'));
    throws(() => target.addEventListener('foo', []));
    throws(() => target.addEventListener('foo', false));

    // Event listener with no options
    target.addEventListener('foo', handler);

    // Same handler can be attached twice, but is only invoked once.
    target.addEventListener('foo', handler);

    target.addEventListener('foo', handlerObj);

    let classCalled;
    const foo = new (class Foo {
      handleEvent(event) {
        classCalled = true;
      }
    })();
    target.addEventListener('foo', foo);

    target.dispatchEvent(event);

    strictEqual(classCalled, true);
    strictEqual(event.eventPhase, Event.NONE);
    strictEqual(event.currentTarget, target);

    strictEqual(dispatchCount, 2);

    target.removeEventListener('foo', handler);

    target.dispatchEvent(event);

    strictEqual(dispatchCount, 3);
  },
};

export const subclassedEventTarget = {
  test() {
    class MyEventTarget extends EventTarget {}
    const event = new Event('foo');
    const target = new MyEventTarget();
    let dispatchCount = 0;
    target.addEventListener('foo', (dispatched) => {
      strictEqual(dispatched, event);
      dispatchCount++;
    });
    target.dispatchEvent(event);
    strictEqual(dispatchCount, 1);
  },
};

export const onceListener = {
  test() {
    const target = new EventTarget();
    const event = new Event('foo');

    let dispatchCount = 0;

    target.addEventListener(
      'foo',
      () => {
        dispatchCount++;
      },
      { once: true }
    );

    target.dispatchEvent(event);
    target.dispatchEvent(event);

    strictEqual(dispatchCount, 1);
  },
};

export const cancelableListener = {
  test() {
    const target = new EventTarget();
    const event = new Event('foo');

    let dispatchCount = 0;

    const ac = new AbortController();

    target.addEventListener(
      'foo',
      () => {
        dispatchCount++;
      },
      { signal: ac.signal }
    );

    // Passing an already aborted signal just works as expected.
    // No errors are thrown.
    target.addEventListener(
      'foo',
      () => {
        dispatchCount++;
      },
      { signal: AbortSignal.abort() }
    );

    ac.abort();

    target.dispatchEvent(event);

    strictEqual(dispatchCount, 0);
  },
};

export const cancelableListenerWithSelfSignal = {
  test() {
    const controller = new AbortController();
    const { signal } = controller;
    const noop = () => {};

    // Fill typeMap so registering the native abort handler below grows it.
    signal.addEventListener('one', noop);
    signal.addEventListener('two', noop);
    signal.addEventListener('three', noop);

    signal.addEventListener('victim', noop, { signal });
    controller.abort();
  },
};

export const cancelableListenerAbortPropagation = {
  test() {
    // TODO(bug): Cancelable event listeners should be removed by signal even when
    // signal's abort event propagation is stopped. This is a safety measure to
    // prevent certain kinds of memory leaks. We currently do not implement this
    // protection.
    // const controller = new AbortController();
    // const { signal } = controller;
    // signal.addEventListener('abort', (e) => e.stopImmediatePropagation(), { once: true });
    // const et = new EventTarget();
    // et.addEventListener('foo', () => {
    //   console.log('....')
    //   throw new Error('should not be called');
    // }, { signal });
    // controller.abort();
    // et.dispatchEvent(new Event('foo'));
  },
};

export const passiveCaptureListener = {
  test() {
    const target = new EventTarget();
    // capture and passive must be false. We do not support these but
    // we allow them to be set for code portability reasons.
    throws(() => {
      target.addEventListener('foo', () => {}, {
        capture: true,
      });
    });
    throws(() => {
      target.addEventListener('foo', () => {}, true);
    });
    throws(() => {
      target.addEventListener('foo', () => {}, {
        passive: true,
      });
    });
    throws(() => {
      target.removeEventListener('foo', () => {}, {
        capture: true,
      });
    });
    throws(() => {
      target.removeEventListener('foo', () => {}, true);
    });
  },
};

export const globalIsEventTarget = {
  test() {
    // TODO(bug): For some reason, even tho our globalThis does, in fact,
    // extend EventTarget and inherits the dispatchEvent, addEventListener, etc,
    // instanceof does not report that fact correctly. So we'll need to fix this.

    // strictEqual(globalThis instanceof EventTarget);

    strictEqual(typeof globalThis.dispatchEvent, 'function');
    strictEqual(typeof globalThis.addEventListener, 'function');
    strictEqual(typeof globalThis.removeEventListener, 'function');

    const event = new Event('foo');
    let dispatchCount = 0;
    addEventListener(
      'foo',
      () => {
        dispatchCount++;
      },
      { once: true }
    );
    dispatchEvent(event);
    strictEqual(dispatchCount, 1);
  },
};

export const errorInHandler = {
  test() {
    // A throwing event handler must not prevent the remaining handlers from running, nor
    // propagate out of dispatchEvent(); the exception is reported to the global scope via
    // the (cancelable) 'error' event.
    const event = new Event('foo');
    const target = new EventTarget();
    let dispatchCount = 0;
    target.addEventListener('foo', () => {
      dispatchCount++;
      throw new Error('boom');
    });
    target.addEventListener('foo', () => {
      dispatchCount++;
    });

    let reported = null;
    const errorHandler = (errEvent) => {
      reported = errEvent.error;
      // The report is handled; suppress the console fallback.
      errEvent.preventDefault();
    };
    globalThis.addEventListener('error', errorHandler);
    try {
      strictEqual(target.dispatchEvent(event), true);
    } finally {
      globalThis.removeEventListener('error', errorHandler);
    }

    strictEqual(dispatchCount, 2);
    strictEqual(reported?.message, 'boom');
  },
};

export const listenersAddedDuringDispatch = {
  test() {
    // Listeners added while an event is being dispatched do not run for the in-flight
    // event, but do run for subsequent dispatches — including when many are added at once
    // (which historically stressed handler storage mutation during iteration).
    const target = new EventTarget();
    let outer = 0;
    let added = 0;
    target.addEventListener('foo', () => {
      outer++;
      for (let i = 0; i < 16; i++) {
        target.addEventListener('foo', () => added++);
      }
    });
    target.addEventListener('foo', () => outer++);

    target.dispatchEvent(new Event('foo'));
    strictEqual(outer, 2);
    strictEqual(added, 0);

    target.dispatchEvent(new Event('foo'));
    strictEqual(outer, 4);
    strictEqual(added, 16);
  },
};

export const stopImmediatePropagation = {
  test() {
    const event = new Event('foo');
    const target = new EventTarget();
    let dispatchCount = 0;
    target.addEventListener('foo', (event) => {
      dispatchCount++;
      // Calling stopImmediatePropagation should prevent the next listener
      // from being invoked.
      event.stopImmediatePropagation();
    });
    target.addEventListener('foo', (event) => {
      throw new Error('should not have been invoked');
    });
    target.dispatchEvent(event);
    strictEqual(dispatchCount, 1);
  },
};

export const nullUndefinedHandler = {
  test() {
    // TODO(bug): Odd as it may seem, the spec allows passing null and undefined
    // to addEventListener. We currently do not handle these correctly.
    const _target = new EventTarget();
    // target.addEventListener('foo', null);
    // target.addEventListener('foo', undefined);
  },
};

export const customEvent = {
  test() {
    const event = new CustomEvent('foo', { detail: { a: 123 } });
    ok(event instanceof Event);
    strictEqual(event.type, 'foo');
    deepStrictEqual(event.detail, { a: 123 });
  },
};

export const closeEvent = {
  test() {
    // The CloseEvent constructor second argument is optional. Our implementation
    // had it as required. Let's make sure we can create it without the second arg.
    new CloseEvent('foo');
    new CloseEvent('foo', { code: 1000, reason: 'bye' });
  },
};

export const handlerThis = {
  test() {
    const et = new EventTarget();
    const handler = mock.fn(function () {
      strictEqual(this, et);
    });
    et.addEventListener('foo', handler);

    const handlerObject = {
      handleEvent: mock.fn(function () {
        strictEqual(this, handlerObject);
      }),
    };
    et.addEventListener('foo', handlerObject);

    et.dispatchEvent(new Event('foo'));
    strictEqual(handler.mock.callCount(), 1);
    strictEqual(handlerObject.handleEvent.mock.callCount(), 1);
  },
};

export const isTrustedDefaults = {
  async test() {
    // User-constructed events are never trusted...
    const userEvents = [
      new Event('foo'),
      new CustomEvent('foo'),
      new MessageEvent('foo', { data: 'bar' }),
      new ErrorEvent('foo'),
      new CloseEvent('foo'),
    ];
    for (const event of userEvents) {
      strictEqual(event.isTrusted, false);
      // ...including when observed by a listener during dispatch.
      const target = new EventTarget();
      let trusted;
      target.addEventListener('foo', (e) => {
        trusted = e.isTrusted;
      });
      target.dispatchEvent(event);
      strictEqual(trusted, false);
    }

    // Events constructed and dispatched by the runtime are trusted.
    {
      const ac = new AbortController();
      let trusted;
      ac.signal.addEventListener('abort', (e) => {
        trusted = e.isTrusted;
      });
      ac.abort();
      strictEqual(trusted, true);
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      const handler = (e) => resolve(e.isTrusted);
      addEventListener('unhandledrejection', handler);
      Promise.reject(new Error('boom'));
      strictEqual(await promise, true);
      removeEventListener('unhandledrejection', handler);
    }
  },
};

// Under the REPORT dispatch policy, a listener exception is reported to the global scope's
// 'error' event synchronously, between the throwing listener and the next one.
export const reportedListenerErrorInterleaving = {
  test() {
    const order = [];
    const boom = new Error('boom');
    const globalHandler = (event) => {
      order.push('global-error');
      strictEqual(event.error, boom);
    };
    addEventListener('error', globalHandler);
    try {
      const target = new EventTarget();
      target.addEventListener('foo', () => {
        order.push('l1');
        throw boom;
      });
      target.addEventListener('foo', () => order.push('l2'));
      // dispatchEvent() itself must not throw.
      target.dispatchEvent(new Event('foo'));
      deepStrictEqual(order, ['l1', 'global-error', 'l2']);
    } finally {
      removeEventListener('error', globalHandler);
    }
  },
};

// A throwing global 'error' listener must not break the REPORT no-throw contract: the
// nested report is routed to the console (HTML's "in error reporting mode" guard) instead
// of propagating or recursing.
export const throwingGlobalErrorListener = {
  test() {
    const order = [];
    const globalHandler = () => {
      order.push('global-error');
      throw new Error('error handler boom');
    };
    addEventListener('error', globalHandler);
    try {
      // Via a REPORT dispatch on an EventTarget.
      const target = new EventTarget();
      target.addEventListener('foo', () => {
        order.push('l1');
        throw new Error('boom');
      });
      target.addEventListener('foo', () => order.push('l2'));
      target.dispatchEvent(new Event('foo'));
      deepStrictEqual(order, ['l1', 'global-error', 'l2']);

      // Via AbortController.abort(), which the spec forbids from throwing.
      order.length = 0;
      const ac = new AbortController();
      ac.signal.addEventListener('abort', () => {
        order.push('abort1');
        throw new Error('abort boom');
      });
      ac.signal.addEventListener('abort', () => order.push('abort2'));
      ac.abort();
      deepStrictEqual(order, ['abort1', 'global-error', 'abort2']);

      // Via reportError() directly.
      reportError(new Error('reported boom'));
    } finally {
      removeEventListener('error', globalHandler);
    }
  },
};

// User code running during the mid-dispatch report can mutate the original listener list;
// removals are honored for listeners that have not run yet.
export const midReportListenerRemoval = {
  test() {
    const order = [];
    const target = new EventTarget();
    const l2 = () => order.push('l2');
    const globalHandler = () => {
      order.push('global-error');
      target.removeEventListener('foo', l2);
    };
    addEventListener('error', globalHandler);
    try {
      target.addEventListener('foo', () => {
        order.push('l1');
        throw new Error('boom');
      });
      target.addEventListener('foo', l2);
      target.dispatchEvent(new Event('foo'));
      deepStrictEqual(order, ['l1', 'global-error']);
    } finally {
      removeEventListener('error', globalHandler);
    }
  },
};

// A throwing WebSocket 'message' listener has its exception reported and the remaining
// listeners still run, but the WebSocket is still errored out afterwards (fail-fast).
export const webSocketThrowingMessageListener = {
  async test() {
    const order = [];
    const boom = new Error('ws boom');
    const globalHandler = () => order.push('global-error');
    addEventListener('error', globalHandler);
    try {
      const { 0: client, 1: server } = new WebSocketPair();
      client.accept();
      server.accept();

      const errorPromise = new Promise((resolve) => {
        client.addEventListener('error', (event) => resolve(event.error));
      });
      const l2Promise = new Promise((resolve) => {
        client.addEventListener('message', () => {
          order.push('l1');
          throw boom;
        });
        client.addEventListener('message', () => {
          // The fail-fast teardown happens strictly after the dispatch completes: this
          // listener still observes a live, usable WebSocket even though the previous
          // listener threw.
          order.push(`l2:readyState=${client.readyState}`);
          client.send('still-works');
          resolve();
        });
      });

      const serverReceived = new Promise((resolve) => {
        server.addEventListener('message', (event) => resolve(event.data));
      });

      server.send('hello');
      await l2Promise;
      deepStrictEqual(order, [
        'l1',
        'global-error',
        `l2:readyState=${WebSocket.READY_STATE_OPEN}`,
      ]);
      // The send() from the second listener made it out before the teardown.
      strictEqual(await serverReceived, 'still-works');
      // The fail-fast reaction still errors the WebSocket with the listener's exception.
      // The exception crosses the JS/KJ boundary in the read loop and is reconstructed, so
      // only the message survives (as before this dispatch used REPORT).
      ok(String(await errorPromise).includes('ws boom'));
    } finally {
      removeEventListener('error', globalHandler);
    }
  },
};

// The standard MessageEventInit members are all supported (and optional) for
// user-constructed events, with spec defaults.
export const messageEventSpecInit = {
  test() {
    const defaults = new MessageEvent('message');
    strictEqual(defaults.data, null);
    strictEqual(defaults.origin, '');
    strictEqual(defaults.lastEventId, '');
    strictEqual(defaults.source, null);
    deepStrictEqual(defaults.ports, []);
    strictEqual(defaults.bubbles, false);
    strictEqual(defaults.cancelable, false);
    strictEqual(defaults.composed, false);

    const data = { hello: 'world' };
    const event = new MessageEvent('message', {
      data,
      origin: 'https://example.org',
      lastEventId: '42',
      bubbles: true,
      cancelable: true,
      composed: true,
    });
    strictEqual(event.data, data);
    strictEqual(event.origin, 'https://example.org');
    strictEqual(event.lastEventId, '42');
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, true);
    strictEqual(event.composed, true);
    event.preventDefault();
    strictEqual(event.defaultPrevented, true);
  },
};

// CloseEventInit supports the common EventInit members.
export const closeEventSpecInit = {
  test() {
    const defaults = new CloseEvent('close');
    strictEqual(defaults.code, 0);
    strictEqual(defaults.reason, '');
    strictEqual(defaults.wasClean, false);
    strictEqual(defaults.bubbles, false);
    strictEqual(defaults.cancelable, false);
    strictEqual(defaults.composed, false);

    const event = new CloseEvent('close', {
      code: 1000,
      reason: 'done',
      wasClean: true,
      bubbles: true,
      cancelable: true,
      composed: true,
    });
    strictEqual(event.code, 1000);
    strictEqual(event.reason, 'done');
    strictEqual(event.wasClean, true);
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, true);
    strictEqual(event.composed, true);
  },
};

// ErrorEventInit supports the common EventInit members.
export const errorEventSpecInit = {
  test() {
    const defaults = new ErrorEvent('error');
    strictEqual(defaults.message, '');
    strictEqual(defaults.bubbles, false);
    strictEqual(defaults.cancelable, false);
    strictEqual(defaults.composed, false);

    const err = new Error('boom');
    const event = new ErrorEvent('error', {
      message: 'boom',
      filename: 'test.js',
      lineno: 1,
      colno: 2,
      error: err,
      bubbles: true,
      cancelable: true,
      composed: true,
    });
    strictEqual(event.message, 'boom');
    strictEqual(event.filename, 'test.js');
    strictEqual(event.lineno, 1);
    strictEqual(event.colno, 2);
    strictEqual(event.error, err);
    strictEqual(event.bubbles, true);
    strictEqual(event.cancelable, true);
    strictEqual(event.composed, true);
    event.preventDefault();
    strictEqual(event.defaultPrevented, true);
  },
};
