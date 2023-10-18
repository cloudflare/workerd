import {
  deepStrictEqual,
  strictEqual,
  throws,
  ok,
} from 'node:assert';

// Test for the Event and EventTarget standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

export const event = {
  test() {
    // Any value that is not-stringifiable fails
    throws(() => new Event(Symbol('test')));

    // stringifiable values do work, however
    strictEqual((new Event({})).type, '[object Object]');
    strictEqual((new Event(null)).type, 'null');
    strictEqual((new Event(1)).type, '1');

    // Passing undefined explicitly works
    strictEqual((new Event(undefined)).type, 'undefined');

    // But not passing a value for type fails
    throws(() => new Event());

    // We can create an Event object with no options and it works as expected.
    const event = new Event("foo");
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
  }
};

export const eventWithOptions = {
  test() {
    // The options argument must be an object
    throws(() => new Event('foo', 1));
    throws(() => new Event('foo', 'bar'));

    // We can create an Event object with no options and it works as expected.
    const event = new Event("foo", {
      cancelable: true,
      bubbles: 'truthy values work also',
      composed: true
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
  }
};

export const eventSubclass = {
  test() {
    class Foo extends Event {
      constructor() { super("foo"); }
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
    throws(() => event.type = 'foo');
    throws(() => event.bubbles = false);
    throws(() => event.cancelable = false);
    throws(() => event.composed = false);
    throws(() => event.isTrusted = false);
    throws(() => event.defaultPrevented = false);
    throws(() => event.eventPhase = Event.NONE);
    throws(() => event.returnValue = true);
    throws(() => event.timeStamp = 0.0);
    throws(() => event.currentTarget = undefined);
    event.cancelBubble = true;
    strictEqual(event.cancelBubble, true);

    // With the default compatibility flag set, the properties should
    // exist on the prototype and not as own properties on the event itself.
    strictEqual(Reflect.getOwnPropertyDescriptor(event, 'cancelable'), undefined);

    // Which means a subclass can replace the implementation successfully.
    class Bar extends Event {
      constructor() { super('bar'); }
      get bubbles() { return 'hello'; }
    }
    const bar = new Bar();
    strictEqual(bar.bubbles, 'hello');
    strictEqual(bar.composed, false);
  }
};

export const extendableEventNotConstructable = {
  test() {
    // While the spec defines ExtendableEvent to be consructable, we do not support
    // doing so. This is intentional because the only real use case of ExtendableEvent
    // is to allow calling waitUntil, which only works on trusted events which can only
    // originate from the runtime. That is, user code cannot create their own trusted
    // events.
    strictEqual(typeof ExtendableEvent, 'function');
    throws(() => new ExtendableEvent("foo"));
  }
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
      handleEvent: handler
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
    });
    target.addEventListener('foo', foo);

    target.dispatchEvent(event);

    strictEqual(classCalled, true);
    strictEqual(event.eventPhase, Event.NONE);
    strictEqual(event.currentTarget, target);

    strictEqual(dispatchCount, 2);

    target.removeEventListener('foo', handler);

    target.dispatchEvent(event);

    strictEqual(dispatchCount, 3);
  }
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
  }
};

export const onceListener = {
  test() {
    const target = new EventTarget();
    const event = new Event('foo');

    let dispatchCount = 0;

    target.addEventListener('foo', () => {
      dispatchCount++;
    }, { once: true });

    target.dispatchEvent(event);
    target.dispatchEvent(event);

    strictEqual(dispatchCount, 1);
  }
};

export const cancelableListener = {
  test() {
    const target = new EventTarget();
    const event = new Event('foo');

    let dispatchCount = 0;

    const ac = new AbortController();

    target.addEventListener('foo', () => {
      dispatchCount++;
    }, { signal: ac.signal });

    // Passing an already aborted signal just works as expected.
    // No errors are thrown.
    target.addEventListener('foo', () => {
      dispatchCount++;
    }, { signal: AbortSignal.abort() });

    ac.abort();

    target.dispatchEvent(event);

    strictEqual(dispatchCount, 0);
  }
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
  }
};

export const passiveCaptureListener = {
  test() {
    const target = new EventTarget();
    // capture and passive must be false. We do not support these but
    // we allow them to be set for code portability reasons.
    throws(() => {
      target.addEventListener('foo', () => {}, {
        capture: true
      });
    });
    throws(() => {
      target.addEventListener('foo', () => {}, true);
    });
    throws(() => {
      target.addEventListener('foo', () => {}, {
        passive: true
      });
    });
    throws(() => {
      target.removeEventListener('foo', () => {}, {
        capture: true
      });
    });
    throws(() => {
      target.removeEventListener('foo', () => {}, true);
    });
  }
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
    addEventListener('foo', () => {
      dispatchCount++;
    }, { once: true });
    dispatchEvent(event);
    strictEqual(dispatchCount, 1);
  }
};

export const errorInHandler = {
  test() {
    // TODO(bug): Erroring in one event handler should not prevent others from being
    // run but we currently do not implement this correctly.
    const event = new Event('foo');
    const target = new EventTarget();
    let dispatchCount = 0;
    target.addEventListener('foo', () => {
      dispatchCount++;
      throw new Error('boom');
    })
    target.addEventListener('foo', () => {
      dispatchCount++;
    });

    throws(() => target.dispatchEvent(event));

    // The dispatchCount here should be 2, but with the current bug, it's only 1
    // strictEqual(dispatchCount, 2);
    strictEqual(dispatchCount, 1);
  }
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
  }
};

export const nullUndefinedHandler = {
  test() {
    // TODO(bug): Odd as it may seem, the spec allows passing null and undefined
    // to addEventListener. We currently do not handle these correctly.
    const target = new EventTarget();
    // target.addEventListener('foo', null);
    // target.addEventListener('foo', undefined);
  }
};

export const customEvent = {
  test() {
    const event = new CustomEvent('foo', { detail: { a: 123 } });
    ok(event instanceof Event);
    strictEqual(event.type, 'foo');
    deepStrictEqual(event.detail, { a: 123 });
  }
};
