import {
  strictEqual,
  ok,
  throws
} from 'node:assert';

// Test for the AbortSignal and AbortController standard Web API implementations.
// The implementation for these are in api/basics.{h|c++}

export const abortcontroller = {
  test() {
    // AbortSignal is not directly creatable
    throws(() => new AbortSignal);

    const ac = new AbortController();
    ok(ac.signal instanceof AbortSignal);
    strictEqual(ac.signal.aborted, false);

    // every call to ac.signal should always be the same value.
    strictEqual(ac.signal, ac.signal);

    // signal is read only
    throws(() => ac.signal = 1);

    let invoked = 0;
    ac.signal.onabort = (event) => {
      invoked++;
      strictEqual(event.type, 'abort');
    };

    // Will not throw because the signal is not aborted
    ac.signal.throwIfAborted();

    // reason and aborted are read only
    throws(() => ac.signal.reason = 1);
    throws(() => ac.signal.aborted = 'foo');

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
  }
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
  }
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
  }
};

export const timedAbort = {
  async test() {
    const timed = AbortSignal.timeout(100);
    let resolve;
    const promise = new Promise(r => resolve = r);
    let invoked = 0;
    timed.onabort = () => {
      invoked++;
      resolve();
    };
    await promise;
    strictEqual(invoked, 1);
  }
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
  }
};

export const anyAbort2 = {
  async test() {
    const timed = AbortSignal.timeout(100);
    const ac = new AbortController();
    const any = AbortSignal.any([timed, ac.signal]);

    let invoked = 0;
    let resolve;
    const promise = new Promise(r => resolve = r);

    any.onabort = () => {
      invoked++;
      resolve();
    };

    await promise;

    strictEqual(invoked, 1);
  }
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
  }
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
      // TODO(soon): For now, we are relaxing this check and will log a warning
      // if the value is not a function or object. If we get no hits on that warning,
      // we can return to checking for null here.
      //strictEqual(ac.signal.onabort, null);
      strictEqual(ac.signal.onabort, v);
    });

    const handler = {};
    ac.signal.onabort = handler;
    strictEqual(ac.signal.onabort, handler);
  }
};
